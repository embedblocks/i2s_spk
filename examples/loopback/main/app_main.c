/*
 * i2s_mic + i2s_spk loopback example — record with the microphone, then
 * play the recording back through the speaker. No PC, no transport
 * protocol — everything happens entirely on-device.
 *
 * This copy of the example lives inside the i2s_spk repository and pulls
 * in i2s_mic from the ESP Component Registry (see main/idf_component.yml).
 * An identical copy lives inside the i2s_mic repository
 * (examples/loopback), pulling in i2s_spk from the registry instead — the
 * two are intentionally the same demo, cross-referenced from both
 * component's own example sets, matching how each component's README
 * points at "an end-to-end example" without requiring a reader to already
 * have both repositories checked out side by side.
 *
 * ============================================================================
 * WHY RECORD-THEN-PLAYBACK, NOT LIVE/CONTINUOUS LOOPBACK
 * ============================================================================
 * The obvious version of this demo would run i2s_mic and i2s_spk
 * simultaneously — capture a buffer, immediately play it, repeat — for a
 * true live loopback. That's possible on chips with two independent I2S
 * peripherals (ESP32, ESP32-S3): put i2s_mic on I2S_NUM_0 and i2s_spk on
 * I2S_NUM_1, and they never contend for anything.
 *
 * It is NOT possible on ESP32-C3 (or any chip with SOC_I2S_NUM == 1). Both
 * i2s_mic_init() and i2s_spk_init() independently call ESP-IDF's
 * i2s_new_channel(), which claims a physical I2S controller for as long as
 * the component stays INITIALIZED — not just while it's RUNNING. On a
 * single-controller chip, whichever of the two components initializes
 * first gets the only available controller, and the other's init() call
 * fails outright (there's nothing left to allocate), regardless of whether
 * the first one is actively running or merely sitting idle in the
 * INITIALIZED state. This is a hardware/singleton-design constraint, not a
 * bug in either component — see i2s_spk's README ("Chip Support") and the
 * design document's Section 10 for the same limitation stated from each
 * component's own side.
 *
 * Since this example is meant to build and run identically on both ESP32
 * and ESP32-C3 from one shared source file (only sdkconfig.defaults.<target>
 * differs — see that file for GPIO wiring per target), it's written to
 * only ever have ONE of the two components INITIALIZED at a time:
 *
 *   1. RECORD phase: i2s_mic_init() -> i2s_mic_start() -> capture N seconds
 *      into a RAM buffer -> i2s_mic_stop() -> i2s_mic_deinit() (fully
 *      releases the I2S controller)
 *   2. PLAYBACK phase: i2s_spk_init() -> i2s_spk_start() -> play the whole
 *      RAM buffer -> i2s_spk_stop() -> i2s_spk_deinit()
 *   3. repeat
 *
 * This costs a fixed record/playback delay instead of true real-time
 * loopback, but it's the only version of this demo that's actually correct
 * on every chip both components claim to support — including the
 * single-I2S-peripheral ones. If you specifically have a 2-I2S-peripheral
 * chip and want true live loopback, that's a different (and simpler!)
 * example: skip the RAM buffer, run both components RUNNING at once on
 * separate ports, and pipe each captured buffer straight into
 * i2s_spk_send_buffer() as it arrives.
 *
 * ============================================================================
 * MIC CAPTURE: STEREO WORKAROUND AND SAMPLE EXTRACTION
 * ============================================================================
 * Reused verbatim from i2s_mic's own streaming examples: on at least one
 * tested ESP32-C3 / ESP-IDF combination, requesting I2S_SLOT_MODE_MONO for
 * RX did not produce a clean single-slot-per-frame stream, so this example
 * requests STEREO explicitly and discards the unwanted interleaved slot
 * (KEEP_SLOT below) while downconverting INMP441's 32-bit samples to
 * 16-bit. See i2s_mic's own examples for the full diagnosis. If your
 * recording comes out silent or hum-only, try flipping KEEP_SLOT from 0 to
 * 1, or the INMP441's L/R pin from GND to VDD.
 *
 * Wiring:
 *   INMP441 SCK  -> LOOPBACK_MIC_GPIO_BCK   (see sdkconfig.defaults.<target>)
 *   INMP441 WS   -> LOOPBACK_MIC_GPIO_WS
 *   INMP441 SD   -> LOOPBACK_MIC_GPIO_DATA
 *   INMP441 L/R  -> GND
 *   INMP441 VDD  -> 3.3V
 *   INMP441 GND  -> GND
 *
 *   MAX98357A BCLK -> LOOPBACK_SPK_GPIO_BCK
 *   MAX98357A LRC  -> LOOPBACK_SPK_GPIO_WS
 *   MAX98357A DIN  -> LOOPBACK_SPK_GPIO_DATA
 *   MAX98357A SD   -> floating (mono mixdown)
 *   MAX98357A GAIN -> floating
 *   MAX98357A VIN  -> 5V (or 3.3V)
 *   MAX98357A GND  -> GND
 *
 * Both devices are wired at once, permanently — only the *logical* I2S
 * channel toggles between them in firmware; nothing physical needs to be
 * moved between the record and playback phases.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "i2s_mic.h"
#include "i2s_spk.h"

#define TAG   "LOOPBACK"

/* ---- Mic-side capture configuration (see the big comment above for why
 * this requests stereo for a single mono microphone). ---------------- */
#define SAMPLE_RATE           16000
#define MIC_BITS_PER_SAMPLE   32
#define MIC_CHANNEL_COUNT     2
#define KEEP_SLOT             0

#define NUM_BUFS        4
#define BUF_FRAMES      480    /* stereo frames per DMA buffer */
#define BUF_BYTES       (BUF_FRAMES * MIC_CHANNEL_COUNT * (MIC_BITS_PER_SAMPLE / 8))

/* ---- Recording buffer (RAM, 16-bit mono, post-conversion) ------------- */
#define RECORD_SAMPLES     ((size_t)CONFIG_LOOPBACK_RECORD_SECONDS * SAMPLE_RATE)
#define RECORD_BUF_BYTES   (RECORD_SAMPLES * sizeof(int16_t))

/* ---- Playback chunking (independent of any DMA sizing — see i2s_spk.h's
 * note that send_buffer() callers may use any buffer length). ---------- */
#define SPK_CHUNK_BYTES   4096

/* ---- Buffer pool + inter-task handoff (same shape as i2s_mic's own
 * examples — see their app_main.c for the full invariant explanation). */
static uint8_t s_bufs[NUM_BUFS][BUF_BYTES];

typedef struct {
    uint8_t *buf;
    size_t   bytes_read;
} filled_item_t;

static QueueHandle_t s_free_q;
static QueueHandle_t s_filled_q;

static volatile uint32_t s_last_overflow_count = 0;
static volatile uint32_t s_last_no_buffer_count = 0;

/* ---- Recording-phase state, owned by audio_capture_task while active and
 * by app_main()/run_record_phase() while inactive — see run_record_phase()
 * for why it's safe to hand ownership back and forth this way without
 * extra locking. */
static int16_t *s_record_buf;
static volatile bool s_recording_active;
static volatile size_t s_record_write_pos;
static SemaphoreHandle_t s_record_done_sem;

/* ------------------------------------------------------------------------
 * i2s_mic callbacks (ISR context — see i2s_mic.h's rules before editing)
 * ---------------------------------------------------------------------- */

static void IRAM_ATTR mic_ready_cb(uint8_t *buffer, size_t bytes_read, void *user_ctx)
{
    (void)user_ctx;
    BaseType_t hp_woken = pdFALSE;

    filled_item_t item = { .buf = buffer, .bytes_read = bytes_read };
    xQueueSendFromISR(s_filled_q, &item, &hp_woken);

    uint8_t *free_buf;
    if (xQueueReceiveFromISR(s_free_q, &free_buf, &hp_woken) == pdTRUE) {
        i2s_mic_request_buffer(free_buf, BUF_BYTES);
    }

    portYIELD_FROM_ISR(hp_woken);
}

static void mic_overflow_cb(uint32_t total_overflow_count, uint32_t total_no_buffer_count, void *user_ctx)
{
    (void)user_ctx;
    s_last_overflow_count = total_overflow_count;
    s_last_no_buffer_count = total_no_buffer_count;
}

/* ------------------------------------------------------------------------
 * audio_capture_task — copies captured audio into the recording buffer
 * instead of transmitting it anywhere (that's the only real difference
 * from i2s_mic's own streaming examples' equivalent task).
 * ---------------------------------------------------------------------- */

static void audio_capture_task(void *arg)
{
    (void)arg;
    filled_item_t item;

    while (1) {
        if (xQueueReceive(s_filled_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (s_recording_active) {
            int32_t *src = (int32_t *)item.buf;
            size_t num_frames = item.bytes_read / (MIC_CHANNEL_COUNT * sizeof(int32_t));
            size_t remaining = RECORD_SAMPLES - s_record_write_pos;
            size_t to_copy = (num_frames < remaining) ? num_frames : remaining;

            for (size_t i = 0; i < to_copy; i++) {
                s_record_buf[s_record_write_pos + i] =
                    (int16_t)(src[MIC_CHANNEL_COUNT * i + KEEP_SLOT] >> 16);
            }
            s_record_write_pos += to_copy;

            if (s_record_write_pos >= RECORD_SAMPLES) {
                s_recording_active = false;
                xSemaphoreGive(s_record_done_sem);
            }
        }

        /* Whether or not we copied from it, item.buf is ours to reuse the
         * moment we're done reading it — return it to the pool. */
        xQueueSend(s_free_q, &item.buf, portMAX_DELAY);
    }
}

static void overflow_report_task(void *arg)
{
    (void)arg;
    uint32_t last_ovf = 0, last_nobuf = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t ovf = s_last_overflow_count;
        uint32_t nobuf = s_last_no_buffer_count;
        if (ovf != last_ovf || nobuf != last_nobuf) {
            ESP_LOGW(TAG, "drops so far this recording: dma_overflow=%u no_buffer=%u", ovf, nobuf);
            last_ovf = ovf;
            last_nobuf = nobuf;
        }
    }
}

/* ------------------------------------------------------------------------
 * Record phase
 * ---------------------------------------------------------------------- */

static void run_record_phase(void)
{
    ESP_LOGI(TAG, "Recording %d s...", CONFIG_LOOPBACK_RECORD_SECONDS);

    s_record_write_pos = 0;
    s_last_overflow_count = 0;
    s_last_no_buffer_count = 0;

    /* i2s_mic_stop()'s documented contract discards any buffer that was
     * posted but not yet filled at stop time (no callback fires for it —
     * see i2s_mic.h). That means, after a previous cycle's stop(), some
     * buffers may not have made it back into s_free_q. Rather than track
     * that, we just reset both queues and re-post every buffer in
     * s_bufs[] directly, exactly as i2s_mic's own examples do once at
     * boot — this makes each recording phase start from a known-good
     * state regardless of how the previous one ended. */
    xQueueReset(s_free_q);
    xQueueReset(s_filled_q);
    xSemaphoreTake(s_record_done_sem, 0);   /* defensively drain; expected to be empty */

    i2s_mic_config_t mic_cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
        .channel_count = MIC_CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .gpio_bck = CONFIG_LOOPBACK_MIC_GPIO_BCK,
        .gpio_ws = CONFIG_LOOPBACK_MIC_GPIO_WS,
        .gpio_data = CONFIG_LOOPBACK_MIC_GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = 10,
        .dma_buffer_size = BUF_BYTES,
        .max_pending_buffers = NUM_BUFS,
        .cb = mic_ready_cb,
        .overflow_cb = mic_overflow_cb,
        .user_ctx = NULL,
    };

    /* See the file header comment for why i2s_spk is guaranteed NOT
     * INITIALIZED at this point (run_playback_phase() always deinits it
     * before returning) — that's what makes this init() safe on
     * single-I2S-peripheral chips. */
    ESP_ERROR_CHECK(i2s_mic_init(&mic_cfg));

    s_recording_active = true;
    for (int i = 0; i < NUM_BUFS; i++) {
        ESP_ERROR_CHECK(i2s_mic_request_buffer(s_bufs[i], BUF_BYTES));
    }
    ESP_ERROR_CHECK(i2s_mic_start());

    xSemaphoreTake(s_record_done_sem, portMAX_DELAY);

    ESP_ERROR_CHECK(i2s_mic_stop());
    ESP_ERROR_CHECK(i2s_mic_deinit());

    ESP_LOGI(TAG, "Recording complete: %u samples, drops: dma_overflow=%u no_buffer=%u",
             (unsigned)s_record_write_pos, s_last_overflow_count, s_last_no_buffer_count);
}

/* ------------------------------------------------------------------------
 * Playback phase
 * ---------------------------------------------------------------------- */

/* Duplicate each mono 16-bit sample into both L and R slots of an
 * interleaved stereo buffer.
 *
 * Why this exists: playing back through i2s_spk configured for MONO was
 * found to produce audio on only ONE physical DAC/amp output channel on
 * classic ESP32 (the other stayed silent); the same configuration plays
 * correctly on both channels on ESP32-C3. This is a real difference in
 * how the two chips' I2S TX hardware handles a MONO slot_mode request,
 * not a bug in i2s_spk — it's the TX-side counterpart to the RX-side mono
 * ambiguity i2s_mic's own examples already document and work around (see
 * the KEEP_SLOT/stereo-capture comment at the top of this file). Since
 * this example targets both chip families from one shared source file,
 * it sidesteps the ambiguity the same way: request STEREO explicitly and
 * put the same sample in both slots, rather than depend on which chips
 * currently happen to handle MONO correctly. */
static void upmix_mono_to_stereo(const uint8_t *mono_bytes, size_t mono_len, uint8_t *stereo_bytes)
{
    const int16_t *src = (const int16_t *)mono_bytes;
    int16_t *dst = (int16_t *)stereo_bytes;
    size_t num_samples = mono_len / sizeof(int16_t);
    for (size_t i = 0; i < num_samples; i++) {
        dst[2 * i]     = src[i];
        dst[2 * i + 1] = src[i];
    }
}

static void run_playback_phase(void)
{
    ESP_LOGI(TAG, "Playing back...");

    i2s_spk_config_t spk_cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel_count = 2,
        .slot_mode = I2S_SLOT_MODE_STEREO,   /* see upmix_mono_to_stereo()'s comment for why */
        .gpio_bck = CONFIG_LOOPBACK_SPK_GPIO_BCK,
        .gpio_ws = CONFIG_LOOPBACK_SPK_GPIO_WS,
        .gpio_data = CONFIG_LOOPBACK_SPK_GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = 6,
        .dma_buffer_size = 2048,
        .write_timeout_ms = I2S_SPK_WAIT_FOREVER,
        .user_ctx = NULL,
    };

    /* i2s_mic is guaranteed fully deinited by run_record_phase() before we
     * get here — see the file header comment. */
    ESP_ERROR_CHECK(i2s_spk_init(&spk_cfg));
    ESP_ERROR_CHECK(i2s_spk_start());

    uint8_t *pcm_bytes = (uint8_t *)s_record_buf;
    size_t total_bytes = s_record_write_pos * sizeof(int16_t);
    size_t offset = 0;

    /* Upmixed one SPK_CHUNK_BYTES window at a time, rather than doubling
     * the whole recording buffer up front — keeps the extra RAM cost
     * fixed and small regardless of CONFIG_LOOPBACK_RECORD_SECONDS. */
    static uint8_t s_stereo_chunk_buf[SPK_CHUNK_BYTES * 2];

    while (offset < total_bytes) {
        size_t want = total_bytes - offset;
        if (want > SPK_CHUNK_BYTES) {
            want = SPK_CHUNK_BYTES;
        }

        upmix_mono_to_stereo(pcm_bytes + offset, want, s_stereo_chunk_buf);
        size_t stereo_len = want * 2;

        size_t sent = 0;
        while (sent < stereo_len) {
            size_t this_sent = 0;
            esp_err_t ret = i2s_spk_send_buffer(s_stereo_chunk_buf + sent, stereo_len - sent, &this_sent);
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "i2s_spk_send_buffer failed: %s", esp_err_to_name(ret));
                offset = total_bytes;   /* stop the outer loop too */
                break;
            }
            sent += this_sent;
            if (this_sent == 0) {
                break;
            }
        }
        offset += want;
    }

    /* Read before stop(): the queued tail is still playing, so this counts
     * only gaps during playback, not the idle time after the last write. */
    uint32_t underruns = i2s_spk_get_underrun_count();

    ESP_ERROR_CHECK(i2s_spk_stop());
    ESP_ERROR_CHECK(i2s_spk_deinit());

    ESP_LOGI(TAG, "Playback complete (%u DMA underruns)", (unsigned)underruns);
}

/* ------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    s_free_q = xQueueCreate(NUM_BUFS, sizeof(uint8_t *));
    s_filled_q = xQueueCreate(NUM_BUFS, sizeof(filled_item_t));
    s_record_done_sem = xSemaphoreCreateBinary();

    s_record_buf = (int16_t *)malloc(RECORD_BUF_BYTES);
    if (s_record_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte record buffer — lower "
                 "CONFIG_LOOPBACK_RECORD_SECONDS via menuconfig", (unsigned)RECORD_BUF_BYTES);
        return;
    }
    ESP_LOGI(TAG, "Record buffer: %d s (%u bytes)",
             CONFIG_LOOPBACK_RECORD_SECONDS, (unsigned)RECORD_BUF_BYTES);

    xTaskCreate(audio_capture_task, "audio_capture", 4096, NULL, 5, NULL);
    xTaskCreate(overflow_report_task, "ovf_report", 2048, NULL, 1, NULL);

    while (1) {
        run_record_phase();
        run_playback_phase();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

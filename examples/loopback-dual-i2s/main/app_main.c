/*
 * i2s_mic + i2s_spk LIVE loopback example — for chips with TWO independent
 * I2S peripherals (ESP32, ESP32-S3). Captures audio with i2s_mic and
 * relays it straight to i2s_spk, continuously, in real time. No PC, no
 * RAM recording buffer, no repeated init/deinit cycling.
 *
 * This copy of the example lives inside the i2s_spk repository and pulls
 * in i2s_mic from the ESP Component Registry (see main/idf_component.yml).
 * An identical copy lives inside the i2s_mic repository
 * (examples/loopback-dual-i2s), pulling in i2s_spk from the registry
 * instead — the two are intentionally the same demo, cross-referenced
 * from both components' own example sets.
 *
 * ============================================================================
 * WHY THIS EXAMPLE EXISTS SEPARATELY FROM examples/loopback-esp32c3
 * ============================================================================
 * examples/loopback-esp32c3 (in both repositories) uses a record-then-
 * playback structure: capture N seconds into RAM, fully release i2s_mic,
 * initialize i2s_spk, play the RAM buffer back, fully release i2s_spk,
 * repeat. That structure exists ONLY because chips with a single I2S
 * peripheral (SOC_I2S_NUM == 1 — ESP32-C3, ESP32-C6) cannot have i2s_mic
 * and i2s_spk both INITIALIZED at the same time: each independently calls
 * ESP-IDF's i2s_new_channel(), which claims a physical I2S controller for
 * as long as the component stays INITIALIZED, not just while running — on
 * a single-controller chip, whichever initializes first gets the only
 * available controller and the other's init() call fails outright.
 *
 * ESP32 and ESP32-S3 don't have that constraint — they each have TWO
 * independent I2S peripherals. There is no reason to pay the cost of a RAM
 * buffer or repeated teardown/bringup on these chips, and doing so
 * anyway turned out to have its own real cost: testing showed the
 * loopback-esp32c3 structure's repeated i2s_mic_init()/deinit() cycling
 * produces a large, suspiciously constant number of DMA-level overflow
 * events on classic ESP32 (tens of drops, at a nearly identical count
 * every single cycle — consistent with a fixed cost around channel
 * bring-up/teardown rather than an ongoing throughput problem), which
 * i2s_mic's own single-init streaming examples do not exhibit at all. That
 * issue is still being tracked down on the loopback-esp32c3 side — see
 * this repository's other example's README — but it's avoided here
 * entirely, simply by never doing that init/deinit cycling in the first
 * place: this example initializes both components exactly ONCE, at boot,
 * and never tears either down again.
 *
 * Structure:
 *   - i2s_mic on I2S_NUM_0, i2s_spk on I2S_NUM_1, both INITIALIZED and
 *     RUNNING simultaneously, for the entire lifetime of the program.
 *   - i2s_mic's ISR callback hands each captured buffer to a task, which
 *     converts it (32-bit stereo mic capture -> 16-bit mono -> upmixed
 *     16-bit stereo for the speaker — see the two comment blocks below for
 *     why each conversion exists) and immediately calls the blocking
 *     i2s_spk_send_buffer() on it, then returns the mic buffer to the pool.
 *   - No RAM recording buffer, no completion semaphore, no record/playback
 *     phase distinction — just a continuous relay loop paced by real
 *     playback time (i2s_spk_send_buffer() itself is the pacing, exactly
 *     as in i2s_spk's own streaming examples).
 *
 * Expect a genuinely live loopback: roughly one relay-buffer's worth of
 * latency (a few tens of milliseconds, set by BUF_FRAMES below), not the
 * multi-second record/playback delay of loopback-esp32c3.
 *
 * ============================================================================
 * MIC CAPTURE: STEREO WORKAROUND AND SAMPLE EXTRACTION
 * ============================================================================
 * Reused verbatim from i2s_mic's own streaming examples: on at least one
 * tested ESP32-C3 / ESP-IDF combination, requesting I2S_SLOT_MODE_MONO for
 * RX did not produce a clean single-slot-per-frame stream, so this example
 * requests STEREO explicitly and discards the unwanted interleaved slot
 * (KEEP_SLOT below) while downconverting INMP441's 32-bit samples to
 * 16-bit. If your relayed audio comes out silent or hum-only, try flipping
 * KEEP_SLOT from 0 to 1, or the INMP441's L/R pin from GND to VDD.
 *
 * ============================================================================
 * SPEAKER PLAYBACK: WHY STEREO, NOT MONO
 * ============================================================================
 * i2s_spk is configured STEREO here (with each mono sample duplicated into
 * both slots via upmix_mono_to_stereo()) rather than MONO, because
 * requesting MONO directly from i2s_spk was found, on classic ESP32, to
 * produce audio on only ONE physical DAC/amp output channel — a
 * chip-family difference in how ESP32's I2S TX hardware handles a MONO
 * slot request compared to ESP32-C3's, not a bug in i2s_spk itself (which
 * passes slot_mode straight through to the driver unchanged). See
 * i2s_spk's README, "Notes," for the full writeup. Since ESP32 is one of
 * this example's two supported targets, the fix is applied here too.
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
 *   MAX98357A SD   -> floating
 *   MAX98357A GAIN -> floating
 *   MAX98357A VIN  -> 5V (or 3.3V)
 *   MAX98357A GND  -> GND
 *
 * (If you're using a GY-PCM5102 board instead, remember its XSMT pin must
 * be tied to 3.3V or the DAC's output stays soft-muted — see i2s_spk's
 * README for this and other PCM5102-specific pins.)
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "i2s_mic.h"
#include "i2s_spk.h"

#define TAG   "LOOPBACK_LIVE"

/* ---- Mic-side capture configuration (see the file header for why this
 * requests stereo for a single mono microphone). ---------------------- */
#define SAMPLE_RATE           16000
#define MIC_BITS_PER_SAMPLE   32
#define MIC_CHANNEL_COUNT     2
#define KEEP_SLOT             0

#define NUM_BUFS        4
#define BUF_FRAMES      480    /* chosen to stay under classic ESP32's I2S
                                 * DMA descriptor size limit without being
                                 * silently clamped (unlike 512, which the
                                 * driver clamps to 511 — see i2s_spk's
                                 * examples' own history with this). Also
                                 * directly sets relay latency: 480 frames
                                 * / 16000 Hz = 30 ms per hop. */
#define MIC_BUF_BYTES   (BUF_FRAMES * MIC_CHANNEL_COUNT * (MIC_BITS_PER_SAMPLE / 8))

/* ---- Speaker-side playback configuration ------------------------------ */
#define SPK_BITS_PER_SAMPLE   16
#define SPK_CHANNEL_COUNT     2   /* STEREO with duplicated samples — see
                                    * the file header's "SPEAKER PLAYBACK"
                                    * section for why, not MONO. */
#define SPK_DMA_BUFFER_COUNT  6
#define SPK_DMA_BUFFER_SIZE   2048

static uint8_t s_bufs[NUM_BUFS][MIC_BUF_BYTES];
static uint8_t s_stereo_out_buf[BUF_FRAMES * SPK_CHANNEL_COUNT * (SPK_BITS_PER_SAMPLE / 8)];

typedef struct {
    uint8_t *buf;
    size_t   bytes_read;
} filled_item_t;

static QueueHandle_t s_free_q;
static QueueHandle_t s_filled_q;

static volatile uint32_t s_last_overflow_count = 0;
static volatile uint32_t s_last_no_buffer_count = 0;
static volatile uint32_t s_short_write_count = 0;

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
        i2s_mic_request_buffer(free_buf, MIC_BUF_BYTES);
    }

    portYIELD_FROM_ISR(hp_woken);
}

static void mic_overflow_cb(uint32_t total_overflow_count, uint32_t total_no_buffer_count, void *user_ctx)
{
    (void)user_ctx;
    s_last_overflow_count = total_overflow_count;
    s_last_no_buffer_count = total_no_buffer_count;
}

/* Duplicate each mono 16-bit sample into both L and R slots — see the file
 * header's "SPEAKER PLAYBACK: WHY STEREO, NOT MONO" section. */
static void upmix_mono_to_stereo(const int16_t *mono, size_t num_samples, uint8_t *stereo_bytes)
{
    int16_t *dst = (int16_t *)stereo_bytes;
    for (size_t i = 0; i < num_samples; i++) {
        dst[2 * i]     = mono[i];
        dst[2 * i + 1] = mono[i];
    }
}

/* ------------------------------------------------------------------------
 * audio_relay_task — the entire demo. Pull a filled mic buffer, convert
 * it, hand it straight to the speaker, repeat forever.
 * ---------------------------------------------------------------------- */

static void audio_relay_task(void *arg)
{
    (void)arg;
    filled_item_t item;
    static int16_t s_mono_scratch[BUF_FRAMES];

    while (1) {
        if (xQueueReceive(s_filled_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int32_t *src = (int32_t *)item.buf;
        size_t num_frames = item.bytes_read / (MIC_CHANNEL_COUNT * sizeof(int32_t));
        if (num_frames > BUF_FRAMES) {
            num_frames = BUF_FRAMES;   /* defensive; shouldn't happen */
        }

        for (size_t i = 0; i < num_frames; i++) {
            s_mono_scratch[i] = (int16_t)(src[MIC_CHANNEL_COUNT * i + KEEP_SLOT] >> 16);
        }

        /* item.buf is safe to reuse the moment we're done reading from it —
         * return it to the pool before doing the (potentially blocking)
         * speaker write below, so the mic side is never held up waiting on
         * the speaker. */
        xQueueSend(s_free_q, &item.buf, portMAX_DELAY);

        upmix_mono_to_stereo(s_mono_scratch, num_frames, s_stereo_out_buf);
        size_t stereo_len = num_frames * SPK_CHANNEL_COUNT * (SPK_BITS_PER_SAMPLE / 8);

        size_t sent = 0;
        while (sent < stereo_len) {
            size_t this_sent = 0;
            esp_err_t ret = i2s_spk_send_buffer(s_stereo_out_buf + sent, stereo_len - sent, &this_sent);
            if (this_sent < stereo_len - sent) {
                s_short_write_count++;
            }
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "i2s_spk_send_buffer failed: %s", esp_err_to_name(ret));
                break;
            }
            sent += this_sent;
            if (this_sent == 0) {
                break;
            }
        }
    }
}

static void status_report_task(void *arg)
{
    (void)arg;
    uint32_t last_ovf = 0, last_nobuf = 0, last_short = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t ovf = s_last_overflow_count;
        uint32_t nobuf = s_last_no_buffer_count;
        uint32_t shortw = s_short_write_count;
        if (ovf != last_ovf || nobuf != last_nobuf || shortw != last_short) {
            ESP_LOGW(TAG, "mic: dma_overflow=%u no_buffer=%u | spk: short_write=%u",
                     ovf, nobuf, shortw);
            last_ovf = ovf;
            last_nobuf = nobuf;
            last_short = shortw;
        }
    }
}

/* ------------------------------------------------------------------------
 * Entry point — init both components ONCE, start both ONCE, relay forever.
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    s_free_q = xQueueCreate(NUM_BUFS, sizeof(uint8_t *));
    s_filled_q = xQueueCreate(NUM_BUFS, sizeof(filled_item_t));

    i2s_mic_config_t mic_cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
        .channel_count = MIC_CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .gpio_bck = CONFIG_LOOPBACK_MIC_GPIO_BCK,
        .gpio_ws = CONFIG_LOOPBACK_MIC_GPIO_WS,
        .gpio_data = CONFIG_LOOPBACK_MIC_GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = 6,
        .dma_buffer_size = MIC_BUF_BYTES,
        .max_pending_buffers = NUM_BUFS,
        .cb = mic_ready_cb,
        .overflow_cb = mic_overflow_cb,
        .user_ctx = NULL,
    };

    i2s_spk_config_t spk_cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = SPK_BITS_PER_SAMPLE,
        .channel_count = SPK_CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .gpio_bck = CONFIG_LOOPBACK_SPK_GPIO_BCK,
        .gpio_ws = CONFIG_LOOPBACK_SPK_GPIO_WS,
        .gpio_data = CONFIG_LOOPBACK_SPK_GPIO_DATA,
        .port = I2S_NUM_1,   /* the other physical I2S peripheral — this is
                               * the whole point of this example; see the
                               * file header. */
        .dma_buffer_count = SPK_DMA_BUFFER_COUNT,
        .dma_buffer_size = SPK_DMA_BUFFER_SIZE,
        .write_timeout = portMAX_DELAY,
        .user_ctx = NULL,
    };

    /* Both initialized once, here, and never deinited — that's the entire
     * difference from loopback-esp32c3. On a 2-I2S-peripheral chip these
     * two calls don't contend for anything. */
    ESP_ERROR_CHECK(i2s_mic_init(&mic_cfg));
    ESP_ERROR_CHECK(i2s_spk_init(&spk_cfg));

    for (int i = 0; i < NUM_BUFS; i++) {
        ESP_ERROR_CHECK(i2s_mic_request_buffer(s_bufs[i], MIC_BUF_BYTES));
    }

    ESP_ERROR_CHECK(i2s_mic_start());
    ESP_ERROR_CHECK(i2s_spk_start());

    ESP_LOGI(TAG, "Live loopback running — speak into the mic.");

    xTaskCreate(audio_relay_task, "audio_relay", 4096, NULL, 5, NULL);
    xTaskCreate(status_report_task, "status_report", 2048, NULL, 1, NULL);

    /* Nothing left for app_main() to do — both components run forever,
     * driven entirely by mic_ready_cb() and audio_relay_task(). */
    vTaskDelete(NULL);
}

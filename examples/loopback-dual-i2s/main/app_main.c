/*
 * i2s_mic + i2s_spk LIVE loopback example — for chips with TWO independent
 * I2S peripherals (ESP32, ESP32-S3). Captures audio with i2s_mic and
 * relays it straight to i2s_spk, continuously, in real time. No PC, no
 * RAM recording buffer, no repeated init/deinit cycling.
 *
 * The same example exists in both repositories: in i2s_spk it pulls
 * i2s_mic from the ESP Component Registry, in i2s_mic it pulls i2s_spk
 * from the registry (see main/idf_component.yml). The code is identical.
 *
 * ============================================================================
 * WHY THIS EXAMPLE EXISTS SEPARATELY FROM THE SINGLE-I2S LOOPBACK
 * ============================================================================
 * The single-I2S loopback example (examples/loopback-esp32c3 in i2s_spk,
 * examples/loopback_example in i2s_mic) records into RAM, releases
 * i2s_mic, then plays back with i2s_spk. That structure exists only
 * because chips with one I2S peripheral (ESP32-C3, ESP32-C6) cannot have
 * both components initialized at once: each claims a physical I2S
 * controller in i2s_new_channel() for as long as it stays initialized.
 *
 * ESP32 and ESP32-S3 have two I2S peripherals, so both components can be
 * initialized once at boot and run side by side forever, with a genuinely
 * live loopback.
 *
 * Structure:
 *   - i2s_mic on I2S_NUM_0, i2s_spk on I2S_NUM_1, both running for the
 *     lifetime of the program.
 *   - One relay task: i2s_mic_read() one DMA buffer (30 ms), convert it
 *     (32-bit stereo capture -> 16-bit mono -> duplicated into 16-bit
 *     stereo for the speaker), then the blocking i2s_spk_send_buffer().
 *   - Playback paces the loop. While the speaker write blocks, new mic
 *     buffers wait in the I2S driver's own queue (up to 5, about 150 ms),
 *     so nothing is lost as long as the relay keeps up on average — which
 *     it does, since both sides run from the same 16 kHz clock source.
 *
 * Expect a live loopback with a small, constant delay: one capture buffer
 * (30 ms) plus whatever the speaker's DMA buffers hold.
 *
 * ============================================================================
 * MIC CAPTURE: STEREO WORKAROUND AND SAMPLE EXTRACTION
 * ============================================================================
 * Same approach as i2s_mic's own streaming examples: on at least one tested
 * ESP32-C3 / ESP-IDF combination, requesting I2S_SLOT_MODE_MONO for RX did
 * not produce a clean single-slot-per-frame stream, so this example
 * requests STEREO explicitly and discards the unwanted interleaved slot
 * (KEEP_SLOT below) while downconverting INMP441's 32-bit samples to
 * 16-bit. If your relayed audio comes out silent or hum-only, try flipping
 * KEEP_SLOT from 0 to 1, or the INMP441's L/R pin from GND to VDD.
 *
 * ============================================================================
 * SPEAKER PLAYBACK: WHY STEREO, NOT MONO
 * ============================================================================
 * i2s_spk is configured STEREO here, with each mono sample duplicated into
 * both slots, rather than MONO: requesting MONO directly from i2s_spk was
 * found, on classic ESP32, to produce audio on only ONE physical DAC/amp
 * output channel — a chip-family difference in how ESP32's I2S TX hardware
 * handles a MONO slot request, not a bug in i2s_spk. See i2s_spk's README,
 * "Notes," for the full writeup.
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
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2s_mic.h"
#include "i2s_spk.h"

#define TAG   "LOOPBACK_LIVE"

/* ---- Mic-side capture configuration (see the file header for why this
 * requests stereo for a single mono microphone). ---------------------- */
#define SAMPLE_RATE           16000
#define MIC_BITS_PER_SAMPLE   32
#define MIC_CHANNEL_COUNT     2
#define KEEP_SLOT             0

#define MIC_DMA_BUFFER_COUNT  6      /* the driver can hold 5 unread buffers (~150 ms) */
#define BUF_FRAMES            480    /* 30 ms at 16 kHz. One DMA buffer is capped
                                      * at 4092 bytes, so at 8 bytes per stereo
                                      * 32-bit frame the most is 511; i2s_mic_init()
                                      * rejects anything larger. Also sets the
                                      * capture side of the relay latency. */
#define MIC_BUF_BYTES   (BUF_FRAMES * MIC_CHANNEL_COUNT * (MIC_BITS_PER_SAMPLE / 8))

/* ---- Speaker-side playback configuration ------------------------------ */
#define SPK_BITS_PER_SAMPLE   16
#define SPK_CHANNEL_COUNT     2   /* STEREO with duplicated samples — see
                                    * the file header's "SPEAKER PLAYBACK"
                                    * section for why, not MONO. */
#define SPK_DMA_BUFFER_COUNT  6
#define SPK_DMA_BUFFER_SIZE   2048

/* One capture buffer and one output buffer: the relay task reads, converts
 * and sends one DMA buffer at a time. */
static int32_t s_mic_buf[BUF_FRAMES * MIC_CHANNEL_COUNT];
static int16_t s_spk_buf[BUF_FRAMES * SPK_CHANNEL_COUNT];

static volatile uint32_t s_short_write_count = 0;

/* Keep KEEP_SLOT of each stereo frame, downconvert INMP441's 32-bit sample
 * to 16-bit, and duplicate it into both speaker slots — see the file
 * header's "SPEAKER PLAYBACK: WHY STEREO, NOT MONO" section. */
static size_t convert_for_speaker(const int32_t *mic, size_t mic_bytes, int16_t *spk)
{
    size_t frames = mic_bytes / (MIC_CHANNEL_COUNT * sizeof(int32_t));
    for (size_t i = 0; i < frames; i++) {
        int16_t s = (int16_t)(mic[MIC_CHANNEL_COUNT * i + KEEP_SLOT] >> 16);
        spk[2 * i]     = s;
        spk[2 * i + 1] = s;
    }
    return frames * SPK_CHANNEL_COUNT * (SPK_BITS_PER_SAMPLE / 8);
}

/* ------------------------------------------------------------------------
 * audio_relay_task — the entire demo. Read a mic buffer, convert it, hand
 * it straight to the speaker, repeat forever.
 * ---------------------------------------------------------------------- */

static void audio_relay_task(void *arg)
{
    (void)arg;

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_mic_read(s_mic_buf, MIC_BUF_BYTES, &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            /* Only happens if capture is stopped; this example never stops. */
            ESP_LOGE(TAG, "i2s_mic_read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t out_len = convert_for_speaker(s_mic_buf, bytes_read, s_spk_buf);
        uint8_t *out = (uint8_t *)s_spk_buf;   /* i2s_spk_send_buffer() takes a non-const pointer */

        size_t sent = 0;
        while (sent < out_len) {
            size_t this_sent = 0;
            ret = i2s_spk_send_buffer(out + sent, out_len - sent, &this_sent);
            if (this_sent < out_len - sent) {
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
    uint32_t last_lost = 0, last_short = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t lost = i2s_mic_get_overflow_count();
        uint32_t shortw = s_short_write_count;
        if (lost != last_lost || shortw != last_short) {
            ESP_LOGW(TAG, "mic: audio lost %u buffers (%u ms) | spk: short_write=%u",
                     (unsigned)lost, (unsigned)(lost * BUF_FRAMES * 1000u / SAMPLE_RATE),
                     (unsigned)shortw);
            last_lost = lost;
            last_short = shortw;
        }
    }
}

/* ------------------------------------------------------------------------
 * Entry point — init both components ONCE, start both ONCE, relay forever.
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    i2s_mic_config_t mic_cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
        .channel_count = MIC_CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .gpio_bck = CONFIG_LOOPBACK_MIC_GPIO_BCK,
        .gpio_ws = CONFIG_LOOPBACK_MIC_GPIO_WS,
        .gpio_data = CONFIG_LOOPBACK_MIC_GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = MIC_DMA_BUFFER_COUNT,
        .dma_buffer_size = MIC_BUF_BYTES,
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

    /* Both initialized once, here, and never deinited. On a
     * 2-I2S-peripheral chip these two calls don't contend for anything. */
    ESP_ERROR_CHECK(i2s_mic_init(&mic_cfg));
    ESP_ERROR_CHECK(i2s_spk_init(&spk_cfg));

    ESP_ERROR_CHECK(i2s_mic_start());
    ESP_ERROR_CHECK(i2s_spk_start());

    xTaskCreate(audio_relay_task, "audio_relay", 4096, NULL, 5, NULL);
    xTaskCreate(status_report_task, "status_report", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "Live loopback running — speak into the mic.");

    /* Nothing left for app_main() to do — the relay task runs forever. */
    vTaskDelete(NULL);
}

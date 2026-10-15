/*
 * i2s_spk streaming example — PC (WAV file) -> USB-Serial-JTAG -> ESP32-C3
 *                              -> i2s_spk -> MAX98357A -> speaker
 *
 * FOR ESP32-C3, ESP32-S3, ESP32-C6, AND OTHER BOARDS WITH A NATIVE
 * USB-Serial-JTAG PERIPHERAL AND NO SEPARATE UART BRIDGE CHIP — the single
 * USB cable you already flash/monitor through is used for the audio
 * stream too. No extra hardware needed.
 *
 * If your board instead has a separate CP2102/CH340-style bridge chip (a
 * second, distinct COM port appears when you plug in — true of most
 * classic ESP32 devkits), use the sibling example
 * `i2s_spk_uart_bridge_example` instead.
 *
 * ============================================================================
 * WHY THIS EXAMPLE USES A CHUNK/ACK PROTOCOL INSTEAD OF "JUST STREAM IT"
 * ============================================================================
 * i2s_mic's examples stream continuously: the ESP is the sender, the PC's
 * job is just to keep up, and if it briefly can't, the OS's own socket/
 * serial receive buffers absorb the jitter. That pattern does NOT carry
 * over here, because the direction of the bottleneck is reversed:
 *
 *   - Here, the PC is the SENDER, and the ESP is the RECEIVER.
 *   - The ESP's *consumption* rate is fixed by the audio sample rate
 *     (i2s_spk_send_buffer() only accepts data as fast as TX DMA actually
 *     drains it, i.e. in real time) — it cannot speed up to match however
 *     fast the PC feels like writing to the serial port.
 *   - A PC-side script has no natural reason to pace itself to real time
 *     on its own: file reads and serial writes both look "instant" from
 *     Python's perspective compared to 16 kHz audio. Without an explicit
 *     pacing mechanism, the PC would blast the whole WAV file at
 *     USB-CDC/UART speed (hundreds of KB/s to several MB/s), vastly faster
 *     than the ~32 KB/s the speaker can actually consume, and overflow
 *     whatever's on the receiving end — the USB-Serial-JTAG driver's own
 *     RX ring buffer here.
 *
 * Three ways to fix this were considered (see the design document's
 * Section 10.1, which calls this out as an example-level tuning concern):
 *
 *   1. Hardware RTS/CTS flow control — not available here. A native
 *      USB-Serial-JTAG endpoint is a USB-CDC device, not a real UART with
 *      RTS/CTS lines, so there is nothing to wire up even in principle.
 *   2. Keep the audio bitrate well under the transport's raw capacity and
 *      rely on generous on-chip buffering to absorb jitter — this only
 *      helps with *transient* bursts. It does not fix the actual problem
 *      here, which isn't a brief burst but a sustained rate mismatch: an
 *      unpaced sender will eventually fill any finite buffer no matter how
 *      large, because it is not slowing down to match a fixed real-time
 *      consumer.
 *   3. An application-level ACK/credit protocol, where the PC sends one
 *      bounded chunk and then WAITS for the ESP to acknowledge it before
 *      sending the next — chosen here. Each ACK is only sent after the
 *      chunk has actually been handed to i2s_spk_send_buffer() (a blocking
 *      call paced by real playback time — see i2s_spk.h), so the PC's
 *      send rate is tied directly to the ESP's real consumption rate,
 *      automatically, with no separate timer or rate limiter needed on
 *      either end. This is transport-agnostic and works identically for
 *      both this example and the UART-bridge one.
 *
 * The wire format (sync bytes, magic, header layout, and the chunk/length-
 * prefix/ACK protocol) is defined and owned entirely by this example, not
 * by the i2s_spk component. i2s_spk knows nothing about USB, UART,
 * headers, or byte order — see the design document's Section 10
 * ("component boundary" discussion) for why that split exists.
 *
 * Wiring (MAX98357A I2S class-D amp — adjust GPIO_BCK/GPIO_WS/GPIO_DATA
 * below for your board):
 *   MAX98357A BCLK -> ESP32 GPIO_BCK
 *   MAX98357A LRC  -> ESP32 GPIO_WS
 *   MAX98357A DIN  -> ESP32 GPIO_DATA
 *   MAX98357A GAIN -> leave floating (default 9 dB) or see its datasheet
 *   MAX98357A SD   -> leave floating (selects (L+R)/2 mono mixdown — this
 *                     example sends true mono, so this is the setting to
 *                     use; tying it to GND disables the amp entirely, and
 *                     tying it to VDD selects right-channel-only, which
 *                     would play silence against a mono source)
 *   MAX98357A VIN  -> 5V (or 3.3V; the amp works at either, with less
 *                     output power at 3.3V)
 *   MAX98357A GND  -> GND
 *
 * A PCM5102 or UDA1334A I2S DAC into a headphone jack should work
 * identically in place of the MAX98357A — i2s_spk only speaks standard
 * Philips-format I2S and has no part-specific logic, same as i2s_mic.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2s_spk.h"

#define TAG            "SPK_JTAG"

/* ---- Audio / I2S configuration ----------------------------------------
 * Mono, 16-bit, 16 kHz audio. The PC-side script converts/resamples the
 * source WAV to exactly this format before sending; see
 * spk_audio_send.py. That's the WIRE format only, though — see the note
 * on SPK_CHANNEL_COUNT below for what's actually requested from i2s_spk.
 *
 * CORRECTION (this comment previously claimed there was no TX-side
 * equivalent of i2s_mic's mono-mode RX quirk — that turned out to be
 * wrong, just not visible on this chip). On classic ESP32, requesting
 * I2S_SLOT_MODE_MONO from i2s_spk was found to produce audio on only ONE
 * physical DAC/amp output channel; the ESP32-C3 this example targets
 * happens not to exhibit that behavior, which is why it went unnoticed
 * here. See upmix_mono_to_stereo() below for the fix this example now
 * uses regardless of chip, to avoid depending on which chips currently
 * happen to handle MONO correctly. */
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16

/* WIRE_CHANNEL_COUNT describes the mono PCM format the PC sends over the
 * wire (spk_audio_send.py always converts to mono) — used for the audio
 * header, which is about the wire format, not what actually goes to the DAC.
 *
 * SPK_CHANNEL_COUNT is what's actually requested from i2s_spk: STEREO,
 * not MONO, with each sample duplicated into both slots — see
 * upmix_mono_to_stereo() for why. */
#define WIRE_CHANNEL_COUNT   1
#define SPK_CHANNEL_COUNT    2

#define GPIO_BCK          4
#define GPIO_WS           3
#define GPIO_DATA         2

#define DMA_BUFFER_COUNT  6
#define DMA_BUFFER_SIZE   2048   /* bytes; independent of CHUNK_BYTES below —
                                   * see i2s_spk.h's note that send_buffer()
                                   * callers aren't required to match this. */

/* ---- Wire protocol ------------------------------------------------------
 * Handshake: PC waits for READY, sends TRIGGER_BYTE (validated in a loop,
 * not accepted as the first byte received — see host_comm_init() below),
 * ESP replies with audio_header_t once. Then a chunked, ACK-paced PCM
 * stream: PC sends a 4-byte little-endian chunk length followed by that
 * many raw PCM bytes, waits for a single ACK_BYTE from the ESP, and
 * repeats. A chunk length of 0 marks end-of-stream (no ACK follows it);
 * after that the ESP goes back to waiting for the next TRIGGER_BYTE, so
 * the PC script can be re-run to play another file without resetting the
 * board. */
#define TRIGGER_BYTE      0xA5
#define ACK_BYTE          0x06   /* ASCII ACK */
#define AUDIO_MAGIC       0xC0FFEE02u
#define CHUNK_BYTES       4096   /* max mono PCM payload per chunk, as
                                   * received from the PC; must match
                                   * CHUNK_BYTES in spk_audio_send.py */

typedef struct {
    uint8_t  sync[3];          /* 0xAA 0xAA 0xAA */
    uint32_t magic;            /* AUDIO_MAGIC */
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint8_t  channel_count;
} __attribute__((packed)) audio_header_t;

static uint8_t s_chunk_buf[CHUNK_BYTES];        /* raw mono PCM received from the PC */
static uint8_t s_stereo_buf[CHUNK_BYTES * 2];   /* upmixed L=R stereo PCM handed to i2s_spk */

/* ---- Telemetry, logged once per completed playback session (logging is
 * disabled while the binary stream runs — see host_comm_init()).
 * Underruns come from i2s_spk_get_underrun_count(): the speaker runs
 * continuously and counts while idle between sessions, so the session
 * figure is the difference from a snapshot taken just before the
 * session's first write. */
static uint32_t s_short_write_count;
static uint32_t s_underruns_at_start;
static uint32_t s_chunks_played;

/* ------------------------------------------------------------------------
 * Transport helpers (USB-Serial-JTAG)
 * ---------------------------------------------------------------------- */

static void read_exact(uint8_t *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = usb_serial_jtag_read_bytes(dst + got, len - got, portMAX_DELAY);
        if (n > 0) {
            got += (size_t)n;
        }
    }
}

static void write_exact(const uint8_t *src, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = usb_serial_jtag_write_bytes(src + sent, len - sent, portMAX_DELAY);
        if (n > 0) {
            sent += (size_t)n;
        }
    }
}

/* Duplicate each mono 16-bit sample into both L and R slots of an
 * interleaved stereo buffer — see the corrected comment at the top of
 * this file (near WIRE_CHANNEL_COUNT/SPK_CHANNEL_COUNT) for why this
 * example requests STEREO from i2s_spk instead of MONO. This changes what
 * i2s_spk is configured for, NOT the wire protocol — the PC still sends
 * plain mono PCM; only this local buffer, right before
 * i2s_spk_send_buffer(), is upmixed. */
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

/* ------------------------------------------------------------------------
 * One playback session: send header, receive chunks until the 0-length
 * end marker, feed each into i2s_spk, ACK each, track telemetry.
 * ---------------------------------------------------------------------- */

static void run_playback_session(void)
{
    audio_header_t hdr = {
        .sync = {0xAA, 0xAA, 0xAA},
        .magic = AUDIO_MAGIC,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel_count = WIRE_CHANNEL_COUNT,
    };

    /* Disable logging before the header/PCM stream starts — any log line
     * emitted while the binary protocol is running would land inside it
     * and desync the PC script's parser, exactly as in i2s_mic's examples.
     * Restored once this session's 0-length end marker is seen, below. */
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));

    write_exact((const uint8_t *)&hdr, sizeof(hdr));

    s_short_write_count = 0;
    s_underruns_at_start = i2s_spk_get_underrun_count();
    s_chunks_played = 0;

    while (1) {
        uint8_t len_bytes[4];
        read_exact(len_bytes, sizeof(len_bytes));
        uint32_t chunk_len = (uint32_t)len_bytes[0]
                            | ((uint32_t)len_bytes[1] << 8)
                            | ((uint32_t)len_bytes[2] << 16)
                            | ((uint32_t)len_bytes[3] << 24);

        if (chunk_len == 0) {
            break;   /* end-of-stream marker; no ACK follows it */
        }

        /* Defensive bound: the PC script is written to never exceed
         * CHUNK_BYTES, but a corrupted/desynced length prefix could claim
         * otherwise. Refusing to read more than our buffer holds avoids an
         * out-of-bounds write; the session is aborted rather than guessing
         * at a recovery. */
        if (chunk_len > CHUNK_BYTES) {
            esp_log_level_set("*", ESP_LOG_WARN);
            ESP_LOGE(TAG, "chunk_len %" PRIu32 " exceeds CHUNK_BYTES (%d) — aborting session",
                     chunk_len, CHUNK_BYTES);
            return;
        }

        read_exact(s_chunk_buf, chunk_len);

        /* Upmix the received mono chunk into interleaved stereo (L=R) —
         * see upmix_mono_to_stereo()'s comment for why i2s_spk is
         * configured STEREO here even though the source audio is mono. */
        upmix_mono_to_stereo(s_chunk_buf, chunk_len, s_stereo_buf);
        size_t stereo_len = chunk_len * 2;

        if (s_chunks_played == 0) {
            s_underruns_at_start = i2s_spk_get_underrun_count();
        }

        size_t sent = 0;
        while (sent < stereo_len) {
            size_t this_sent = 0;
            esp_err_t ret = i2s_spk_send_buffer(s_stereo_buf + sent, stereo_len - sent, &this_sent);
            if (this_sent < stereo_len - sent) {
                s_short_write_count++;
            }
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                esp_log_level_set("*", ESP_LOG_WARN);
                ESP_LOGE(TAG, "i2s_spk_send_buffer failed: %s", esp_err_to_name(ret));
                return;
            }
            sent += this_sent;
            if (this_sent == 0) {
                /* No progress at all this call (e.g. component not
                 * RUNNING) — avoid spinning forever. */
                break;
            }
        }
        s_chunks_played++;

        uint8_t ack = ACK_BYTE;
        write_exact(&ack, 1);
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "playback session complete: %" PRIu32 " chunks, "
             "%" PRIu32 " short writes, %" PRIu32 " DMA underruns",
             s_chunks_played, s_short_write_count,
             i2s_spk_get_underrun_count() - s_underruns_at_start);
}

/* ------------------------------------------------------------------------
 * Host handshake over USB-Serial-JTAG
 * ---------------------------------------------------------------------- */

static void host_comm_init(void)
{
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_cfg.rx_buffer_size = 4096;
    usb_cfg.tx_buffer_size = 256;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    /* Note: this only installs the driver for our own reads/writes. The
     * console's log output keeps working over the same physical
     * USB-Serial-JTAG cable via ESP-IDF's separate, lower-level console
     * output path — the two coexist by design, same as in i2s_mic's
     * examples. We still explicitly silence logging during the binary
     * protocol itself (see run_playback_session()) out of the same
     * caution i2s_mic's examples apply. */
}

static void wait_for_trigger(void)
{
    ESP_LOGI(TAG, "=== READY, waiting for trigger ===");
    fflush(stdout);

    /* Validated in a loop against a specific expected value, not accepted
     * as the first byte received — a floating/noisy line, or a stray byte
     * left over from a previous session, could otherwise desync the
     * one-shot header that follows. */
    uint8_t trigger = 0;
    do {
        read_exact(&trigger, 1);
    } while (trigger != TRIGGER_BYTE);

    ESP_LOGI(TAG, "Trigger 0x%02X received", trigger);
    fflush(stdout);
}

/* ------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    i2s_spk_config_t cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel_count = SPK_CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_STEREO,   /* see upmix_mono_to_stereo()'s comment for why */
        .gpio_bck = GPIO_BCK,
        .gpio_ws = GPIO_WS,
        .gpio_data = GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = DMA_BUFFER_COUNT,
        .dma_buffer_size = DMA_BUFFER_SIZE,
        /* Block until accepted — this is what makes send_buffer() the
         * real-time pacing reference the ACK protocol relies on. */
        .write_timeout_ms = I2S_SPK_WAIT_FOREVER,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(i2s_spk_init(&cfg));
    ESP_ERROR_CHECK(i2s_spk_start());

    host_comm_init();

    while (1) {
        wait_for_trigger();
        run_playback_session();
    }
}

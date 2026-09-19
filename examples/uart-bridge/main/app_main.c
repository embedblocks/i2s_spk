/*
 * i2s_spk streaming example — PC (WAV file) -> UART0 -> classic ESP32
 *                              -> i2s_spk -> MAX98357A -> speaker
 *
 * FOR CLASSIC ESP32 (AND OTHER BOARDS) WITH A SEPARATE USB-TO-UART BRIDGE
 * CHIP — a second, distinct COM port appears when you plug in, wired to
 * the chip's real UART0 pins (true of most classic ESP32 devkits, using an
 * onboard CP2102, CH340, FTDI, etc.). This example reuses that same
 * UART0 — the one your programming/console cable already talks to — for
 * the audio stream too. No extra hardware needed, same single cable.
 *
 * If your board instead has no separate bridge chip (the cable you flash
 * through is the chip's native USB-Serial-JTAG peripheral — common on
 * ESP32-C3/S3/C6 devkits), use the sibling example
 * `i2s_spk_usb_jtag_example` instead. The two are not interchangeable:
 * this one needs a real UART with a negotiable baud rate, which a native
 * USB-CDC endpoint is not.
 *
 * ============================================================================
 * WHY THIS EXAMPLE USES A CHUNK/ACK PROTOCOL INSTEAD OF "JUST STREAM IT"
 * ============================================================================
 * See the sibling `i2s_spk_usb_jtag_example`'s app_main.c for the full
 * reasoning (it applies identically here — the transport differs, the
 * rate-mismatch problem does not). Short version: the PC is the sender,
 * the ESP's I2S TX consumption rate is fixed by the sample rate, and an
 * unpaced sender would eventually overflow UART0's RX ring buffer no
 * matter how generously it's sized. This example paces the PC via an
 * explicit chunk/ACK protocol: send one bounded chunk, wait for the ESP to
 * ACK it (which only happens after that chunk has been handed off to the
 * real-time-blocking i2s_spk_send_buffer()), then send the next.
 *
 * Unlike the USB-Serial-JTAG variant, hardware RTS/CTS flow control is at
 * least electrically possible here (this is a real UART) — but it isn't
 * used, because most classic ESP32 devkits' onboard bridge chips only
 * expose TX/RX/DTR/RTS to the host, with RTS/DTR already wired to trigger
 * the EN/IO0 auto-reset sequence rather than to genuine hardware flow
 * control pins. Relying on RTS/CTS would make this example work only on
 * the subset of boards with a bridge chip wired for it, and silently hang
 * on everything else. The chunk/ACK protocol works identically on any
 * board with a plain 2-wire (TX/RX) UART bridge, which is the common case.
 *
 * Handshake: connect at a safe HANDSHAKE_BAUD (115200), wait for a
 * validated trigger byte, switch UART0 to a faster STREAM_BAUD (460800),
 * send the audio_header_t, then run the chunk/ACK loop. Logging is
 * disabled once a stream starts, since it shares the same wire as the
 * binary protocol; restored once each stream's end marker is seen.
 *
 * 460800 was chosen over the more common 921600 because several cheap
 * USB-serial bridge chips (CH340/CP2102 clones especially) don't reliably
 * honor 921600 on the PC/driver side — the ESP can transmit/receive at
 * that rate just fine, but the PC's OS/driver may silently fail to switch
 * to match it. 460800 is much more consistently supported, and there's no
 * need to push higher here anyway: this protocol's actual pace is set by
 * real-time audio playback (16 kHz x 16-bit x mono = 32,000 bytes/s of
 * payload), not by transport speed — 460800 baud's ~46,000 bytes/s raw
 * capacity is already far more headroom than this protocol needs, since
 * each chunk's round trip (send ~4 KB, wait for one ACK byte) is quick
 * compared to the ~128ms a 4096-byte mono 16-bit chunk takes to actually
 * play. The bottleneck here is intentionally the speaker, not the wire.
 *
 * The wire format (sync bytes, magic, header layout, and the chunk/length-
 * prefix/ACK protocol) is defined and owned entirely by this example, not
 * by the i2s_spk component. i2s_spk knows nothing about UART, headers, or
 * byte order — see the design document's Section 10 for why that split
 * exists.
 *
 * Wiring (MAX98357A I2S class-D amp):
 *   MAX98357A BCLK -> ESP32 GPIO_BCK
 *   MAX98357A LRC  -> ESP32 GPIO_WS
 *   MAX98357A DIN  -> ESP32 GPIO_DATA
 *   MAX98357A GAIN -> leave floating (default ~9 dB)
 *   MAX98357A SD   -> leave floating (selects mono (L+R)/2 mixdown, matching
 *                     this example's true-mono output)
 *   MAX98357A VIN  -> 5V (or 3.3V, less output power)
 *   MAX98357A GND  -> GND
 *
 * The GPIO defaults below (25/26/27) are classic-ESP32 numbers, chosen to
 * avoid UART0's own pins (GPIO1 TX / GPIO3 RX — do not assume this mapping
 * on other chips; ESP32-C3's UART0 default is GPIO21 TX / GPIO20 RX, for
 * example) and the usual SPI-flash pin range. Adjust for your specific
 * board.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2s_spk.h"

#define TAG            "SPK_UART"

/* ---- Audio / I2S configuration ---------------------------------------- */
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16
#define CHANNEL_COUNT     1
#define BYTES_PER_FRAME   (CHANNEL_COUNT * (BITS_PER_SAMPLE / 8))

#define GPIO_BCK          12
#define GPIO_WS           13
#define GPIO_DATA         14

#define DMA_BUFFER_COUNT  6
#define DMA_BUFFER_SIZE   2048

/* ---- UART configuration (reusing the console/programming UART0) -------- */
#define HANDSHAKE_BAUD    115200
#define STREAM_BAUD       460800

/* ---- Wire protocol — same shape as the USB-Serial-JTAG variant, see its
 * app_main.c for the full description. */
#define TRIGGER_BYTE      0xA5
#define ACK_BYTE          0x06
#define AUDIO_MAGIC       0xC0FFEE02u
#define CHUNK_BYTES       4096   /* must match CHUNK_BYTES in spk_audio_send.py */

typedef struct {
    uint8_t  sync[3];
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint8_t  channel_count;
} __attribute__((packed)) audio_header_t;

static uint8_t s_chunk_buf[CHUNK_BYTES];

static uint32_t s_short_write_count;
static uint32_t s_underrun_estimate_count;
static uint32_t s_chunks_played;

/* ------------------------------------------------------------------------
 * Transport helpers (UART0)
 * ---------------------------------------------------------------------- */

static void read_exact(uint8_t *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = uart_read_bytes(UART_NUM_0, dst + got, len - got, portMAX_DELAY);
        if (n > 0) {
            got += (size_t)n;
        }
    }
}

static void write_exact(const uint8_t *src, size_t len)
{
    int n = uart_write_bytes(UART_NUM_0, (const char *)src, len);
    /* uart_write_bytes() queues into the driver's own TX ring buffer and
     * blocks only if that ring is full; a short return here would mean
     * the ring itself is smaller than `len`, which never happens for the
     * small, fixed-size writes this example makes (a 1-byte ACK or an
     * 11-byte header) against the multi-KB TX buffer sized in
     * host_comm_init(). */
    if (n < 0 || (size_t)n != len) {
        ESP_LOGW(TAG, "short UART write: %d/%zu", n, len);
    }
}

/* ------------------------------------------------------------------------
 * One playback session
 * ---------------------------------------------------------------------- */

static void run_playback_session(void)
{
    audio_header_t hdr = {
        .sync = {0xAA, 0xAA, 0xAA},
        .magic = AUDIO_MAGIC,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel_count = CHANNEL_COUNT,
    };

    /* Disable logging before the header/PCM stream starts — any log line
     * would land inside the binary protocol and desync the PC script's
     * parser. Restored once this session's 0-length end marker is seen. */
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));

    write_exact((const uint8_t *)&hdr, sizeof(hdr));
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);

    s_short_write_count = 0;
    s_underrun_estimate_count = 0;
    s_chunks_played = 0;

    uint64_t prev_chunk_duration_us = 0;
    bool have_prev_chunk = false;

    while (1) {
        uint64_t wait_start_us = esp_timer_get_time();

        uint8_t len_bytes[4];
        read_exact(len_bytes, sizeof(len_bytes));
        uint32_t chunk_len = (uint32_t)len_bytes[0]
                            | ((uint32_t)len_bytes[1] << 8)
                            | ((uint32_t)len_bytes[2] << 16)
                            | ((uint32_t)len_bytes[3] << 24);

        if (chunk_len == 0) {
            break;   /* end-of-stream marker; no ACK follows it */
        }

        if (chunk_len > CHUNK_BYTES) {
            esp_log_level_set("*", ESP_LOG_WARN);
            ESP_LOGE(TAG, "chunk_len %" PRIu32 " exceeds CHUNK_BYTES (%d) — aborting session",
                     chunk_len, CHUNK_BYTES);
            return;
        }

        read_exact(s_chunk_buf, chunk_len);
        uint64_t data_ready_us = esp_timer_get_time();

        if (have_prev_chunk) {
            uint64_t idle_gap_us = data_ready_us - wait_start_us;
            if (idle_gap_us > prev_chunk_duration_us) {
                s_underrun_estimate_count++;
            }
        }

        size_t sent = 0;
        while (sent < chunk_len) {
            size_t this_sent = 0;
            esp_err_t ret = i2s_spk_send_buffer(s_chunk_buf + sent, chunk_len - sent, &this_sent);
            if (this_sent < chunk_len - sent) {
                s_short_write_count++;
            }
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                esp_log_level_set("*", ESP_LOG_WARN);
                ESP_LOGE(TAG, "i2s_spk_send_buffer failed: %s", esp_err_to_name(ret));
                return;
            }
            sent += this_sent;
            if (this_sent == 0) {
                break;
            }
        }
        s_chunks_played++;

        prev_chunk_duration_us = ((uint64_t)chunk_len / BYTES_PER_FRAME) * 1000000ULL / SAMPLE_RATE;
        have_prev_chunk = true;

        uint8_t ack = ACK_BYTE;
        write_exact(&ack, 1);
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "playback session complete: %" PRIu32 " chunks, "
             "%" PRIu32 " short writes, %" PRIu32 " estimated underruns",
             s_chunks_played, s_short_write_count, s_underrun_estimate_count);
}

/* ------------------------------------------------------------------------
 * Host handshake over UART0 (shared with console/programming)
 * ---------------------------------------------------------------------- */

static void host_comm_init(void)
{
    const int tx_buf_size = CHUNK_BYTES * 2;
    const int rx_buf_size = CHUNK_BYTES * 2;

    uart_driver_install(UART_NUM_0, rx_buf_size, tx_buf_size, 0, NULL, 0);
    uart_vfs_dev_use_driver(0);

    uart_config_t handshake_cfg = {
        .baud_rate  = HANDSHAKE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(UART_NUM_0, &handshake_cfg);
}

static void wait_for_trigger(void)
{
    /* Make sure we're back at the safe handshake baud before printing
     * READY / waiting for the next trigger — the previous session (if any)
     * left UART0 at STREAM_BAUD. */
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
    uart_config_t handshake_cfg = {
        .baud_rate  = HANDSHAKE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(UART_NUM_0, &handshake_cfg);

    ESP_LOGI(TAG, "=== READY, waiting for trigger ===");
    fflush(stdout);

    /* Validated in a loop against a specific expected value, not accepted
     * as the first byte received — a floating/noisy line, or a stray byte
     * left over from a previous session, could otherwise desync the
     * one-shot header that follows. */
    uint8_t rx_byte = 0;
    do {
        read_exact(&rx_byte, 1);
    } while (rx_byte != TRIGGER_BYTE);

    ESP_LOGI(TAG, "Trigger 0x%02X received - switching to %d baud", rx_byte, STREAM_BAUD);
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(600));
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);

    uart_config_t stream_cfg = {
        .baud_rate  = STREAM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(UART_NUM_0, &stream_cfg);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    i2s_spk_config_t cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel_count = CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_MONO,
        .gpio_bck = GPIO_BCK,
        .gpio_ws = GPIO_WS,
        .gpio_data = GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = DMA_BUFFER_COUNT,
        .dma_buffer_size = DMA_BUFFER_SIZE,
        .write_timeout = portMAX_DELAY,
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

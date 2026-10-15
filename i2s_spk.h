/*
 * i2s_spk — task-free I2S speaker (audio sink) component for ESP-IDF v6.0.x
 *
 * See the design document ("ESP-IDF I2S Microphone and Speaker Components")
 * and its companion implementation specification for the full contract this
 * header implements. In particular:
 *
 *   - Like i2s_mic, this component has no internal FreeRTOS task.
 *     i2s_spk_send_buffer() is an ordinary *blocking* call that runs in the
 *     caller's own task context. Its return is the sole completion signal —
 *     there is no "buffer consumed" callback (design document Section 5.2).
 *   - Underruns are counted, not inferred. ESP-IDF's TX DMA interrupt
 *     reports a "send queue overflow" (on_send_q_ovf) when a DMA buffer
 *     finishes playing while every free buffer is still unclaimed by the
 *     application — i.e. DMA has run out of fresh audio and is now playing
 *     buffers that auto_clear has zeroed. i2s_spk registers that callback
 *     and exposes the count via i2s_spk_get_underrun_count(). (Versions
 *     before 0.2.0 documented this callback backwards; see CHANGELOG.)
 *   - This is a singleton component: only one i2s_spk instance exists per
 *     firmware image (see the design document's Open Question 5). On
 *     chips with a single I2S peripheral (SOC_I2S_NUM == 1), i2s_spk and
 *     i2s_mic must not be simultaneously initialized — see the
 *     implementation specification's Section 10.
 *
 * Real-time use: enable CONFIG_I2S_ISR_IRAM_SAFE. Without it, the I2S DMA
 * interrupt is held off while flash is being written or erased; if more
 * than one DMA buffer completes in that window, the driver only sees the
 * last one, and the underrun count (and the driver's free-buffer
 * bookkeeping) can go wrong. See the README.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Value for i2s_spk_config_t::write_timeout_ms meaning "block until the
 * data is accepted". ESP-IDF converts the timeout with pdMS_TO_TICKS()
 * internally, so this is in practice a very long wait (about 49.7 days at
 * a 100 Hz tick rate) rather than a literal portMAX_DELAY.
 */
#define I2S_SPK_WAIT_FOREVER  UINT32_MAX

typedef struct {
    int sample_rate;

    /** 16 or 32 only. 24-bit is not supported (see implementation spec
     *  Section 7 — same reasoning as i2s_mic). */
    int bits_per_sample;

    /** 1 = mono, 2 = stereo. Must match slot_mode exactly (validated at
     *  init()). */
    int channel_count;

    /** I2S_SLOT_MODE_MONO or I2S_SLOT_MODE_STEREO; must match
     *  channel_count. */
    i2s_slot_mode_t slot_mode;

    /** Mandatory GPIOs — there is no "unused" convention for these. */
    int gpio_bck;
    int gpio_ws;
    int gpio_data;

    /** I2S peripheral/port to use (e.g. I2S_NUM_0, I2S_NUM_1). Plain `int`
     *  because ESP-IDF v6.0 removed the `i2s_port_t` type in favor of int.
     *  On chips with two I2S peripherals, the recommended convention when
     *  running i2s_mic and i2s_spk together is I2S_NUM_0 for i2s_mic and
     *  I2S_NUM_1 for i2s_spk. */
    int port;

    /** Number of DMA buffers in the TX ring (ESP-IDF's dma_desc_num).
     *  Must be >= 2. Together with dma_buffer_size this sets the output
     *  latency — see the README's "Latency" section. */
    int dma_buffer_count;

    /** Size of one DMA buffer in bytes. Must be an exact multiple of
     *  channel_count * (bits_per_sample / 8), and must be a size the
     *  driver can allocate exactly (at most 4092 bytes on ESP32, -S3, -C3
     *  and -C6). init() reads back the allocation with
     *  i2s_channel_get_info() and fails with ESP_ERR_INVALID_SIZE if the
     *  driver clamped or rounded it. The application may call
     *  i2s_spk_send_buffer() with any length; it need not match this. */
    int dma_buffer_size;

    /** Max time, in milliseconds, i2s_spk_send_buffer() waits for each
     *  free DMA buffer. I2S_SPK_WAIT_FOREVER blocks until accepted, which
     *  is what makes the blocking call itself the pacing mechanism for a
     *  real-time producer. Note: the timeout applies to each buffer
     *  acquisition inside the call, not to the call as a whole, so a call
     *  spanning several DMA buffers can block for longer than this in
     *  total while it keeps making progress.
     *
     *  (Before 0.2.0 this field was `TickType_t write_timeout` and was
     *  passed through to the driver unconverted, which made every finite
     *  timeout too short whenever the tick rate was not 1000 Hz.) */
    uint32_t write_timeout_ms;

    void *user_ctx;
} i2s_spk_config_t;

/**
 * @brief Initialize the speaker component.
 *
 * @return
 *   - ESP_OK
 *   - ESP_ERR_INVALID_ARG    NULL config; unsupported bits_per_sample (not
 *                            16 or 32); channel_count/slot_mode mismatch;
 *                            dma_buffer_count < 2; dma_buffer_size not a
 *                            positive exact multiple of
 *                            channel_count * (bits_per_sample/8)
 *   - ESP_ERR_INVALID_SIZE   the driver did not allocate exactly
 *                            dma_buffer_count * dma_buffer_size bytes of
 *                            DMA buffer (dma_buffer_size above the
 *                            driver's per-buffer limit, or adjusted for
 *                            cache alignment)
 *   - ESP_ERR_INVALID_STATE  already INITIALIZED or RUNNING
 *   - ESP_ERR_NO_MEM         allocation failure
 *   - any other esp_err_t propagated unchanged from the underlying
 *     ESP-IDF I2S driver calls (i2s_new_channel, i2s_channel_init_std_mode,
 *     i2s_channel_register_event_callback, i2s_channel_get_info)
 */
esp_err_t i2s_spk_init(const i2s_spk_config_t *config);

/**
 * @brief Tear down the speaker component and release the underlying
 *        ESP-IDF I2S channel.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (not INITIALIZED — must stop()
 *         first if RUNNING), or an esp_err_t propagated unchanged from
 *         i2s_del_channel().
 */
esp_err_t i2s_spk_deinit(void);

/**
 * @brief Start playback (enables the TX channel) and reset the underrun
 *        count to 0.
 *
 * Note: DMA buffers keep their contents across stop()/start(). Audio that
 * was written but had not yet played when stop() was called can play
 * after the next start().
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (not INITIALIZED), or an esp_err_t
 *         propagated unchanged from i2s_channel_enable().
 */
esp_err_t i2s_spk_start(void);

/**
 * @brief Stop playback.
 *
 * Calls i2s_channel_disable() to halt DMA output. If an i2s_spk_send_buffer()
 * call is in progress on another task, the driver tells it to stop and
 * waits for it to return; that normally takes at most one DMA buffer
 * period, because the writer re-checks the channel state each time a DMA
 * buffer is freed. i2s_spk_stop() is not a general cancellation mechanism.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (not RUNNING), or an esp_err_t
 *         propagated unchanged from i2s_channel_disable().
 */
esp_err_t i2s_spk_stop(void);

/**
 * @brief Send an application-owned buffer of audio to be played.
 *
 * Blocking call, wrapping i2s_channel_write() in the caller's own task
 * context. Waits according to i2s_spk_config_t::write_timeout_ms until the
 * requested bytes have been copied into TX DMA buffers.
 *
 * The buffer is safe for the application to reuse or refill as soon as
 * this call returns, regardless of outcome.
 *
 * On normal full success: returns ESP_OK and *bytes_sent == buffer_len.
 *
 * On timeout: returns ESP_ERR_TIMEOUT and *bytes_sent contains the amount
 * actually transferred, which may be less than buffer_len.
 *
 * If i2s_spk_stop() concurrently disables the channel, this call may
 * instead return ESP_OK with *bytes_sent < buffer_len. Callers must
 * therefore check *bytes_sent against buffer_len on every return.
 *
 * If called while the component is INITIALIZED but not RUNNING, the
 * driver waits up to write_timeout_ms for the channel to be started and
 * then returns ESP_ERR_INVALID_STATE.
 *
 * @param buffer      Application-owned buffer containing audio to play.
 * @param buffer_len  Length of `buffer` in bytes.
 * @param bytes_sent  Out param: bytes actually copied to TX DMA. Must not
 *                    be NULL.
 *
 * @return
 *   - ESP_OK           (check *bytes_sent — may be a partial transfer)
 *   - ESP_ERR_TIMEOUT  partial transfer; write_timeout_ms elapsed while
 *                      waiting for a free DMA buffer
 *   - ESP_ERR_INVALID_ARG    buffer is NULL, buffer_len is 0, or
 *                            bytes_sent is NULL
 *   - ESP_ERR_INVALID_STATE  component is UNINITIALIZED, or the channel
 *                            was not running (see above)
 *   - any other esp_err_t propagated unchanged from i2s_channel_write()
 */
esp_err_t i2s_spk_send_buffer(const uint8_t *buffer, size_t buffer_len, size_t *bytes_sent);

/**
 * @brief Number of DMA underruns since the last i2s_spk_start().
 *
 * Incremented from the I2S TX DMA interrupt (ESP-IDF's on_send_q_ovf
 * event) each time a DMA buffer finishes playing while every free DMA
 * buffer is still unclaimed by the application. One count is roughly one
 * DMA buffer period of silence played because no new audio arrived in
 * time.
 *
 * How to read it:
 *   - A writer that blocks in i2s_spk_send_buffer() keeps about N-1 DMA
 *     buffers of audio queued (N = dma_buffer_count). A gap of P buffer
 *     periods in which the application writes nothing therefore adds about
 *     max(0, P - (N - 1)) counts, measured from the return of the last
 *     write (+/-1 depending on where the gap falls relative to DMA buffer
 *     boundaries). Each count is registered as a buffer of silence starts
 *     to play. Short stalls are absorbed by the queued audio and do not
 *     count.
 *   - The count also rises whenever the application deliberately stops
 *     writing (between sounds, after the last buffer), so it only means
 *     "underrun" while the application intends to feed continuously.
 *   - If the first write comes more than N-1 buffer periods after
 *     i2s_spk_start(), counts accrue before any audio is written.
 *   - Requires CONFIG_I2S_ISR_IRAM_SAFE for accuracy across flash writes
 *     (see the file header comment).
 *
 * Safe to call from any task at any time; returns 0 before the first
 * i2s_spk_init().
 */
uint32_t i2s_spk_get_underrun_count(void);

#ifdef __cplusplus
}
#endif

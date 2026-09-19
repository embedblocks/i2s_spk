/*
 * i2s_spk — task-free I2S speaker (audio sink) component for ESP-IDF v6.0.x
 *
 * See the design document ("ESP-IDF I2S Microphone and Speaker Components")
 * and its companion implementation specification for the full contract this
 * header implements. In particular:
 *
 *   - Like i2s_mic, this component has no internal FreeRTOS task. Unlike
 *     i2s_mic, it also has no application-facing ISR callback at all:
 *     i2s_spk_send_buffer() is an ordinary *blocking* call that runs in the
 *     caller's own task context. Its return is the sole completion signal —
 *     there is no separate "buffer consumed" callback (design document
 *     Section 5.2; this was a deliberate, explicit removal, not an
 *     oversight — see the implementation specification's Section 13).
 *   - There is no software-detected underrun condition in V1. ESP-IDF
 *     provides no TX underrun callback; when the application does not
 *     supply data fast enough, the channel's auto_clear behavior inserts
 *     silence instead of repeating stale data or stalling (design document
 *     Section 3.2). This component does not invent a detector for that
 *     condition — see the design document's Open Question 2 and the
 *     implementation specification's "Do Not Invent" list (Section 13).
 *     Applications that want an approximate, best-effort signal for this
 *     can time their own send loop against the buffer's real-time playback
 *     duration — both example projects in this distribution do exactly
 *     that, entirely at the application layer; see their READMEs.
 *   - This is a singleton component: only one i2s_spk instance exists per
 *     firmware image (see the design document's Open Question 5). On
 *     chips with a single I2S peripheral (SOC_I2S_NUM == 1), i2s_spk and
 *     i2s_mic must not be simultaneously initialized — see the
 *     implementation specification's Section 10.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;

    /** 16 or 32 only. 24-bit is not supported in V1 (see implementation
     *  spec Section 7 — same reasoning as i2s_mic). */
    int bits_per_sample;

    /** 1 = mono, 2 = stereo. Must match slot_mode exactly (validated at
     *  init()). */
    int channel_count;

    /** I2S_SLOT_MODE_MONO or I2S_SLOT_MODE_STEREO; must match
     *  channel_count. */
    i2s_slot_mode_t slot_mode;

    /** Mandatory GPIOs — there is no "unused" convention for these in V1. */
    int gpio_bck;
    int gpio_ws;
    int gpio_data;

    /** I2S peripheral/port to use (e.g. I2S_NUM_0, I2S_NUM_1). Plain `int`
     *  because ESP-IDF v6.0 removed the `i2s_port_t` type in favor of int.
     *  On chips with two I2S peripherals, the recommended convention when
     *  running i2s_mic and i2s_spk together is I2S_NUM_0 for i2s_mic and
     *  I2S_NUM_1 for i2s_spk. On single-peripheral chips, simultaneous
     *  i2s_mic + i2s_spk operation is out of scope for V1 — see the
     *  implementation spec's Section 10. */
    int port;

    /** Passed directly as ESP-IDF's own DMA descriptor count
     *  (dma_desc_num). Not independently validated by this component. */
    int dma_buffer_count;

    /** Bytes of audio payload consumed from one application buffer per
     *  i2s_spk_send_buffer() call's worth of DMA descriptor sizing. Must
     *  be an exact multiple of channel_count * (bits_per_sample / 8), or
     *  init() fails with ESP_ERR_INVALID_ARG. Note that, unlike i2s_mic,
     *  this value only sizes ESP-IDF's internal DMA descriptors — the
     *  application is free to call i2s_spk_send_buffer() with any buffer
     *  length it likes; it is not required to match dma_buffer_size. */
    int dma_buffer_size;

    /** Max time i2s_spk_send_buffer() blocks waiting for the requested
     *  bytes to be submitted to TX DMA. portMAX_DELAY for "block
     *  indefinitely," which is what makes the blocking call itself the
     *  pacing mechanism for a real-time producer (see the design
     *  document's Section 5.2 and Section 10.1's guidance on using this
     *  for example-level flow control). */
    TickType_t write_timeout;

    void *user_ctx;
} i2s_spk_config_t;

/**
 * @brief Initialize the speaker component.
 *
 * @return
 *   - ESP_OK
 *   - ESP_ERR_INVALID_ARG    NULL config; unsupported bits_per_sample (not
 *                            16 or 32); channel_count/slot_mode mismatch;
 *                            dma_buffer_size not an exact multiple of
 *                            channel_count * (bits_per_sample/8)
 *   - ESP_ERR_INVALID_STATE  already INITIALIZED or RUNNING
 *   - ESP_ERR_NO_MEM         allocation failure
 *   - any other esp_err_t propagated unchanged from the underlying
 *     ESP-IDF I2S driver calls (i2s_new_channel, i2s_channel_init_std_mode)
 */
esp_err_t i2s_spk_init(i2s_spk_config_t *config);

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
 * @brief Start playback (enables the TX channel).
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (not INITIALIZED), or an esp_err_t
 *         propagated unchanged from i2s_channel_enable().
 */
esp_err_t i2s_spk_start(void);

/**
 * @brief Stop playback.
 *
 * Calls i2s_channel_disable() to halt DMA output. If an i2s_spk_send_buffer()
 * call is blocked in progress on another task when i2s_spk_stop() is called,
 * this function does not cancel or interrupt it — it simply waits behind
 * ESP-IDF's own internal per-channel synchronization for as long as that
 * call takes to return (up to that call's own write_timeout in the worst
 * case). i2s_spk_stop() must not be used as a cancellation mechanism for an
 * in-flight i2s_spk_send_buffer() call. See the implementation
 * specification's Section 5 for the exact guarantee and its derivation.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (not RUNNING), or an esp_err_t
 *         propagated unchanged from i2s_channel_disable().
 */
esp_err_t i2s_spk_stop(void);

/**
 * @brief Send an application-owned buffer of audio to be played.
 *
 * Blocking call, wrapping i2s_channel_write() in the caller's own task
 * context. Waits according to i2s_spk_config_t::write_timeout until the
 * requested bytes have been submitted to TX DMA.
 *
 * There is no separate "buffer consumed" callback: the buffer is safe for
 * the application to reuse or refill as soon as this call returns,
 * regardless of outcome (design document Section 5.2).
 *
 * On normal full success: returns ESP_OK and *bytes_sent == buffer_len.
 *
 * On timeout: returns ESP_ERR_TIMEOUT and *bytes_sent contains the amount
 * actually transferred, which may be less than buffer_len.
 *
 * If i2s_spk_stop() concurrently disables the channel, this call may
 * instead return ESP_OK with *bytes_sent < buffer_len, depending on the
 * exact interleaving with the underlying ESP-IDF write loop. Callers must
 * therefore check *bytes_sent against buffer_len on every return, not only
 * when the return value is ESP_ERR_TIMEOUT.
 *
 * @param buffer      Application-owned buffer containing audio to play.
 * @param buffer_len   Length of `buffer` in bytes.
 * @param bytes_sent   Out param: bytes actually submitted to TX DMA. Must
 *                     not be NULL.
 *
 * @return
 *   - ESP_OK           (check *bytes_sent — may be a partial transfer, see
 *                       above)
 *   - ESP_ERR_TIMEOUT  partial transfer; write_timeout elapsed
 *   - ESP_ERR_INVALID_ARG    buffer is NULL, buffer_len is 0, or
 *                            bytes_sent is NULL
 *   - ESP_ERR_INVALID_STATE  component is UNINITIALIZED
 *   - any other esp_err_t propagated unchanged from i2s_channel_write()
 */
esp_err_t i2s_spk_send_buffer(uint8_t *buffer, size_t buffer_len, size_t *bytes_sent);

#ifdef __cplusplus
}
#endif

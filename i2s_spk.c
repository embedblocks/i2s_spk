/*
 * i2s_spk — implementation.
 *
 * See include/i2s_spk.h and the design/implementation-specification
 * documents for the contract this file implements. Section references in
 * comments below refer to the implementation specification unless noted
 * otherwise.
 *
 * Unlike i2s_mic, this component touches exactly one synchronization
 * primitive:
 *
 *   `s_spk.lifecycle_mutex` — an ordinary FreeRTOS mutex guarding the
 *   public lifecycle state and its transitions. init()/start()/stop()/
 *   deinit() are task-context-only calls (never from an ISR), so a plain
 *   blocking mutex is correct and sufficient (spec Section 2's speaker
 *   paragraph, Section 5).
 *
 * There is deliberately no ISR-safe critical section, no pending-buffer
 * queue, and no `in_flight` counter here: i2s_spk has no application-facing
 * ISR callback at all (design document Section 5.2) — i2s_spk_send_buffer()
 * is an ordinary blocking call in the caller's own task context, and its
 * return is the sole completion signal. Adding any of the mic's ISR-side
 * machinery here would be inventing synchronization the design doesn't
 * call for; see the implementation spec's Section 13 "Do Not Invent" list.
 *
 * i2s_spk_send_buffer() itself intentionally does NOT take the lifecycle
 * mutex. This is a deliberate consequence of the spec's own account of how
 * i2s_spk_stop() interacts with an in-flight send (spec Section 5, the
 * "Speaker: i2s_spk_stop() called concurrently..." paragraph): the
 * blocking-until-resolved behavior described there is enforced by
 * ESP-IDF's own internal per-channel binary semaphore inside
 * i2s_channel_write()/i2s_channel_disable(), not by this component's
 * lifecycle mutex. If send_buffer() held the lifecycle mutex for the
 * duration of its (potentially long, write_timeout-bounded) call, a
 * concurrent stop() would block on that mutex instead of on
 * i2s_channel_disable()'s own semaphore wait — a different, undocumented
 * interleaving the spec does not describe. Instead, send_buffer() performs
 * an unsynchronized read of a "fully constructed" flag (the same pattern
 * used by i2s_mic_request_buffer() for its UNINITIALIZED check, spec
 * Section 2's note) to reject calls made before init() or after deinit(),
 * then calls straight into i2s_channel_write(), whose own internal
 * synchronization against a concurrent stop()/disable() is what the spec
 * section above describes and relies on.
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "i2s_spk.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "i2s_spk";

typedef enum {
    SPK_STATE_UNINITIALIZED = 0,
    SPK_STATE_INITIALIZED,
    SPK_STATE_RUNNING,
} spk_lifecycle_state_t;

typedef struct {
    /* --- Lifecycle (guarded by lifecycle_mutex; task context only) --- */
    SemaphoreHandle_t lifecycle_mutex;
    spk_lifecycle_state_t state;

    /* Set once by init() after everything below is fully constructed;
     * cleared once by deinit() before it frees/releases anything.
     * Deliberately an ordinary, unsynchronized flag, read without any lock
     * by i2s_spk_send_buffer() — see the file header comment and spec
     * Section 2's note on i2s_mic_request_buffer()'s analogous check for
     * why this is safe: init()/deinit() are not invoked concurrently with
     * send_buffer() on the same instance by contract. */
    volatile bool initialized_flag;

    /* --- Effectively read-only after init(), before deinit() --- */
    TickType_t write_timeout;
    void *user_ctx;
    i2s_chan_handle_t tx_handle;
} i2s_spk_ctx_t;

static i2s_spk_ctx_t s_spk = { 0 };

/* ------------------------------------------------------------------------
 * init / deinit / start / stop
 * ---------------------------------------------------------------------- */

esp_err_t i2s_spk_init(i2s_spk_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->bits_per_sample != 16 && config->bits_per_sample != 32) {
        return ESP_ERR_INVALID_ARG;
    }
    bool slot_mode_ok =
        (config->channel_count == 1 && config->slot_mode == I2S_SLOT_MODE_MONO) ||
        (config->channel_count == 2 && config->slot_mode == I2S_SLOT_MODE_STEREO);
    if (!slot_mode_ok) {
        return ESP_ERR_INVALID_ARG;
    }

    /* slot_bit_width == bits_per_sample for the 16/32-bit cases this
     * component supports (implementation spec Section 7, same reasoning as
     * i2s_mic). */
    const int slot_bit_width = config->bits_per_sample;
    const int bytes_per_frame = config->channel_count * (slot_bit_width / 8);
    if (config->dma_buffer_size <= 0 || (config->dma_buffer_size % bytes_per_frame) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int dma_frame_num = config->dma_buffer_size / bytes_per_frame;

    if (s_spk.lifecycle_mutex == NULL) {
        s_spk.lifecycle_mutex = xSemaphoreCreateMutex();
        if (s_spk.lifecycle_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_spk.lifecycle_mutex, portMAX_DELAY);

    if (s_spk.state != SPK_STATE_UNINITIALIZED) {
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->port, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = config->dma_buffer_count;
    chan_cfg.dma_frame_num = dma_frame_num;
    /* Implementation spec Section 12: must be explicitly set to true. The
     * driver's own default is false — silence-on-underrun does not happen
     * for free. auto_clear is a union alias of auto_clear_after_cb in the
     * pinned v6.0.x headers; auto_clear_before_cb is a distinct sibling
     * field for a use case this component has no need for (no TX
     * application callback exists at all — see the header comment) and is
     * deliberately left at its own default of false. */
    chan_cfg.auto_clear = true;

    i2s_chan_handle_t tx_handle = NULL;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)config->bits_per_sample, config->slot_mode),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->gpio_bck,
            .ws = config->gpio_ws,
            .dout = config->gpio_data,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ret;
    }

    s_spk.write_timeout = config->write_timeout;
    s_spk.user_ctx = config->user_ctx;
    s_spk.tx_handle = tx_handle;

    s_spk.state = SPK_STATE_INITIALIZED;
    /* Set last, after every other field above is fully constructed —
     * send_buffer() reads this without a lock (see file header comment). */
    s_spk.initialized_flag = true;

    xSemaphoreGive(s_spk.lifecycle_mutex);
    return ESP_OK;
}

esp_err_t i2s_spk_deinit(void)
{
    if (s_spk.lifecycle_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_spk.lifecycle_mutex, portMAX_DELAY);

    if (s_spk.state != SPK_STATE_INITIALIZED) {
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Cleared before anything is released — mirrors i2s_mic_deinit()'s
     * ordering for the same reason (spec Section 2's note). */
    s_spk.initialized_flag = false;

    esp_err_t ret = i2s_del_channel(s_spk.tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_del_channel failed: %s", esp_err_to_name(ret));
    }

    s_spk.tx_handle = NULL;
    s_spk.user_ctx = NULL;
    s_spk.write_timeout = 0;

    s_spk.state = SPK_STATE_UNINITIALIZED;

    xSemaphoreGive(s_spk.lifecycle_mutex);
    return ret;
}

esp_err_t i2s_spk_start(void)
{
    if (s_spk.lifecycle_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_spk.lifecycle_mutex, portMAX_DELAY);

    if (s_spk.state != SPK_STATE_INITIALIZED) {
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2s_channel_enable(s_spk.tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ret;
    }

    s_spk.state = SPK_STATE_RUNNING;

    xSemaphoreGive(s_spk.lifecycle_mutex);
    return ESP_OK;
}

esp_err_t i2s_spk_stop(void)
{
    if (s_spk.lifecycle_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Held for stop()'s entire execution, exactly as for i2s_mic_stop() —
     * serializes against a second concurrent stop()/start() (spec Section
     * 2/5's speaker paragraph). Note this does NOT, by itself, make
     * i2s_channel_disable() return quickly: if a i2s_spk_send_buffer()
     * call is in progress on another task, i2s_channel_disable() blocks
     * waiting on ESP-IDF's own internal per-channel binary semaphore until
     * that write returns — see the file header comment and spec Section 5.
     * This function must not be used, or documented, as a way to cancel an
     * in-flight send. */
    xSemaphoreTake(s_spk.lifecycle_mutex, portMAX_DELAY);

    if (s_spk.state != SPK_STATE_RUNNING) {
        xSemaphoreGive(s_spk.lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2s_channel_disable(s_spk.tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
    }

    s_spk.state = SPK_STATE_INITIALIZED;

    xSemaphoreGive(s_spk.lifecycle_mutex);
    return ret;
}

/* ------------------------------------------------------------------------
 * i2s_spk_send_buffer — blocking, ordinary task context only
 * ---------------------------------------------------------------------- */

esp_err_t i2s_spk_send_buffer(uint8_t *buffer, size_t buffer_len, size_t *bytes_sent)
{
    if (buffer == NULL || buffer_len == 0 || bytes_sent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Unsynchronized read by design — see the file header comment and
     * i2s_mic_request_buffer()'s identical pattern (spec Section 2's
     * note). init()/deinit() are not invoked concurrently with this call
     * on the same instance by contract, so this is race-free in practice
     * even though it is not lock-protected. */
    if (!s_spk.initialized_flag) {
        *bytes_sent = 0;
        return ESP_ERR_INVALID_STATE;
    }

    /* i2s_channel_write()'s own internal per-channel synchronization is
     * what governs interaction with a concurrent i2s_spk_stop() — see the
     * file header comment and implementation spec Section 5. This
     * component does not add its own locking around this call. */
    return i2s_channel_write(s_spk.tx_handle, buffer, buffer_len, bytes_sent, s_spk.write_timeout);
}

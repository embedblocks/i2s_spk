/*
 * i2s_spk — implementation.
 *
 * See i2s_spk.h and the design/implementation-specification documents for
 * the contract this file implements. Section references in comments below
 * refer to the implementation specification unless noted otherwise.
 *
 * Synchronization:
 *
 *   `s_spk.lifecycle_mutex` — an ordinary FreeRTOS mutex guarding the
 *   public lifecycle state and its transitions. init()/start()/stop()/
 *   deinit() are task-context-only calls (never from an ISR), so a plain
 *   blocking mutex is correct and sufficient (spec Section 2's speaker
 *   paragraph, Section 5).
 *
 *   `s_spk.underrun_count` — a volatile 32-bit counter with a single writer,
 *   the driver's TX DMA interrupt (via on_send_q_ovf). It is reset only in
 *   start(), while the channel is disabled and the interrupt cannot fire,
 *   and read from any task; aligned 32-bit loads and stores are atomic on
 *   all supported targets, so no lock is needed.
 *
 * i2s_spk_send_buffer() intentionally does NOT take the lifecycle mutex.
 * Its interaction with a concurrent i2s_spk_stop() is governed by
 * ESP-IDF's own per-channel binary semaphore inside
 * i2s_channel_write()/i2s_channel_disable(): disable() marks the channel
 * stopped and waits for the writer, which re-checks the state each time a
 * DMA buffer frees up. If send_buffer() held the lifecycle mutex for its
 * whole (write_timeout_ms-bounded) call, a concurrent stop() would block on
 * that mutex instead — a different interleaving from the one the spec
 * describes. Instead, send_buffer() reads an unsynchronized "fully
 * constructed" flag (the same pattern as i2s_mic) to reject calls made
 * before init() or after deinit().
 */
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "i2s_spk.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
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
     * cleared once by deinit() before it frees/releases anything. Read
     * without a lock by i2s_spk_send_buffer() — init()/deinit() are not
     * invoked concurrently with send_buffer() by contract. */
    volatile bool initialized_flag;

    /* Written only by the TX DMA ISR (spk_on_send_q_ovf) and by start()
     * while the channel is disabled. Lives in .bss, i.e. internal DRAM,
     * so it is safe to touch from an IRAM-safe ISR. */
    volatile uint32_t underrun_count;

    /* --- Effectively read-only after init(), before deinit() --- */
    uint32_t write_timeout_ms;
    void *user_ctx;
    i2s_chan_handle_t tx_handle;
} i2s_spk_ctx_t;

static i2s_spk_ctx_t s_spk = { 0 };

/* ------------------------------------------------------------------------
 * TX DMA interrupt callback
 * ---------------------------------------------------------------------- */

/* Called by the driver from its TX DMA interrupt when a DMA buffer finishes
 * playing while its free-buffer queue is already full — every free buffer
 * is unclaimed, so DMA is now replaying buffers that auto_clear has zeroed.
 * IRAM_ATTR unconditionally: required when CONFIG_I2S_ISR_IRAM_SAFE is
 * enabled (the driver rejects non-IRAM callbacks then), harmless when not. */
static bool IRAM_ATTR spk_on_send_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_spk.underrun_count = s_spk.underrun_count + 1;
    return false;   /* no task woken */
}

/* ------------------------------------------------------------------------
 * init / deinit / start / stop
 * ---------------------------------------------------------------------- */

esp_err_t i2s_spk_init(const i2s_spk_config_t *config)
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
    /* The driver sizes its free-buffer queue as dma_desc_num - 1, so one
     * buffer would leave no queue at all. */
    if (config->dma_buffer_count < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* slot_bit_width == bits_per_sample for the 16/32-bit cases this
     * component supports (implementation spec Section 7). */
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
     * driver's own default is false. auto_clear is a union alias of
     * auto_clear_after_cb in the v6.0.x headers: the driver zeroes each
     * buffer after it has played, so a buffer the application does not
     * refill plays as silence. */
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
        goto fail_del_channel;
    }

    /* The DMA buffers are allocated by i2s_channel_init_std_mode(). The
     * driver silently clamps a buffer above its per-descriptor limit (and
     * may round it for cache alignment on some targets), logging only a
     * warning. Either would change the latency and the unit the underrun
     * counter counts in, so read back what was actually allocated and
     * reject any difference — the same check i2s_mic performs. */
    i2s_chan_info_t info;
    ret = i2s_channel_get_info(tx_handle, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_get_info failed: %s", esp_err_to_name(ret));
        goto fail_del_channel;
    }
    const uint32_t requested = (uint32_t)config->dma_buffer_count * (uint32_t)config->dma_buffer_size;
    if (info.total_dma_buf_size != requested) {
        ESP_LOGE(TAG, "driver allocated %" PRIu32 " bytes of DMA buffer, requested %" PRIu32
                 " (%d x %d); dma_buffer_size is probably above the driver's per-buffer limit",
                 info.total_dma_buf_size, requested, config->dma_buffer_count, config->dma_buffer_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto fail_del_channel;
    }

    /* Must be registered while the channel is READY (before enable). */
    const i2s_event_callbacks_t cbs = {
        .on_send_q_ovf = spk_on_send_q_ovf,
    };
    ret = i2s_channel_register_event_callback(tx_handle, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_register_event_callback failed: %s", esp_err_to_name(ret));
        goto fail_del_channel;
    }

    s_spk.write_timeout_ms = config->write_timeout_ms;
    s_spk.user_ctx = config->user_ctx;
    s_spk.tx_handle = tx_handle;
    s_spk.underrun_count = 0;

    s_spk.state = SPK_STATE_INITIALIZED;
    /* Set last, after every other field above is fully constructed —
     * send_buffer() reads this without a lock (see file header comment). */
    s_spk.initialized_flag = true;

    xSemaphoreGive(s_spk.lifecycle_mutex);
    return ESP_OK;

fail_del_channel:
    i2s_del_channel(tx_handle);
    xSemaphoreGive(s_spk.lifecycle_mutex);
    return ret;
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

    /* Cleared before anything is released — mirrors i2s_mic_deinit(). */
    s_spk.initialized_flag = false;

    esp_err_t ret = i2s_del_channel(s_spk.tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_del_channel failed: %s", esp_err_to_name(ret));
    }

    s_spk.tx_handle = NULL;
    s_spk.user_ctx = NULL;
    s_spk.write_timeout_ms = 0;

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

    /* Reset while the channel is still disabled, so the ISR cannot be
     * incrementing concurrently. */
    s_spk.underrun_count = 0;

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

    /* Held for stop()'s entire execution — serializes against a second
     * concurrent stop()/start(). If an i2s_spk_send_buffer() call is in
     * progress on another task, i2s_channel_disable() marks the channel
     * stopped and waits on ESP-IDF's internal per-channel semaphore until
     * that write notices and returns (normally within one DMA buffer
     * period). */
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

esp_err_t i2s_spk_send_buffer(const uint8_t *buffer, size_t buffer_len, size_t *bytes_sent)
{
    if (buffer == NULL || buffer_len == 0 || bytes_sent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Unsynchronized read by design — see the file header comment. */
    if (!s_spk.initialized_flag) {
        *bytes_sent = 0;
        return ESP_ERR_INVALID_STATE;
    }

    /* i2s_channel_write() takes its timeout in milliseconds and converts it
     * with pdMS_TO_TICKS() itself. */
    return i2s_channel_write(s_spk.tx_handle, buffer, buffer_len, bytes_sent, s_spk.write_timeout_ms);
}

uint32_t i2s_spk_get_underrun_count(void)
{
    return s_spk.underrun_count;
}

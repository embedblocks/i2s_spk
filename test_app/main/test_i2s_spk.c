/*
 * i2s_spk on-target tests.
 *
 * Only the board is needed: nothing is read back from the I2S pins, so a
 * DAC may or may not be connected. Audio is 16 kHz, 16-bit stereo
 * (64 000 bytes/s) throughout; all writes are silence.
 *
 * Underrun arithmetic used below (see i2s_spk_get_underrun_count() in
 * i2s_spk.h): a writer that blocks in send_buffer() keeps about N-1 DMA
 * buffers queued, so a gap of P buffer periods with no writes adds about
 * max(0, P - (N - 1)) counts.
 */
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "i2s_spk.h"
#include "test_i2s_spk.h"

static const char *TAG = "test_i2s_spk";

#define SAMPLE_RATE        16000
#define BYTES_PER_MS       64          /* 16 kHz x 2 ch x 2 bytes / 1000 */

/* Timing tests: 4 x 30 ms ring. */
#define T_COUNT            4
#define T_PERIOD_MS        30
#define T_BUF_BYTES        (T_PERIOD_MS * BYTES_PER_MS)      /* 1920 */

/* Flash stress test: 8 x 10 ms ring (finer periods make missed DMA
 * interrupts easier to see). */
#define S_COUNT            8
#define S_PERIOD_MS        10
#define S_BUF_BYTES        (S_PERIOD_MS * BYTES_PER_MS)      /* 640 */

/* Large enough for the stop-during-send test's 12-buffer write. */
#define SILENCE_BYTES      (12 * T_BUF_BYTES)
static uint8_t s_silence[SILENCE_BYTES];   /* zero-initialized */

/* ------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static i2s_spk_config_t make_cfg(int count, int size_bytes, uint32_t timeout_ms)
{
    i2s_spk_config_t cfg = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel_count = 2,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .gpio_bck = CONFIG_TEST_SPK_GPIO_BCK,
        .gpio_ws = CONFIG_TEST_SPK_GPIO_WS,
        .gpio_data = CONFIG_TEST_SPK_GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = count,
        .dma_buffer_size = size_bytes,
        .write_timeout_ms = timeout_ms,
        .user_ctx = NULL,
    };
    return cfg;
}

static void init_timing_channel(void)
{
    i2s_spk_config_t cfg = make_cfg(T_COUNT, T_BUF_BYTES, I2S_SPK_WAIT_FOREVER);
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_start());
}

/* Blocking writes of whole DMA-buffer-sized chunks; paced by DMA. */
static void write_buffers(int n, size_t buf_bytes)
{
    for (int i = 0; i < n; i++) {
        size_t sent = 0;
        TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_send_buffer(s_silence, buf_bytes, &sent));
        TEST_ASSERT_EQUAL(buf_bytes, sent);
    }
}

static void write_for_ms(int ms)
{
    write_buffers(ms / T_PERIOD_MS, T_BUF_BYTES);
}

/* Sleep for at least `us`, overshooting by at most about one tick. Used for
 * gaps whose length must not be shortened by tick rounding. */
static void sleep_at_least_us(int64_t us)
{
    const int64_t end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end) {
        vTaskDelay(1);
    }
}

/* A gap of N+5 buffer periods plus half a period, so the expected count
 * (6) sits mid-way between DMA buffer boundaries. */
static void writer_gap_n_plus_5(void)
{
    sleep_at_least_us((int64_t)((T_COUNT + 5) * T_PERIOD_MS + T_PERIOD_MS / 2) * 1000);
}

void test_spk_warm_up(void)
{
    i2s_spk_config_t cfg = make_cfg(T_COUNT, T_BUF_BYTES, I2S_SPK_WAIT_FOREVER);
    ESP_ERROR_CHECK(i2s_spk_init(&cfg));
    ESP_ERROR_CHECK(i2s_spk_start());
    size_t sent;
    ESP_ERROR_CHECK(i2s_spk_send_buffer(s_silence, T_BUF_BYTES, &sent));
    ESP_ERROR_CHECK(i2s_spk_stop());
    ESP_ERROR_CHECK(i2s_spk_deinit());

    /* esp_partition reads the partition table into a heap-allocated list
     * on first use and keeps it for the life of the app. Do that here, so
     * the flash stress test isn't charged with it as a leak. */
    (void)esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
}

/* ------------------------------------------------------------------------
 * Init validation and lifecycle
 * ---------------------------------------------------------------------- */

TEST_CASE("init rejects invalid configurations", "[i2s_spk][init]")
{
    i2s_spk_config_t cfg;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(NULL));

    cfg = make_cfg(1, T_BUF_BYTES, 100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(&cfg));      /* count < 2 */

    cfg = make_cfg(0, T_BUF_BYTES, 100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(&cfg));

    cfg = make_cfg(T_COUNT, T_BUF_BYTES + 2, 100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(&cfg));      /* not whole frames */

    cfg = make_cfg(T_COUNT, 0, 100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(&cfg));

    cfg = make_cfg(T_COUNT, T_BUF_BYTES, 100);
    cfg.bits_per_sample = 24;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(&cfg));

    cfg = make_cfg(T_COUNT, T_BUF_BYTES, 100);
    cfg.channel_count = 1;                                            /* slot_mode still STEREO */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_init(&cfg));
}

TEST_CASE("init rejects a DMA buffer the driver would clamp, and frees the channel", "[i2s_spk][init]")
{
    /* 4096 bytes is above the driver's 4092-byte per-buffer limit on the
     * supported targets; the driver would silently allocate 4092. */
    i2s_spk_config_t cfg = make_cfg(T_COUNT, 4096, 100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, i2s_spk_init(&cfg));

    /* The failed init must have deleted its channel: the same port is
     * free again, and a valid init succeeds. */
    cfg = make_cfg(T_COUNT, T_BUF_BYTES, 100);
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_init(&cfg));     /* singleton */
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_deinit());
}

TEST_CASE("lifecycle calls in the wrong state return INVALID_STATE", "[i2s_spk][lifecycle]")
{
    size_t sent = 123;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_stop());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_deinit());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_send_buffer(s_silence, 4, &sent));
    TEST_ASSERT_EQUAL(0, sent);

    i2s_spk_config_t cfg = make_cfg(T_COUNT, T_BUF_BYTES, 100);
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_init(&cfg));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_send_buffer(NULL, 4, &sent));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_send_buffer(s_silence, 0, &sent));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, i2s_spk_send_buffer(s_silence, 4, NULL));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_stop());
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, i2s_spk_deinit());
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_stop());
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_deinit());
}

TEST_CASE("repeated init/start/stop/deinit cycles leak nothing", "[i2s_spk][lifecycle]")
{
    /* The leak check itself runs in tearDown(). */
    for (int i = 0; i < 20; i++) {
        init_timing_channel();
        write_buffers(3, T_BUF_BYTES);
        TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_stop());
        TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_deinit());
    }
}

/* ------------------------------------------------------------------------
 * Timeout units
 * ---------------------------------------------------------------------- */

TEST_CASE("write_timeout_ms is in milliseconds", "[i2s_spk][timeout]")
{
    /* With the channel initialized but not started, the driver waits the
     * full timeout for the channel to be enabled, then gives up. Before
     * 0.2.0 the value was passed through as ticks-meant-as-ms: at 100 Hz a
     * 200 ms timeout (20 ticks) waited only 20 ms. */
    TEST_ASSERT_NOT_EQUAL_MESSAGE(1000, CONFIG_FREERTOS_HZ,
                                  "this test needs a tick rate other than 1000 Hz");

    i2s_spk_config_t cfg = make_cfg(T_COUNT, T_BUF_BYTES, 200);
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_init(&cfg));

    size_t sent = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = i2s_spk_send_buffer(s_silence, T_BUF_BYTES, &sent);
    int64_t waited_ms = (esp_timer_get_time() - t0) / 1000;

    ESP_LOGI(TAG, "send_buffer while stopped waited %" PRId64 " ms (timeout 200 ms)", waited_ms);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
    TEST_ASSERT_EQUAL(0, sent);
    TEST_ASSERT_INT_WITHIN(30, 200, (int)waited_ms);

    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_deinit());
}

/* ------------------------------------------------------------------------
 * Underrun counter (U1-U4)
 * ---------------------------------------------------------------------- */

TEST_CASE("U1: no underruns while writing continuously for 10 s", "[i2s_spk][underrun]")
{
    init_timing_channel();
    write_for_ms(10000);
    uint32_t count = i2s_spk_get_underrun_count();
    ESP_LOGI(TAG, "U1: %" PRIu32 " underruns", count);
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

TEST_CASE("U2: a gap of N+5 buffer periods adds about 6 underruns", "[i2s_spk][underrun]")
{
    init_timing_channel();

    /* Prefill: continuous writes keep the ring full before the gap. */
    write_for_ms(2000);
    uint32_t c0 = i2s_spk_get_underrun_count();
    TEST_ASSERT_EQUAL_UINT32(0, c0);

    writer_gap_n_plus_5();
    uint32_t c1 = i2s_spk_get_underrun_count();

    /* Resuming must stop the count straight away. */
    write_for_ms(1000);
    uint32_t c2 = i2s_spk_get_underrun_count();

    ESP_LOGI(TAG, "U2: N=%d, gap=%d.5 periods: +%" PRIu32 " during gap, +%" PRIu32 " after resume",
             T_COUNT, T_COUNT + 5, c1 - c0, c2 - c1);
    TEST_ASSERT_UINT32_WITHIN(1, 6, c1 - c0);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1, c2 - c1);
}

TEST_CASE("U3: start() resets the underrun count", "[i2s_spk][underrun]")
{
    init_timing_channel();
    write_for_ms(1000);
    writer_gap_n_plus_5();
    TEST_ASSERT_GREATER_THAN_UINT32(0, i2s_spk_get_underrun_count());

    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_stop());
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_start());
    TEST_ASSERT_EQUAL_UINT32(0, i2s_spk_get_underrun_count());

    write_for_ms(1000);
    TEST_ASSERT_EQUAL_UINT32(0, i2s_spk_get_underrun_count());
}

typedef struct {
    volatile bool stop;
    volatile bool went_backwards;
    volatile uint32_t last_seen;
    volatile uint32_t samples;
    SemaphoreHandle_t done;
} reader_ctx_t;

static void underrun_reader_task(void *arg)
{
    reader_ctx_t *ctx = (reader_ctx_t *)arg;
    uint32_t prev = 0;
    while (!ctx->stop) {
        uint32_t now = i2s_spk_get_underrun_count();
        if (now < prev) {
            ctx->went_backwards = true;
        }
        prev = now;
        ctx->last_seen = now;
        ctx->samples++;
        vTaskDelay(1);
    }
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

TEST_CASE("U4: the underrun count is readable from another task while writing", "[i2s_spk][underrun]")
{
    reader_ctx_t ctx = { 0 };
    ctx.done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(ctx.done);

    init_timing_channel();
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(underrun_reader_task, "spk_reader", 3072, &ctx, 4, NULL));

    write_for_ms(1000);
    writer_gap_n_plus_5();
    write_for_ms(1000);

    ctx.stop = true;
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.done, pdMS_TO_TICKS(500)));
    vSemaphoreDelete(ctx.done);

    uint32_t final_count = i2s_spk_get_underrun_count();
    ESP_LOGI(TAG, "U4: reader took %" PRIu32 " samples, last saw %" PRIu32 ", final %" PRIu32,
             ctx.samples, ctx.last_seen, final_count);
    TEST_ASSERT_FALSE(ctx.went_backwards);
    TEST_ASSERT_GREATER_THAN_UINT32(0, ctx.samples);
    TEST_ASSERT_GREATER_THAN_UINT32(0, final_count);
    TEST_ASSERT_EQUAL_UINT32(final_count, ctx.last_seen);
}

/* ------------------------------------------------------------------------
 * stop() during a blocked send_buffer()
 * ---------------------------------------------------------------------- */

typedef struct {
    esp_err_t ret;
    size_t sent;
    SemaphoreHandle_t done;
} big_write_ctx_t;

static void big_write_task(void *arg)
{
    big_write_ctx_t *ctx = (big_write_ctx_t *)arg;
    ctx->ret = i2s_spk_send_buffer(s_silence, SILENCE_BYTES, &ctx->sent);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

TEST_CASE("stop() during a blocked send_buffer() returns promptly with a partial transfer", "[i2s_spk][lifecycle]")
{
    big_write_ctx_t ctx = { 0 };
    ctx.done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(ctx.done);

    init_timing_channel();

    /* 12 buffers' worth (360 ms) with a 4-buffer ring: the writer is
     * still blocked inside send_buffer() after 100 ms. */
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(big_write_task, "spk_big_write", 3072, &ctx, 5, NULL));
    vTaskDelay(pdMS_TO_TICKS(100));

    int64_t t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_stop());
    int64_t stop_ms = (esp_timer_get_time() - t0) / 1000;

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.done, pdMS_TO_TICKS(100)));
    vSemaphoreDelete(ctx.done);

    ESP_LOGI(TAG, "stop() took %" PRId64 " ms; send_buffer returned %s with %u of %u bytes",
             stop_ms, esp_err_to_name(ctx.ret), (unsigned)ctx.sent, (unsigned)SILENCE_BYTES);
    /* One buffer period for the writer to notice, plus scheduling slack.
     * (Plain int: ESP-IDF builds Unity without 64-bit assertions unless
     * CONFIG_UNITY_ENABLE_64BIT is set.) */
    TEST_ASSERT_LESS_OR_EQUAL_INT(2 * T_PERIOD_MS, (int)stop_ms);
    TEST_ASSERT_EQUAL(ESP_OK, ctx.ret);
    TEST_ASSERT_GREATER_THAN(0, ctx.sent);
    TEST_ASSERT_LESS_THAN(SILENCE_BYTES, ctx.sent);
}

/* ------------------------------------------------------------------------
 * Flash-erase stress
 * ---------------------------------------------------------------------- */

typedef struct {
    volatile bool stop;
    volatile uint32_t buffers_written;
    SemaphoreHandle_t done;
} stress_writer_ctx_t;

static void stress_writer_task(void *arg)
{
    stress_writer_ctx_t *ctx = (stress_writer_ctx_t *)arg;
    while (!ctx->stop) {
        size_t sent = 0;
        if (i2s_spk_send_buffer(s_silence, S_BUF_BYTES, &sent) == ESP_OK && sent == S_BUF_BYTES) {
            ctx->buffers_written++;
        }
    }
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

TEST_CASE("flash erase stress: every DMA buffer period is accounted for", "[i2s_spk][flash]")
{
    /* Every DMA buffer that finishes playing is either refilled by the
     * writer or counted as an underrun. So over a window,
     *     buffers written + underruns ≈ elapsed time / buffer period
     * to within about N buffers. With CONFIG_I2S_ISR_IRAM_SAFE disabled,
     * DMA interrupts that fall inside a flash erase can coalesce; those
     * buffers are neither refilled nor counted (stale audio replays), and
     * the books stop balancing. */
    const esp_partition_t *part = esp_partition_find_first(0x40, 0x00, "scratch");
    TEST_ASSERT_NOT_NULL_MESSAGE(part, "scratch partition missing from the partition table");

    stress_writer_ctx_t ctx = { 0 };
    ctx.done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(ctx.done);

    i2s_spk_config_t cfg = make_cfg(S_COUNT, S_BUF_BYTES, I2S_SPK_WAIT_FOREVER);
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, i2s_spk_start());
    /* Higher priority than this (erasing) task, so it refills as soon as
     * it is allowed to run. */
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(stress_writer_task, "spk_stress", 3072, &ctx, 10, NULL));
    vTaskDelay(pdMS_TO_TICKS(500));

    int64_t t0 = esp_timer_get_time();
    uint32_t w0 = ctx.buffers_written;
    uint32_t u0 = i2s_spk_get_underrun_count();

    int erases = 0;
    while (esp_timer_get_time() - t0 < 3000000) {
        for (size_t off = 0; off < part->size && esp_timer_get_time() - t0 < 3000000;
             off += part->erase_size) {
            TEST_ASSERT_EQUAL(ESP_OK, esp_partition_erase_range(part, off, part->erase_size));
            erases++;
        }
    }

    int64_t t1 = esp_timer_get_time();
    uint32_t w1 = ctx.buffers_written;
    uint32_t u1 = i2s_spk_get_underrun_count();

    ctx.stop = true;
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.done, pdMS_TO_TICKS(500)));
    vSemaphoreDelete(ctx.done);

    int32_t periods = (int32_t)((t1 - t0) / (S_PERIOD_MS * 1000));
    int32_t written = (int32_t)(w1 - w0);
    int32_t underruns = (int32_t)(u1 - u0);
    int32_t unaccounted = periods - written - underruns;

    ESP_LOGI(TAG, "flash stress (IRAM_SAFE=%s): %d sector erases, %" PRId32 " periods, "
             "%" PRId32 " written, %" PRId32 " underruns, %" PRId32 " unaccounted",
#if CONFIG_I2S_ISR_IRAM_SAFE
             "y",
#else
             "n",
#endif
             erases, periods, written, underruns, unaccounted);

#if CONFIG_I2S_ISR_IRAM_SAFE
    TEST_ASSERT_INT32_WITHIN(S_COUNT + 1, 0, unaccounted);
#else
    if (unaccounted > S_COUNT + 1) {
        ESP_LOGW(TAG, "about %" PRId32 " DMA buffer periods were missed by the driver during "
                 "flash erases (CONFIG_I2S_ISR_IRAM_SAFE is off); those buffers replayed stale "
                 "audio and were not counted as underruns", unaccounted);
    }
    TEST_PASS_MESSAGE("informational run with CONFIG_I2S_ISR_IRAM_SAFE disabled");
#endif
}

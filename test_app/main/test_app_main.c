/*
 * i2s_spk on-target test runner.
 *
 * Runs every TEST_CASE in this app once, then idles. Each test leaves the
 * component UNINITIALIZED; tearDown() makes sure of that even when a test
 * fails part-way, and checks for heap leaks.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_utils_memory.h"
#include "esp_log.h"

#include "i2s_spk.h"
#include "test_i2s_spk.h"

/* Small allowance for heap bookkeeping noise between setUp and tearDown. */
#define TEST_LEAK_THRESHOLD_BYTES  64

void setUp(void)
{
    unity_utils_record_free_mem();
}

void tearDown(void)
{
    /* Bring the component back to UNINITIALIZED whatever state a failed
     * test left it in. Errors are expected here and ignored. */
    (void)i2s_spk_stop();
    (void)i2s_spk_deinit();
    /* Let deleted helper tasks be reclaimed by the idle task. */
    vTaskDelay(pdMS_TO_TICKS(50));
    unity_utils_evaluate_leaks_direct(TEST_LEAK_THRESHOLD_BYTES);
}

void app_main(void)
{
    /* One-time allocations (i2s_spk's lifecycle mutex, the driver's
     * platform objects) happen on the first init; do that here so they
     * aren't reported as a leak by the first test. */
    test_spk_warm_up();

    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();

    ESP_LOGI("test", "done");
}

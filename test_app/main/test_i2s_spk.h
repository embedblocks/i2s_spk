#pragma once

/* Runs one init/start/write/stop/deinit cycle so one-time allocations are
 * made before the leak-checked tests start. */
void test_spk_warm_up(void);

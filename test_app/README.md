# i2s_spk test app

On-target Unity tests for `i2s_spk`. Only the board is needed: nothing is
read back from the I2S pins, so a DAC may or may not be connected. The pins
used are set under `idf.py menuconfig` → "i2s_spk test app" (defaults:
26/25/22 on ESP32, 4/5/6 elsewhere).

```bash
idf.py set-target esp32      # or esp32c3, esp32s3, esp32c6
idf.py build flash monitor
```

All tests run once at boot; the run takes about 30 seconds. A heap leak
check runs after every test.

## What it covers

| Test | Checks |
|---|---|
| init rejects invalid configurations | NULL config, `dma_buffer_count` < 2, partial frames, 24-bit, channel/slot mismatch |
| init rejects a DMA buffer the driver would clamp | 4096-byte buffer → `ESP_ERR_INVALID_SIZE`, and the channel is freed |
| lifecycle calls in the wrong state | `INVALID_STATE` / `INVALID_ARG` paths |
| repeated init/start/stop/deinit cycles | 20 cycles, no heap leak |
| write_timeout_ms is in milliseconds | a 200 ms timeout waits ~200 ms (pre-0.2.0 code waited 20 ms at 100 Hz) |
| U1 | 10 s of continuous writes → 0 underruns |
| U2 | after a 2 s prefill, a gap of N+5.5 buffer periods → 6 ± 1 underruns, and none after writing resumes |
| U3 | `start()` resets the count |
| U4 | another task reads the count while writing; it never goes backwards and matches the final value |
| stop() during a blocked send_buffer() | `stop()` returns within two buffer periods; the send returns `ESP_OK` with a partial count |
| flash erase stress | while a writer task plays continuously, the `scratch` partition is erased for 3 s; buffers written + underruns must match elapsed buffer periods to within N + 1 |

## The flash-erase stress test with and without `CONFIG_I2S_ISR_IRAM_SAFE`

`sdkconfig.defaults` enables `CONFIG_I2S_ISR_IRAM_SAFE`, and with it the
stress test asserts that every DMA buffer period is accounted for. To see
what happens without it, build a second configuration:

```bash
idf.py -B build_iram_off \
  -DSDKCONFIG=sdkconfig.iram_off \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.iram_off" \
  build flash monitor
```

In that build the stress test is informational: it logs how many buffer
periods went unaccounted for (DMA interrupts that coalesced during a flash
erase, so their buffers replayed stale audio and were not counted) and
passes regardless.

The test erases only the `scratch` partition defined in `partitions.csv`,
through `esp_partition_erase_range()`, so it cannot touch NVS or the app.

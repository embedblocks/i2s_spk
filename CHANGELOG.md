# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).





## [0.2.0] - 2026-10-14

This release contains a breaking API change (`write_timeout` →
`write_timeout_ms`) and corrects a documentation error carried since
0.1.0.

### Fixed
- **Timeout units.** `i2s_channel_write()` takes its timeout in
  milliseconds and converts it with `pdMS_TO_TICKS()` itself, but
  `i2s_spk_send_buffer()` passed `i2s_spk_config_t::write_timeout` (a
  `TickType_t`) straight through. At ESP-IDF's default 100 Hz tick rate
  every finite timeout was 10× too short. `portMAX_DELAY` happened to
  still behave as "forever".
- **Underrun documentation.** The README, header and the 0.1.0 entry
  below said ESP-IDF has no TX underrun hook and that `on_send_q_ovf`
  reports the application supplying data *faster* than it can be sent.
  That is backwards: the driver raises `on_send_q_ovf` when a DMA buffer
  finishes playing while every free buffer is still unclaimed by the
  application — DMA has run out of fresh audio. When the application is
  faster, `i2s_channel_write()` simply blocks.

### Changed
- **Breaking:** `i2s_spk_config_t::write_timeout` (`TickType_t`) is
  replaced by `write_timeout_ms` (`uint32_t`, milliseconds). Use the new
  `I2S_SPK_WAIT_FOREVER` (`UINT32_MAX`) where you used `portMAX_DELAY`.
  The timeout applies to each wait for a free DMA buffer inside the call,
  not to the call as a whole.
- `i2s_spk_init()` now takes `const i2s_spk_config_t *` and
  `i2s_spk_send_buffer()` takes `const uint8_t *` (source-compatible).
- `i2s_spk_init()` now rejects `dma_buffer_count < 2`
  (`ESP_ERR_INVALID_ARG`), and reads back the DMA allocation with
  `i2s_channel_get_info()`, returning `ESP_ERR_INVALID_SIZE` if the driver
  did not allocate exactly `dma_buffer_count × dma_buffer_size` bytes.
  The driver silently clamps buffers above 4092 bytes; the README's old
  4096-byte usage example was affected.
- Examples: the timing-based `underrun_estimate_count` heuristic is
  replaced by the measured count; `loopback-dual-i2s` and `loopback` now
  log it too. All examples use `write_timeout_ms = I2S_SPK_WAIT_FOREVER`.

### Added
- `i2s_spk_get_underrun_count()`: DMA underruns since the last
  `i2s_spk_start()`, counted from the driver's `on_send_q_ovf` event by an
  always-`IRAM_ATTR` callback. See the README's "Underrun counter" for how
  to read it (a gap of P buffer periods adds about max(0, P − (N − 1))).
- README: "Latency" section (formula, worked numbers, the trade-off
  against stall tolerance) — the old usage example's 6 × 4096-byte mono
  ring meant ~768 ms of output latency; "Real-time use" section
  recommending `CONFIG_I2S_ISR_IRAM_SAFE`; note that stale audio can
  replay after `stop()`/`start()`.
- `test_app/`: on-target Unity tests (init validation, lifecycle and leak
  checks, timeout units, underrun counter U1–U4, `stop()` during a blocked
  send, flash-erase stress on a dedicated `scratch` partition).

## [0.1.3] - 2026-10-02

The component's code and API are unchanged; this release updates an
example and the registry metadata.

### Changed
- `examples/loopback-dual-i2s`: ported to i2s_mic 0.2.0, which replaced
  its ISR callback API with a blocking `i2s_mic_read()`. The example now
  uses a single relay task: read one 30 ms microphone buffer, convert it,
  and pass it to `i2s_spk_send_buffer()`. The microphone-side ISR
  callback, buffer queues and re-post logic are removed. The example now
  requires `embedblocks/i2s_mic` `^0.2.0`.
- `idf_component.yml`: added `repository`, `documentation`, `issues`,
  `changelog` and `license` fields; `url` now points to the project page
  instead of the Git URL; added search tags (`audio`, `audio-output`,
  `playback`, `dac`, `pcm5102`, `pcm5102a`, `gy-pcm5102`); description
  now names the PCM5102A (GY-PCM5102) DAC module.

### Fixed
- `examples/loopback-dual-i2s` README: replaced the unexplained
  "dma_overflow climbs while audio is clean" note with its cause.
  i2s_mic 0.1.x copied audio in the driver's `on_recv` callback but never
  called `i2s_channel_read()`, so the driver's internal queue filled
  after five buffers and reported an overflow on every buffer after
  that. It was not audio loss and was not caused by the GPIO choice.


## [0.1.2]
### Fixed
- Missing esp_timer REQUIRES added in examples
### Added
- Loopback dual i2s  example added for devices having multiple i2s devices e.g esp32 and not esp32c3

## [0.1.1]

### Fixed
- All three example projects (`examples/usb-jtag`, `examples/uart-bridge`,
  `examples/loopback`) now request `I2S_SLOT_MODE_STEREO` from `i2s_spk`
  and duplicate each mono sample into both slots in application code,
  instead of requesting `I2S_SLOT_MODE_MONO` directly. This was changed
  after `examples/uart-bridge` was found, on classic ESP32 hardware, to
  play audio on only one physical DAC/amp output channel — a chip-family
  difference in how ESP32's I2S TX hardware handles a MONO slot request
  compared to ESP32-C3's, not a bug in the `i2s_spk` component itself
  (which passes `slot_mode` through unchanged). See the component
  README's "Notes" section for the full explanation. `examples/usb-jtag`
  was not actually broken (ESP32-C3 handles MONO correctly) but is
  updated too, to avoid depending on which chips currently happen to get
  this right.

## [0.1.0] - 2026-09-18

### Added
- Initial release of `i2s_spk`: a task-free I2S speaker (audio sink)
  component for ESP-IDF v6.0.x, built on the modern `i2s_std` driver.
- Blocking `i2s_spk_send_buffer()` API — wraps `i2s_channel_write()` in the
  caller's own task context. Its return is the sole completion signal;
  there is no internal FreeRTOS task and no TX-side application callback
  (see the design document's Section 5.2 for why the earlier
  `spk_buffer_consumed_cb_t` concept was removed).
- `auto_clear` enabled on the TX channel so ESP-IDF inserts silence rather
  than repeating stale data or stalling when the application falls behind
  (no software-level underrun detection in V1 — ESP-IDF provides no driver
  hook for it; see the design document's Open Question 2). *Corrected in
  0.2.0: the driver does provide one, `on_send_q_ovf`.*
- Support for 16-bit and 32-bit sample widths, Philips I2S standard slot
  format, `I2S_ROLE_MASTER`.
- Full lifecycle API: `i2s_spk_init()`, `i2s_spk_start()`,
  `i2s_spk_stop()`, `i2s_spk_deinit()`.
- `examples/usb-jtag`: receives a WAV stream from a PC over native
  USB-Serial-JTAG and plays it through the speaker, for ESP32-C3/S3/C6
  (and similar) boards with no separate USB-to-UART bridge chip. Includes
  a companion PC-side Python script and an explicit chunk/ACK flow-control
  protocol (see its README for why this is needed on the playback side).
- `examples/uart-bridge`: the same, over UART0, for classic ESP32 (and
  similar) boards with a separate CP2102/CH340-style bridge chip.
- `examples/loopback`: a single example (one shared source, no per-target
  variants beyond `sdkconfig.defaults.esp32`/`.esp32c3`) that records with
  `i2s_mic` (pulled in from the ESP Component Registry) and plays the
  recording back through `i2s_spk` — no PC or transport involved. Uses a
  record-then-playback structure rather than live streaming so it works
  correctly on both dual-I2S-peripheral chips (ESP32, S3) and
  single-I2S-peripheral chips (ESP32-C3), where `i2s_mic` and `i2s_spk`
  cannot both be `INITIALIZED` at once — see the example's README.

# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

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
  hook for it; see the design document's Open Question 2).
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

# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

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

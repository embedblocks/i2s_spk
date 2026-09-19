# i2s_spk + i2s_mic loopback example

Record a few seconds of audio with `i2s_mic`, then play it back through
`i2s_spk` — entirely on-device, no PC involved. This example lives inside
the `i2s_spk` repository and pulls in `i2s_mic` from the **ESP Component
Registry** (see `main/idf_component.yml`). An identical copy lives inside
the `i2s_mic` repository's own `examples/loopback`, pulling in `i2s_spk`
from the registry instead — same demo, cross-referenced from both
components so either one's example set has a working end-to-end demo.

**One example, both chips.** Unlike `i2s_spk`'s and `i2s_mic`'s own
individual streaming examples (which are split into separate USB-JTAG and
UART-bridge variants because the *transport hardware* genuinely differs
between chip families), this example needs no transport at all — there's
no PC on the other end. The only thing that actually differs between
ESP32 and ESP32-C3 here is GPIO wiring, which is handled by
`sdkconfig.defaults.esp32` / `sdkconfig.defaults.esp32c3` alone. The
firmware source (`main/app_main.c`) is identical for both targets.

---

## Why record-then-playback, not live/continuous loopback

The obvious version of this demo runs `i2s_mic` and `i2s_spk`
*simultaneously* — capture a buffer, immediately play it, repeat, for true
real-time loopback. That works fine on chips with **two** independent I2S
peripherals (ESP32, ESP32-S3): mic on `I2S_NUM_0`, speaker on
`I2S_NUM_1`, no contention.

**It does not work on ESP32-C3**, or any chip with `SOC_I2S_NUM == 1`.
Both `i2s_mic_init()` and `i2s_spk_init()` independently call ESP-IDF's
`i2s_new_channel()`, which claims a physical I2S controller for as long as
the component stays `INITIALIZED` — not just while it's actively running.
On a single-controller chip, whichever component initializes first gets
the only available controller, and the other's `init()` call fails
outright. This is a hardware/singleton-design constraint documented on
both components (see this component's own README, "Chip Support," and the
shared design document's Section 10) — not something this example works
around so much as designs around.

Since this example is meant to build and run identically on both chips
from one shared source file, it structures the whole demo so that **only
one of the two components is ever `INITIALIZED` at a time**:

1. **Record phase** — `i2s_mic_init()` → `i2s_mic_start()` → capture
   `CONFIG_LOOPBACK_RECORD_SECONDS` of audio into a RAM buffer →
   `i2s_mic_stop()` → `i2s_mic_deinit()` (fully releases the I2S
   controller).
2. **Playback phase** — `i2s_spk_init()` → `i2s_spk_start()` → play the
   whole RAM buffer → `i2s_spk_stop()` → `i2s_spk_deinit()`.
3. Repeat.

This costs a fixed record/playback delay instead of true real-time
loopback — you'll hear your own voice a couple of seconds late, not live —
but it's the only version of this demo that's actually correct on every
chip both components claim to support, including the single-I2S-peripheral
ones.

**If you specifically have a 2-I2S-peripheral chip (ESP32, S3) and want
true live loopback instead:** that's a genuinely different, simpler
example — skip the RAM buffer, keep both components `RUNNING`
simultaneously on separate ports, and pipe each buffer `i2s_mic`'s
callback hands you straight into `i2s_spk_send_buffer()` as it arrives. Not
what's provided here, since it wouldn't run on ESP32-C3 at all.

---

## Wiring

Both devices stay wired at all times — only the *logical* I2S channel
toggles between them in firmware; nothing physical moves between the
record and playback phases.

| INMP441 (mic) | GPIO (Kconfig) |
|---|---|
| SCK | `LOOPBACK_MIC_GPIO_BCK` |
| WS  | `LOOPBACK_MIC_GPIO_WS` |
| SD  | `LOOPBACK_MIC_GPIO_DATA` |
| L/R | GND |
| VDD | 3.3V |
| GND | GND |

| MAX98357A (speaker) | GPIO (Kconfig) |
|---|---|
| BCLK | `LOOPBACK_SPK_GPIO_BCK` |
| LRC  | `LOOPBACK_SPK_GPIO_WS` |
| DIN  | `LOOPBACK_SPK_GPIO_DATA` |
| SD   | floating (mono mixdown) |
| GAIN | floating |
| VIN  | 5V (or 3.3V) |
| GND  | GND |

Default pin numbers per target live in `sdkconfig.defaults.esp32` and
`sdkconfig.defaults.esp32c3`; override any of them via `idf.py menuconfig`
→ "i2s_mic + i2s_spk loopback example" if your wiring differs.

---

## RAM budget

The whole recording is held in RAM as 16 kHz / 16-bit / mono PCM
(2 bytes/sample): `CONFIG_LOOPBACK_RECORD_SECONDS × 32,000` bytes. The
default of 2 seconds is 64,000 bytes — comfortable on both ESP32 (520 KB
SRAM) and ESP32-C3 (400 KB SRAM) with default sdkconfig settings. Raise
`CONFIG_LOOPBACK_RECORD_SECONDS` via menuconfig if you want longer clips,
but watch for allocation failures on ESP32-C3 if you push it much past
what your build's other RAM usage leaves free — `app_main()` logs a clear
error and returns if the initial `malloc()` fails.

---

## Registry dependency note

`main/idf_component.yml` declares a dependency on `embedblocks/i2s_mic`
from the ESP Component Registry. **If that exact version isn't available
there yet**, this won't resolve as-is — see the comment block inside
`main/idf_component.yml` for how to override it to build against a local
`i2s_mic` checkout in the meantime (`override_path`), and switch back to
the plain version-constrained form once you've confirmed the published
version.

---

## Build and flash

```bash
idf.py set-target esp32        # or esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor    # adjust port for your OS/board
```

You should see the log alternate between recording and playback:

```
I (...) LOOPBACK: Recording 2 s...
I (...) LOOPBACK: Recording complete: 32000 samples, drops: dma_overflow=0 no_buffer=0
I (...) LOOPBACK: Playing back...
I (...) LOOPBACK: Playback complete
```

Speak into the microphone during the "Recording..." window and you should
hear it played back a moment later through the speaker.

## Known limitations of this example

- Fixed record duration, no way to stop early or trigger recording on
  demand — it just runs continuously in a loop.
- Not true real-time loopback (see "Why record-then-playback" above).
- Same audio-quality caveats as `i2s_mic`'s and `i2s_spk`'s own examples:
  the mono-extraction shift amount (`>> 16`) is a starting point, not a
  calibrated value, and this example doesn't track `short_write_count` /
  `underrun_estimate_count`-style telemetry the way `i2s_spk`'s own
  streaming examples do — see that component's README for where that
  telemetry actually lives and why.

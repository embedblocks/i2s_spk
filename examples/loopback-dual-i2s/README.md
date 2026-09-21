# i2s_spk + i2s_mic live loopback example (dual I2S peripheral)

**For ESP32 and ESP32-S3** — both have two independent I2S peripherals, so
`i2s_mic` and `i2s_spk` can run `INITIALIZED` and `RUNNING` at the same
time, on separate ports, with no contention. This example does exactly
that: capture with `i2s_mic` on `I2S_NUM_0`, relay straight into `i2s_spk`
on `I2S_NUM_1`, continuously, in real time. No PC, no RAM recording
buffer, no repeated init/deinit cycling — both components are set up once
at boot and run forever.

This example lives inside the `i2s_spk` repository and pulls in `i2s_mic`
from the **ESP Component Registry** (see `main/idf_component.yml`). An
identical copy lives inside the `i2s_mic` repository's own
`examples/loopback-dual-i2s`, pulling in `i2s_spk` from the registry
instead.

**This is NOT the same example as `examples/loopback-esp32c3`.** That one
exists specifically because ESP32-C3 (and C6) have only a single I2S
peripheral, which forces a record-then-playback structure with repeated
teardown/bringup of each component. ESP32 and ESP32-S3 don't have that
constraint, so this example skips all of that: it's simpler, and the
loopback is genuinely live (roughly one relay-buffer's worth of latency —
tens of milliseconds — rather than a multi-second record/playback delay).
**Don't try to build this example for ESP32-C3/C6** — it will fail at
`i2s_spk_init()`, since `i2s_mic` will already own the chip's only I2S
controller. Use `examples/loopback-esp32c3` for those chips instead.

---

## Why this example exists as a separate thing at all

It would be tempting to just make `examples/loopback-esp32c3`'s
record-then-playback structure "the" loopback example and use it
everywhere, since it's portable to every chip both components support.
But paying that structure's cost on chips that don't need it turned out to
have a real, measured downside: testing `examples/loopback-esp32c3` on
classic ESP32 showed its repeated `i2s_mic_init()`/`deinit()` cycling
producing a large, suspiciously constant number of DMA-level overflow
events every single cycle — consistent with a fixed cost tied to channel
bring-up/teardown rather than an ongoing throughput problem — which
`i2s_mic`'s own single-init streaming examples don't exhibit at all. That's
still being tracked down on the `loopback-esp32c3` side. This example
avoids the question entirely by never doing that cycling in the first
place, which is also simply the more natural thing to do on a chip that
doesn't need the workaround.

---

## Wiring

Both devices are wired at all times, on separate I2S peripherals.

| INMP441 (mic, I2S_NUM_0) | GPIO (Kconfig) |
|---|---|
| SCK | `LOOPBACK_MIC_GPIO_BCK` |
| WS  | `LOOPBACK_MIC_GPIO_WS` |
| SD  | `LOOPBACK_MIC_GPIO_DATA` |
| L/R | GND |
| VDD | 3.3V |
| GND | GND |

| MAX98357A (speaker, I2S_NUM_1) | GPIO (Kconfig) |
|---|---|
| BCLK | `LOOPBACK_SPK_GPIO_BCK` |
| LRC  | `LOOPBACK_SPK_GPIO_WS` |
| DIN  | `LOOPBACK_SPK_GPIO_DATA` |
| SD   | floating |
| GAIN | floating |
| VIN  | 5V (or 3.3V) |
| GND  | GND |

If you're using a GY-PCM5102 board instead of a MAX98357A, remember its
**XSMT pin must be tied to 3.3V** or the DAC's output stays soft-muted —
see this component's own README for this and its other PCM5102-specific
pins (SCK, FMT).

Default pin numbers per target live in `sdkconfig.defaults.esp32` and
`sdkconfig.defaults.esp32s3`; override via `idf.py menuconfig` → "i2s_mic
+ i2s_spk live loopback example (dual I2S peripheral)" if your wiring
differs.

---

## Registry dependency note

`main/idf_component.yml` declares a dependency on `embedblocks/i2s_mic`
from the ESP Component Registry. **If that exact version isn't available
there yet**, this won't resolve as-is — see the comment block inside
`main/idf_component.yml` for how to override it to build against a local
`i2s_mic` checkout in the meantime (`override_path`).

---

## Build and flash

```bash
idf.py set-target esp32        # or esp32s3 — NOT esp32c3/esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor    # adjust port for your OS/board
```

```
I (...) LOOPBACK_LIVE: Live loopback running — speak into the mic.
```

Speak into the microphone — you should hear it played back with only a
small, roughly constant delay, continuously, for as long as the board runs.

## Known limitations of this example

- No format negotiation — fixed 16 kHz, mono microphone downmixed and
  upmixed for a stereo-input DAC/amp (see the file header comment in
  `main/app_main.c` for both conversions and why each exists).
- `short_write_count` is tracked (see this component's own README, "On
  underrun/overrun tracking") but there's no `underrun_estimate_count`
  heuristic here the way the streaming examples have one — this example's
  relay loop has no natural "expected chunk duration" reference point the
  way a PC-paced protocol does, since here the mic itself is what's already
  pacing the relay.
- ESP32-C3/C6 are explicitly unsupported — use `examples/loopback-esp32c3`
  for those chips.
- **`dma_overflow` climbs alongside successful captures on classic ESP32
  with this example's exact settings (GPIO 14/15/32 for the mic), at
  close to a 1:1 rate with genuinely successful receives — but this does
  NOT correspond to actual audio loss.** This was confirmed by direct
  instrumentation: a logic analyzer probing debug GPIOs toggled at the top
  of `i2s_mic.c`'s `mic_on_recv()` and `mic_on_recv_q_ovf()` showed both
  firing every ~32 ms, `on_recv_q_ovf` consistently ~88µs after
  `on_recv`, with `mic_on_recv()`'s own logic (per its source) only ever
  invoking `overflow_cb` when no application buffer was available — and
  `no_buffer` stayed at 0 throughout. Captured sample content was
  confirmed to be real, live, varying audio (not silence or a fixed
  pattern), and the relayed playback was listened to directly and
  confirmed clean and continuous, with no audible jumps or dropouts. The
  exact mechanism inside ESP-IDF's I2S driver causing `on_recv_q_ovf` to
  fire this often here — when an otherwise-identical `i2s_mic.c` running
  in `i2s_mic`'s own original continuous streaming example (different
  GPIOs: 16/17/18) never exercises this path at all — has not been
  identified. The two known-ruled-out causes are `BUF_FRAMES` (tested at
  both 480 and 512, no change) and concurrent `i2s_spk` operation (tested
  with `i2s_spk_init()` never called at all, no change); the GPIO pins
  themselves are the last remaining untested variable. If you hit this
  and get further with it — including if it turns out to correspond to
  real data loss on a different board/config, contradicting the "clean
  audio" result above — please feed it back so this note can be corrected
  or resolved.

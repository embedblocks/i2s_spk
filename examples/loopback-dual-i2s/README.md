# i2s_spk + i2s_mic live loopback example (dual I2S peripheral)

**For ESP32 and ESP32-S3** — both have two independent I2S peripherals, so
`i2s_mic` and `i2s_spk` can run at the same time on separate ports. This
example captures with `i2s_mic` on `I2S_NUM_0` and relays straight into
`i2s_spk` on `I2S_NUM_1`, continuously, in real time. No PC, no RAM
recording buffer, no repeated init/deinit: both components are set up once
at boot and run forever.

This example lives inside the `i2s_spk` repository and pulls in `i2s_mic`
from the **ESP Component Registry** (see `main/idf_component.yml`). The
same example lives in the `i2s_mic` repository as
`examples/loopback-dual-i2s`, pulling in `i2s_spk` from the registry
instead. The code is identical.

**This is not the single-I2S loopback example** (`examples/loopback-esp32c3` in this repository). That one
exists because ESP32-C3 (and C6) have only one I2S peripheral, which
forces a record-then-playback structure with each component torn down
between phases. ESP32 and ESP32-S3 don't have that constraint, so this
example is simpler and genuinely live. **Don't build it for
ESP32-C3/C6** — `i2s_spk_init()` will fail, because `i2s_mic` already owns
the chip's only I2S controller.

---

## How it works

One relay task does everything:

1. `i2s_mic_read()` blocks until the next 30 ms capture buffer is ready and
   copies it.
2. The buffer is converted: keep one slot of each 32-bit stereo frame,
   downconvert to 16-bit, and duplicate each sample into both speaker
   slots (see the comments in `main/app_main.c` for why each step exists).
3. `i2s_spk_send_buffer()` writes it to the speaker and blocks until the
   speaker has room, which paces the loop.

While the speaker write blocks, new microphone buffers wait in the I2S
driver's own queue (up to 5, about 150 ms). Both sides run from the same
16 kHz clock, so the relay keeps up and no audio is lost. If it ever falls
behind by more than that, `i2s_mic_get_overflow_count()` counts the dropped
buffers and the status task logs them.

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
see `i2s_spk`'s README for this and its other PCM5102-specific pins (SCK,
FMT).

Default pins per target are in `sdkconfig.defaults.esp32` and
`sdkconfig.defaults.esp32s3`; override them in `idf.py menuconfig` →
"i2s_mic + i2s_spk live loopback example (dual I2S peripheral)".

---

## Registry dependency note

`main/idf_component.yml` requires `embedblocks/i2s_mic` **0.2.0 or later**
(the `i2s_mic_read()` API). Until 0.2.0 is published, the comment in that
file shows how to build against a local `i2s_mic` checkout with
`override_path`.

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

Speak into the microphone — you should hear it played back with a small,
constant delay, for as long as the board runs. The status task logs only
when something changes, so a quiet log means no audio was lost and no
speaker write came up short.

---

## Known limitations

- Fixed format: 16 kHz, one mono microphone slot, upmixed for a
  stereo-input DAC/amp.
- `short_write` counts speaker writes that returned less than requested.
  `underrun` is `i2s_spk_get_underrun_count()`: DMA buffers that played as
  silence because the relay fell behind. With the default 6 × 2048-byte
  speaker ring (about 32 ms per buffer), the relay can stall for roughly
  160 ms before this rises.
- ESP32-C3/C6 are not supported — use the single-I2S loopback example.

### About the old `dma_overflow` note

With `i2s_mic` 0.1.x, this example's log showed `dma_overflow` climbing at
about one per capture buffer while the audio was clean and `no_buffer`
stayed at 0. A logic analyzer showed `on_recv_q_ovf` firing about 88 µs
after `on_recv` on every buffer. That was not audio loss and not caused by
the GPIOs: 0.1.x copied audio in the driver's `on_recv` callback but never
called `i2s_channel_read()`, so the driver's internal queue filled after 5
buffers and the driver reported a "full queue" on every buffer after that.
`i2s_mic` 0.2.0 reads through `i2s_channel_read()`, so its overflow count
only rises when audio is really dropped. The same effect explains the
"large, nearly constant" overflow count per cycle seen with 0.1.x in the
single-I2S loopback: a fixed-length recording produced a fixed number of
spurious events.

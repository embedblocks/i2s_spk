# i2s_spk

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

Task-free I2S speaker (audio sink) component for ESP-IDF, built on the
modern `i2s_std` driver. Hand it filled audio buffers via a blocking call;
it submits them to TX DMA and returns once they've been accepted — no
internal FreeRTOS task, no TX-side application callback, no hidden copies.
It is the playback-side counterpart of
[`i2s_mic`](https://github.com/embedblocks/i2s_mic), built from the same
design document and sharing its architectural philosophy, but its
concurrency model is deliberately simpler — see "Why there's no callback"
below.

Works with any I2S DAC or class-D amp module that speaks the standard
Philips slot format (MAX98357A, PCM5102, UDA1334A, and similar parts —
see the compatibility note under Notes).

---

## Features

* **No internal FreeRTOS task** — `i2s_spk_send_buffer()` runs synchronously
  in the caller's own task context; no extra task, no extra stack, no
  scheduling latency of its own.
* **Blocking send is the only completion signal** — there is no separate
  "buffer consumed" callback. `i2s_spk_send_buffer()` returns once your
  data has been submitted to TX DMA (fully, partially, or on timeout), and
  your buffer is safe to reuse or refill the moment it returns.
* **Application-owned buffers** — you fill an ordinary buffer and hand it
  off; the component never hands you a DMA-owned pointer, and there is no
  internal buffer pool to manage.
* **Silence-on-underrun, not stale audio or a stall** — `auto_clear` is
  enabled on the TX channel, so ESP-IDF automatically outputs zeros when
  your application hasn't supplied new data in time, rather than repeating
  the last DMA buffer or blocking the peripheral.
* **Measured underrun counter** — `i2s_spk_get_underrun_count()` reports how
  many DMA buffers played as silence because no new audio arrived in time,
  counted by ESP-IDF's own TX DMA interrupt, not estimated.
* **The blocking call doubles as backpressure** — because
  `i2s_spk_send_buffer()` doesn't return until ESP-IDF's TX DMA has
  accepted your data, a caller that simply calls it in a loop is
  automatically paced to real time. This is the mechanism the streaming
  example projects build their PC-to-ESP flow control on top of (see their
  READMEs) — no timer, no manual rate limiting needed at the call site.
* **Zero heap allocation on the hot path** — buffers are entirely
  caller-supplied.

---

## Why there's no callback

`i2s_mic`'s data path runs in ISR context because the microphone is
always the one producing data — the driver decides when a buffer is ready,
and the component has to react on the driver's schedule. The speaker case
is the opposite: *your application* decides when to supply the next buffer,
by calling `i2s_spk_send_buffer()` whenever it has one ready. There's
nothing for the component to notify you about that you don't already know
by virtue of having made the call and gotten a return value. An earlier
design draft considered a `spk_buffer_consumed_cb_t`-style completion
callback for symmetry with `i2s_mic`, but it was deliberately dropped —
see the design document's Section 5.2 — because it would have added an
ISR-context (or otherwise asynchronous) hand-off for no actual benefit: the
blocking call's own return is already an unambiguous, synchronous
completion signal.

---

## Chip Support

| Chip | Status |
|---|---|
| ESP32 | Confirmed |
| ESP32-C3 | Confirmed |
| ESP32-S3 | Expected to work (`SOC_I2S_NUM >= 1`) |
| ESP32-C6 | Expected to work (`SOC_I2S_NUM >= 1`) |

Any target with `SOC_I2S_NUM >= 1` should work via the standard driver;
only the two listed above have been directly tested against this
component's specific implementation.

**Running `i2s_mic` and `i2s_spk` together on the same board:** on chips
with two I2S peripherals (ESP32, ESP32-S3), run them on separate ports —
`I2S_NUM_0` for `i2s_mic`, `I2S_NUM_1` for `i2s_spk` is the recommended
convention. On single-I2S-peripheral chips (ESP32-C3, ESP32-C6), the two
components must not be simultaneously initialized in V1 — see Known
Limitations.

---

## Installation

```bash
idf.py add-dependency "embedblocks/i2s_spk^0.2.0"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  embedblocks/i2s_spk: "^0.2.0"
```

---

## Supported Formats (V1)

- `bits_per_sample`: 16 or 32 only (24-bit is rejected — same reasoning as
  `i2s_mic`: ESP-IDF's slot-width-from-data-width auto behavior has
  documented real-world reports of corruption at 24-bit against hardware
  expecting a 32-bit slot).
- Philips I2S standard slot format only (no MSB-justified/PCM/TDM, no MCLK,
  no signal inversion).
- `I2S_ROLE_MASTER` only.

---

## Usage

```c
#include "i2s_spk.h"

#define FRAME_BYTES  960    // 30 ms of 16 kHz, 16-bit mono audio

static uint8_t s_frame[FRAME_BYTES];

void app_main(void)
{
    i2s_spk_config_t cfg = {
        .sample_rate = 16000,
        .bits_per_sample = 16,
        .channel_count = 1,
        .slot_mode = I2S_SLOT_MODE_MONO,
        .gpio_bck = GPIO_NUM_4,
        .gpio_ws = GPIO_NUM_5,
        .gpio_data = GPIO_NUM_6,
        .port = I2S_NUM_0,
        .dma_buffer_count = 4,      // 4 x 10 ms ring: ~30-40 ms output latency
        .dma_buffer_size = 320,     // 10 ms of mono 16-bit audio at 16 kHz
        .write_timeout_ms = I2S_SPK_WAIT_FOREVER,   // block until accepted
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(i2s_spk_init(&cfg));
    ESP_ERROR_CHECK(i2s_spk_start());

    while (1) {
        size_t got = fill_buffer_from_somewhere(s_frame, FRAME_BYTES);
        if (got == 0) {
            break;
        }

        size_t sent = 0;
        while (sent < got) {
            size_t this_sent = 0;
            esp_err_t ret = i2s_spk_send_buffer(s_frame + sent, got - sent, &this_sent);
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_ERROR_CHECK(ret);   // genuine driver error
            }
            sent += this_sent;
            if (this_sent == 0) {
                break;   // avoid spinning forever on a persistent zero-progress case
            }
        }
    }

    ESP_LOGI("app", "underruns: %" PRIu32, i2s_spk_get_underrun_count());
}
```

**Note the `while (sent < got)` loop.** `i2s_spk_send_buffer()` can return
a partial transfer even on `ESP_OK` (see the header doc comment), so a
single call is not guaranteed to consume the whole buffer — check
`*bytes_sent`, don't assume it. With `write_timeout_ms =
I2S_SPK_WAIT_FOREVER` this only happens if a concurrent `i2s_spk_stop()`
disables the channel mid-write; with a finite timeout it can also happen on
ordinary timeout.

**`write_timeout_ms` is in milliseconds** and applies to each wait for a
free DMA buffer inside the call, not to the call as a whole: a long buffer
that spans several DMA buffers can block for longer than the timeout in
total, as long as it keeps making progress. `I2S_SPK_WAIT_FOREVER`
(`UINT32_MAX`) blocks until accepted; because ESP-IDF converts the value
with `pdMS_TO_TICKS()`, that is in practice a wait of about 49.7 days at a
100 Hz tick rate rather than a literal `portMAX_DELAY`.

---

## Latency

Audio passes through a ring of `dma_buffer_count` DMA buffers. A writer
that blocks in `i2s_spk_send_buffer()` keeps that ring almost full, so
output latency sits near its maximum:

```
latency ≈ dma_buffer_count × dma_buffer_size / bytes_per_second
bytes_per_second = sample_rate × channel_count × bits_per_sample / 8
```

| Configuration | Per buffer | Latency |
|---|---|---|
| 6 × 2048 B, stereo 16-bit, 16 kHz (the examples) | 32 ms | ~160–190 ms |
| 4 × 640 B, stereo 16-bit, 16 kHz | 10 ms | ~30–40 ms |
| 4 × 320 B, mono 16-bit, 16 kHz (the snippet above) | 10 ms | ~30–40 ms |

The same ring is also your protection against the writer stalling: a
blocked writer keeps about `dma_buffer_count − 1` buffers queued, so it can
stall for roughly that many buffer periods before DMA runs dry. Lower
latency means less tolerance — size it for your longest expected stall,
and use the underrun counter to check. `dma_buffer_size` does not have to
match the size of the buffers you pass to `i2s_spk_send_buffer()`; for
interactive audio, small DMA buffers with a few of them are usually the
right trade.

`dma_buffer_size` must also be a size the driver can allocate exactly — at
most 4092 bytes on ESP32, -S3, -C3 and -C6. `i2s_spk_init()` reads back the
allocation and returns `ESP_ERR_INVALID_SIZE` if the driver clamped or
rounded it, rather than silently running with a different latency.

---

## Underrun counter

`i2s_spk_get_underrun_count()` returns the number of DMA underruns since
the last `i2s_spk_start()` (which resets it to 0).

**Where it comes from.** ESP-IDF's TX driver keeps a queue of DMA buffers
that have finished playing and are free to refill;
`i2s_spk_send_buffer()` takes buffers from it, and blocks when it is
empty. When a buffer finishes playing while that queue is already full —
every free buffer is unclaimed — the driver drops the oldest entry and
raises its `on_send_q_ovf` event. At that point DMA is replaying buffers
the application never refilled, which `auto_clear` has zeroed: an
underrun. i2s_spk registers that event and counts it. It is measured by
the driver, not inferred.

**How to read it.**

- One count ≈ one DMA buffer period of silence played because no new audio
  arrived in time.
- Because a blocking writer keeps about N−1 buffers queued
  (N = `dma_buffer_count`), a gap of P buffer periods with no writes adds
  about **max(0, P − (N − 1))** counts, measured from the return of the
  last write (±1 depending on where the gap falls relative to DMA buffer
  boundaries). Each count is registered as a buffer of silence starts to
  play. Short stalls are absorbed and don't count.
- The count also rises whenever the application deliberately stops
  writing — between sounds, or after the last buffer — so it only means
  "underrun" while the application intends to feed continuously. To
  measure one stretch of playback, take a snapshot before its first write
  and subtract (the streaming examples do this).
- If the first write comes more than N−1 buffer periods after
  `i2s_spk_start()`, counts accrue before any audio has been written.
- It is accurate across flash writes only with `CONFIG_I2S_ISR_IRAM_SAFE`
  enabled — see below.

`*bytes_sent` remains the other signal: a short transfer means the driver
did not accept everything in the time allowed (or the channel was stopped
mid-write).

---

## Real-time use: enable `CONFIG_I2S_ISR_IRAM_SAFE`

While flash is being written or erased (NVS writes, OTA, a filesystem),
ESP-IDF disables the flash cache, parks the other CPU on dual-core chips,
and holds off every interrupt that isn't IRAM-safe. The I2S DMA hardware
keeps cycling the ring regardless, but the driver's DMA interrupt is
delayed. If more than one DMA buffer finishes in that window, the
interrupts coalesce and the driver only sees the last one: the others are
never returned to the free queue or cleared by `auto_clear`, so stale
audio can replay, the driver's free-buffer bookkeeping drifts, and the
underrun counter undercounts.

Set `CONFIG_I2S_ISR_IRAM_SAFE=y` (Component config → ESP-Driver: I2S
Configurations) in any project that plays audio continuously. i2s_spk's
own interrupt callback is always placed in IRAM, so it works either way.

Note that no setting lets your *tasks* run during a flash write: no task
runs on either core until it finishes, wherever its code lives. A sector
erase can take tens of milliseconds or more, which no reasonable DMA ring
covers — keep long flash operations out of periods where audio must be
continuous.

---

## Examples

| Example | Transport | Board type | Notes |
|---|---|---|---|
| `examples/usb-jtag` | Native USB-Serial-JTAG | ESP32-C3/S3/C6 boards with no separate UART bridge chip | Single cable, no extra hardware |
| `examples/uart-bridge` | UART0 (shared with console) | Classic ESP32 boards with a CP2102/CH340-style bridge chip | Single cable, no extra hardware |

Both examples receive a WAV file streamed from a PC script and play it
through an I2S DAC/amp. **Unlike `i2s_mic`'s examples, both use an
explicit chunk/ACK flow-control protocol rather than streaming
continuously** — see either example's README for why: the direction of
the bottleneck is reversed for playback (the PC is the sender, and the
ESP's I2S TX consumption rate is fixed by the sample rate), so the "just
stream continuously" pattern that works for capture would let a PC that
briefly gets ahead overflow whatever's on the receiving end.

---

## Notes

**Singleton.** One `i2s_spk` instance per firmware image — calling
`init()` a second time before `deinit()` returns `ESP_ERR_INVALID_STATE`.

**Amp/DAC compatibility is broader than any one part number.** `i2s_spk`
has no part-specific logic at all — it's a generic I2S transmitter. Both
examples wire to a **MAX98357A** I2S class-D amplifier (drives a small
speaker directly, needs only 3.3V/5V, GND, and the 3 I2S signal lines — no
separate DAC + amplifier stage). A PCM5102 or UDA1334A I2S DAC into a
headphone jack should work identically, since the component itself only
speaks standard Philips-format I2S and has no awareness of what's on the
other end of the wire; if you try one, that's useful information worth
contributing back.

**`i2s_spk_send_buffer()` is an ordinary blocking task-context call** — it
must not be called from an ISR. The component's only interrupt-context code
is its internal underrun-counting callback; nothing application-facing runs
in an ISR.

**Stale audio can replay after a restart.** DMA buffers keep their contents
across `i2s_spk_stop()`/`i2s_spk_start()`: audio that had been written but
not yet played when `stop()` was called can play after the next
`start()`.

**MONO slot_mode has been observed to behave differently across chip
families.** On classic ESP32, configuring `i2s_spk` with
`I2S_SLOT_MODE_MONO` was found (via this repository's own uart-bridge
example) to produce audio on only ONE physical DAC/amp output channel —
the other stayed silent — while the exact same configuration plays
correctly on both channels on ESP32-C3. This is a difference in how each
chip's I2S TX hardware interprets a MONO slot request, not a bug in
`i2s_spk` itself: the component passes `slot_mode` straight to ESP-IDF's
driver unchanged, and this is the TX-side counterpart to the RX-side mono
ambiguity already documented on `i2s_mic`. If you're driving mono audio to
a stereo-input DAC/amp (as both example projects in this distribution do),
the robust fix — used in all of this distribution's examples — is to
request `I2S_SLOT_MODE_STEREO` with `channel_count = 2` and duplicate each
sample into both slots yourself in application code, rather than rely on
which chips currently happen to duplicate a MONO source correctly. See
`upmix_mono_to_stereo()` in any of the example `app_main.c` files for a
minimal implementation of this.

---

## Implementation Notes / Judgment Calls

The design and implementation-specification documents this component was
built from are unusually complete, but a few points required a decision
during implementation, each also called out in code comments at the
relevant spot:

1. **`i2s_port_t` doesn't exist in ESP-IDF v6.0, and `REQUIRES driver` no
   longer pulls in I2S headers.** Same situation as `i2s_mic` — this
   component's `CMakeLists.txt` requires `esp_driver_i2s` explicitly, and
   `i2s_spk_config_t::port` is typed plain `int`.
2. **`i2s_spk_send_buffer()` does not take the lifecycle mutex.** See the
   large comment at the top of `i2s_spk.c` for the reasoning: the spec's
   account of how `stop()` interacts with an in-flight send describes
   ESP-IDF's own internal per-channel semaphore doing the blocking, not
   this component's lock. Holding the lifecycle mutex across a
   potentially long, `write_timeout_ms`-bounded call would produce a
   different (undocumented) interleaving than the one the spec actually
   describes and relies on. Instead, `send_buffer()` uses the same
   unsynchronized "fully constructed" flag pattern `i2s_mic_request_buffer()`
   uses for its `UNINITIALIZED` check.
3. **The underrun callback is always `IRAM_ATTR`.** With
   `CONFIG_I2S_ISR_IRAM_SAFE` enabled the driver rejects callbacks that
   aren't in IRAM; with it disabled the attribute is harmless. The counter
   it increments lives in internal RAM.
4. **Underruns are counted via `on_send_q_ovf`.** Versions before 0.2.0
   said ESP-IDF had no TX underrun hook and described this event as the
   opposite condition; that was a misreading of the driver. The event is
   the driver's own measurement of DMA running out of fresh audio (see
   "Underrun counter" above), so exposing it does not fall under the
   implementation specification's "Do Not Invent" rule.
5. **DMA allocation is verified.** `i2s_spk_init()` compares
   `i2s_channel_get_info()`'s `total_dma_buf_size` with
   `dma_buffer_count × dma_buffer_size` and fails on any difference, the
   same check `i2s_mic` performs.

Everything else — the state machine, the single-lock model, the
`auto_clear` configuration, and the byte-accounting formula for
`dma_buffer_size` — is implemented exactly as specified.

---

## Known Limitations

- Multi-instance / handle-based API is out of scope for V1 (singleton
  only).
- MSB-justified/PCM/TDM slot formats, MCLK, and signal inversion are not
  supported.
- 24-bit audio is not supported (`bits_per_sample` must be 16 or 32).
- No coordination with a companion microphone component — on
  single-I2S-peripheral chips (`SOC_I2S_NUM == 1`, e.g. ESP32-C3/C6),
  `i2s_mic` and `i2s_spk` must not be simultaneously initialized; the
  application is responsible for enforcing this, and for correct port
  assignment on chips where both can run together.
- `I2S_SLOT_MODE_MONO` is not portable across chip families in practice —
  see "MONO slot_mode has been observed to behave differently across chip
  families" above. All examples in this distribution avoid it entirely by
  using STEREO with duplicated samples instead.

---

## Tests

`test_app/` is an on-target Unity test project. It needs only the board —
no DAC has to be connected, since nothing is read back from the I2S pins.

```bash
cd test_app
idf.py set-target esp32      # or esp32c3, esp32s3, esp32c6
idf.py build flash monitor
```

It covers init validation, the lifecycle (including a repeated
init/start/stop/deinit leak check), the timeout units, the underrun counter
(continuous writes, a gap of N+5 buffer periods, reset on `start()`, reads
from another task), `stop()` during a blocked `send_buffer()`, and a
flash-erase stress test on a dedicated `scratch` partition. The default
`sdkconfig.defaults` enables `CONFIG_I2S_ISR_IRAM_SAFE`; to see the
difference it makes, run the stress test once more with it disabled (see
`test_app/README.md`).

---

## Requirements

- ESP-IDF v6.0.x (v6.0–v6.0.3 verified against the pinned header behavior
  this component relies on; re-verify `i2s_channel_write`/`disable`/`enable`
  semantics against any later v6.0.x patch before upgrading).
- A target with `SOC_I2S_NUM >= 1` (see Chip Support above).

---

## License

MIT License — see LICENSE file.

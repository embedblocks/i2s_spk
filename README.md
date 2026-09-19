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
* **The blocking call doubles as backpressure** — because
  `i2s_spk_send_buffer()` doesn't return until ESP-IDF's TX DMA has
  accepted your data, a caller that simply calls it in a loop is
  automatically paced to real time. This is the mechanism both example
  projects build their PC-to-ESP flow control on top of (see their
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
idf.py add-dependency "embedblocks/i2s_spk^0.1.0"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  embedblocks/i2s_spk: "^0.1.0"
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

#define BUF_BYTES  4096

static uint8_t s_playback_buf[BUF_BYTES];

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
        .dma_buffer_count = 6,
        .dma_buffer_size = BUF_BYTES,
        .write_timeout = portMAX_DELAY,   // block until accepted — see below
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(i2s_spk_init(&cfg));
    ESP_ERROR_CHECK(i2s_spk_start());

    while (1) {
        size_t got = fill_buffer_from_somewhere(s_playback_buf, BUF_BYTES);
        if (got == 0) {
            break;
        }

        size_t sent = 0;
        uint8_t *p = s_playback_buf;
        while (sent < got) {
            size_t this_sent = 0;
            esp_err_t ret = i2s_spk_send_buffer(p, got - sent, &this_sent);
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                ESP_ERROR_CHECK(ret);   // genuine driver error
            }
            p += this_sent;
            sent += this_sent;
            if (this_sent == 0) {
                break;   // avoid spinning forever on a persistent zero-progress case
            }
        }
    }
}
```

**Note the `while (sent < got)` loop.** `i2s_spk_send_buffer()` can return
a partial transfer even on `ESP_OK` (see the header doc comment), so a
single call is not guaranteed to consume the whole buffer — check
`*bytes_sent`, don't assume it. With `write_timeout = portMAX_DELAY` this
only happens if a concurrent `i2s_spk_stop()` disables the channel
mid-write; with a finite timeout it can also happen on ordinary timeout.

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
must not be called from an ISR (unlike `i2s_mic_request_buffer()`, which is
dual-context-safe). There is no ISR-context rule set for it to follow,
because there is no ISR-context code path on the TX side at all.

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

**No underrun detection, by design — see below** for why this component
deliberately does not add one, and where an approximate signal for it
actually lives.

---

## On underrun/overrun tracking (and why it isn't in this component)

It's natural to expect `i2s_spk` to expose something like `i2s_mic`'s dual
overflow counters — one counter for a driver-level condition, one for an
application-pacing condition. The design document this component was built
from considers exactly that (Open Question 2) and explicitly decides
against it for V1, for a concrete reason: **ESP-IDF's I2S driver has no TX
underrun callback at all** (`on_sent`/`on_send_q_ovf` exist, but the
former just reports ordinary completions and the latter reports the
*opposite* condition — the application supplying data faster than it can
be sent, not slower). Without a driver hook to observe from, any
"underrun counter" the component exposed would be inferred, not measured
— and the implementation specification's explicit "Do Not Invent" list
(Section 13) calls out adding such a callback by name as something not to
do without stopping to ask first, precisely because it would present an
inference as if it were a fact ESP-IDF actually reported.

So this component's public API has no overflow/underrun callback, and
none of its internal state tracks one. What it does provide instead —
`*bytes_sent` on every `i2s_spk_send_buffer()` call — is the one genuine,
driver-reported signal available: whether the driver accepted everything
you asked it to in the time you gave it. **Both example projects track
this and log it periodically**, as the closest honest analogue to
`i2s_mic`'s counters:

- **`short_write_count`** — incremented whenever `*bytes_sent < buffer_len`
  on return, for *either* `ESP_OK` or `ESP_ERR_TIMEOUT`. This is a
  driver-confirmed signal (not a heuristic), directly and exclusively
  produced by the contract `i2s_spk_send_buffer()` already documents —
  no new component API was needed to expose it. With
  `write_timeout = portMAX_DELAY` (what both examples use), a nonzero
  count here would only happen due to a concurrent `stop()`, so it stays
  at zero in ordinary operation.
- **`underrun_estimate_count`** — a best-effort, application-level
  *heuristic*: the example times the gap between finishing one chunk's
  worth of `i2s_spk_send_buffer()` calls and starting the next, and flags
  it if that gap meaningfully exceeds the real-time playback duration of
  the audio already handed to the driver (which would mean TX DMA likely
  ran dry and `auto_clear` inserted silence before more data arrived).
  This is explicitly an estimate, not a measurement — there is no ESP-IDF
  hook confirming it actually happened, which is the same limitation the
  design document flags for the component itself. It is implemented at
  the application layer specifically because it's a timing inference
  about *this example's own producer loop*, not a fact about the
  component's or the driver's internal state — putting it in the
  component would misattribute an application-level guess as
  component-verified telemetry.

See either example's README ("What's actually happening") for where these
counters are logged.

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
   potentially long, `write_timeout`-bounded call would produce a
   different (undocumented) interleaving than the one the spec actually
   describes and relies on. Instead, `send_buffer()` uses the same
   unsynchronized "fully constructed" flag pattern `i2s_mic_request_buffer()`
   uses for its `UNINITIALIZED` check.
3. **`IRAM_ATTR` is not applied anywhere in this component.** Unlike
   `i2s_mic`, there is no ISR-context code path here at all — `i2s_spk`
   has no callback that ESP-IDF invokes from interrupt context — so the
   `CONFIG_I2S_ISR_IRAM_SAFE` constraint that shapes several of
   `i2s_mic`'s implementation choices simply does not apply to this
   component's own code. (It still applies to *ESP-IDF's own* internal TX
   ISR handling, which is outside this component's code entirely.)
4. **No underrun/overrun callback or counters in the component itself.**
   Discussed at length above — a deliberate decision required by the
   design document's own Open Question 2 and the implementation
   specification's "Do Not Invent" list, not an oversight.

Everything else — the state machine, the single-lock model, the
`auto_clear` configuration, and the byte-accounting formula for
`dma_buffer_size` — is implemented exactly as specified, with no invented
behavior beyond those four points.

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
- No software-level underrun detection (see "On underrun/overrun tracking"
  above) — only the application-level heuristic both examples implement.
- `I2S_SLOT_MODE_MONO` is not portable across chip families in practice —
  see "MONO slot_mode has been observed to behave differently across chip
  families" above. All examples in this distribution avoid it entirely by
  using STEREO with duplicated samples instead.

---

## Requirements

- ESP-IDF v6.0.x (v6.0–v6.0.3 verified against the pinned header behavior
  this component relies on; re-verify `i2s_channel_write`/`disable`/`enable`
  semantics against any later v6.0.x patch before upgrading).
- A target with `SOC_I2S_NUM >= 1` (see Chip Support above).

---

## License

MIT License — see LICENSE file.

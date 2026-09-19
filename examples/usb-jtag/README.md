# i2s_spk streaming example — USB-Serial-JTAG variant

**For ESP32-C3, ESP32-S3, ESP32-C6, and other boards with a native
USB-Serial-JTAG peripheral and no separate USB-to-UART bridge chip** — the
single USB cable you already flash/monitor through is used for the audio
stream too. No extra hardware needed.

If your board instead has a separate CP2102/CH340-style bridge chip (a
second, distinct COM port appears when you plug in — true of most classic
ESP32 devkits), use the sibling example **`i2s_spk_uart_bridge_example`**
instead. The two are not interchangeable: this one only works on chips
with a native USB-Serial-JTAG peripheral, and reuses the exact same cable
your programming/console connection already uses.

A PC-side Python script reads a WAV file, converts/resamples it to the
format the firmware expects, and streams it to the ESP32, which plays it
through an I2S DAC/amp using the `i2s_spk` component. Wired here to a
**MAX98357A** I2S class-D amplifier (drives a small speaker directly). See
"DAC/amp compatibility" below.

**Everything about the transport (handshake, sync bytes, header layout,
the chunk/ACK protocol) lives in this example, not in `i2s_spk` itself.**
The component only knows about I2S and buffers you hand it.

---

## Flow control: why this example isn't "just stream it"

`i2s_mic`'s examples stream continuously — the ESP is the sender, and the
PC just has to keep up. That pattern is reversed here: **the PC is the
sender, and the ESP's own I2S TX consumption rate is fixed by the sample
rate.** A PC script that just blasts a WAV file over serial as fast as
Python and the OS will allow has no reason to pace itself to 16 kHz on its
own, and would overflow the ESP's receive buffer (or the USB-Serial-JTAG
driver's internal RX ring) well before the audio finished playing.

This example uses an **explicit chunk/ACK protocol** instead: the PC sends
one bounded chunk (length-prefixed), then waits for a single ACK byte from
the ESP before sending the next. The ESP only sends that ACK after the
chunk has actually been handed off to `i2s_spk_send_buffer()` — a
*blocking* call that doesn't return until ESP-IDF's TX DMA has accepted
the data, which happens at the real playback rate. That blocking behavior
is what paces the whole protocol: the PC's effective send rate becomes
whatever the ESP can actually consume, automatically, with no timer or
rate limiter written anywhere.

Two alternatives were considered and rejected for this specific transport:

- **Hardware RTS/CTS flow control** — not applicable. A native
  USB-Serial-JTAG endpoint is a USB-CDC device, not a physical UART with
  RTS/CTS lines; there's nothing to wire up.
- **Just keep the bitrate low and buffer generously** — this only absorbs
  brief bursts. It doesn't fix a sustained, unpaced sender: without an
  actual pacing mechanism, the PC would eventually fill any finite buffer
  no matter its size, because nothing is slowing it down to match the
  speaker's fixed real-time consumption rate.

See the large comment block at the top of `main/app_main.c` for the full
reasoning, and `spk_audio_send.py` for the PC-side half of the protocol.

---

## Wiring

| MAX98357A pin | Example default (adjust for your board) |
|---|---|
| BCLK | GPIO 4 |
| LRC  | GPIO 5 |
| DIN  | GPIO 6 |
| GAIN | Floating (default ~9 dB gain) |
| SD   | Floating (selects mono (L+R)/2 mixdown — matches this example's true-mono output) |
| VIN  | 5V (or 3.3V, with less output power) |
| GND  | GND |

Change `GPIO_BCK` / `GPIO_WS` / `GPIO_DATA` in `main/app_main.c` if your
board uses different pins. **Don't tie SD to GND** (that disables the
amp) or to VIN (that selects right-channel-only, which plays silence
against this example's mono source) — leave it floating.

## DAC/amp compatibility

`i2s_spk` has no part-specific logic at all — it's a generic I2S
transmitter that writes whatever you hand it. This example is wired to a
**MAX98357A** because it needs only 3 signal lines and drives a speaker
directly with no separate amplifier stage, mirroring how INMP441 is used
as `i2s_mic`'s simplest reference part. A **PCM5102** or **UDA1334A** I2S
DAC into a headphone jack should work identically — same standard Philips
I2S format, same 3 signal lines, just line-level output instead of a
driven speaker. If you try one, that's useful information worth
contributing back.

## Build and flash

This example vendors a local copy of `i2s_spk` under `components/i2s_spk/`
(via `EXTRA_COMPONENT_DIRS` pointing at the component root) so it builds
standalone without needing the component published to the ESP Component
Registry first. Requires ESP-IDF v6.0.x.

```bash
idf.py set-target esp32c3       # or esp32s3 / esp32c6 — must match your actual board
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Double-check `set-target` matches your board: building for the wrong
target produces linker errors for peripheral-specific symbols (like
`usb_serial_jtag_*`) that look like a missing dependency but are actually
a target mismatch.

Once you see `=== READY, waiting for trigger ===`, quit the monitor
(Ctrl+], or Ctrl+T then Ctrl+X) so the PC script can open the port itself —
the ESP32 and the monitor can't both hold it at once.

## Run the PC-side script

```bash
pip install pyserial numpy

python spk_audio_send.py --port COM5 --wav song.wav
```

(`COM5` on Windows; `/dev/ttyACM0` or similar on Linux/macOS.)

The script resamples/converts the WAV to 16 kHz / 16-bit / mono
automatically, regardless of the source file's own rate, bit depth, or
channel count — see `load_wav_as_target_pcm()` in `spk_audio_send.py`.

You can re-run the script (without resetting the board) to play another
file: the firmware goes back to waiting for a trigger after each
completed stream.

## What's actually happening

1. `main/app_main.c` configures `i2s_spk` for 16 kHz, mono, 16-bit
   samples and starts it immediately at boot (`i2s_spk_start()`) — the
   channel stays enabled across playback sessions; only the host handshake
   repeats.
2. `host_comm_init()` calls `usb_serial_jtag_driver_install()` to get
   interrupt-driven, buffered reads and writes on top of the same
   USB-Serial-JTAG cable that already carries the console's log output —
   these coexist by design, same as in `i2s_mic`'s examples.
3. The trigger read loops until it sees the specific `TRIGGER_BYTE` value
   (`0xA5`) rather than accepting the first byte it gets, for the same
   reason `i2s_mic`'s examples validate their trigger: a stray or noisy
   byte accepted prematurely would desync the one-shot header that
   follows.
4. Once triggered, logging is disabled (any log line during the binary
   protocol would land inside it and desync the PC script's parser) and
   the `audio_header_t` is sent once.
5. The chunk/ACK loop then runs until the PC sends a 0-length chunk as an
   end-of-stream marker (see "Flow control" above for the protocol
   itself). Each chunk is handed to `i2s_spk_send_buffer()` in a loop that
   accounts for partial transfers (see `i2s_spk.h`'s doc comment on why a
   single call isn't guaranteed to consume the whole buffer), then ACKed.
6. Logging is re-enabled once the end marker is seen, and a one-line
   summary is printed: chunks played, `short_write_count` (times
   `i2s_spk_send_buffer()` didn't fully consume what it was given —
   driver-confirmed, not a heuristic), and `underrun_estimate_count` (a
   best-effort, timing-based *guess* at how many times TX DMA likely ran
   dry waiting for the next chunk — see `i2s_spk`'s README, "On
   underrun/overrun tracking," for why this can only be an estimate and
   why it lives here in the example rather than in the component).
7. The firmware then goes back to waiting for the next trigger.

## Known limitations of this example (not of `i2s_spk`)

- One playback session at a time — no mixing, no queueing multiple files.
- No compression, no format negotiation beyond the fixed 16 kHz/16-bit/
  mono the PC script always converts to.
- `underrun_estimate_count` is exactly that — an estimate, not a
  hardware-confirmed count. See `i2s_spk`'s README for why no confirmed
  signal exists in V1.
- Sharing the console channel for both logging and the audio stream means
  you lose console log visibility for the duration of each playback
  session.

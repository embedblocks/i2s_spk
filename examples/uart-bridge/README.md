# i2s_spk streaming example — UART-bridge variant

**For classic ESP32 (and other boards) with a separate USB-to-UART bridge
chip** — a second, distinct COM port appears when you plug in, wired to
the chip's real UART0 pins (true of most classic ESP32 devkits, using an
onboard CP2102, CH340, FTDI, etc.). This example reuses that same UART0 —
the one your programming/console cable already talks to — for the audio
stream too. No extra hardware needed, same single cable.

If your board instead has no separate bridge chip (the cable you flash
through is the chip's native USB-Serial-JTAG peripheral — common on
ESP32-C3/S3/C6 devkits), use the sibling example
**`i2s_spk_usb_jtag_example`** instead. The two are not interchangeable:
this one needs a real UART with a negotiable baud rate, which a native
USB-CDC endpoint is not.

A PC-side Python script reads a WAV file, converts/resamples it to the
format the firmware expects, and streams it to the ESP32, which plays it
through an I2S DAC/amp using the `i2s_spk` component. Wired here to a
**MAX98357A** I2S class-D amplifier. See "DAC/amp compatibility" below.

**Everything about the transport (handshake, sync bytes, header layout,
the chunk/ACK protocol) lives in this example, not in `i2s_spk` itself.**
The component only knows about I2S and buffers you hand it.

---

## Flow control: why this example isn't "just stream it"

Same underlying reason as the USB-Serial-JTAG variant — see its README for
the full writeup, and the large comment block at the top of this example's
`main/app_main.c`. In short: the PC is the sender here, and the ESP's own
I2S TX consumption rate is fixed by the sample rate, so an unpaced sender
would eventually overflow UART0's RX ring buffer no matter how large it's
sized. This example uses the same chunk/ACK protocol as the
USB-Serial-JTAG variant: the PC sends one bounded, length-prefixed chunk,
waits for a single ACK byte (sent only after that chunk has been handed
off to the real-time-blocking `i2s_spk_send_buffer()`), then sends the
next.

**A note on hardware flow control specifically:** this variant is a real
UART, so RTS/CTS flow control is at least electrically conceivable here in
a way it isn't for the USB-Serial-JTAG variant. It still isn't used,
because most classic ESP32 devkits' onboard bridge chips only expose
TX/RX/DTR/RTS to the host — with RTS/DTR already wired to drive the
EN/IO0 auto-reset sequence, not available as genuine flow-control signals.
Depending on RTS/CTS would make this example work only on the subset of
boards wired for it, and hang silently on everything else. The chunk/ACK
protocol works on any board with a plain 2-wire (TX/RX) UART bridge — the
common case — which is why it's used here too, identically to the
USB-Serial-JTAG variant.

**Bandwidth check:** 16 kHz × 16-bit × mono = 32,000 bytes/s of audio
payload, comfortably under 460800 baud's ~46,000 bytes/s raw capacity —
but note that raw transport headroom isn't actually what makes this
protocol safe; the chunk/ACK pacing is. The headroom just means each
chunk's round trip (send ~4 KB, wait for one ACK byte) is quick compared
to the ~128ms a 4096-byte mono 16-bit chunk takes to actually play, so the
protocol's pace ends up set by the speaker, not the wire, well before the
wire itself becomes a limiting factor.

---

## Wiring

| MAX98357A pin | Example default (adjust for your board) |
|---|---|
| BCLK | GPIO 25 |
| LRC  | GPIO 26 |
| DIN  | GPIO 27 |
| GAIN | Floating (default ~9 dB gain) |
| SD   | Floating (selects mono (L+R)/2 mixdown) |
| VIN  | 5V (or 3.3V, less output power) |
| GND  | GND |

These GPIO defaults are classic-ESP32 numbers, chosen to avoid UART0's own
pins (GPIO1 TX / GPIO3 RX) and the usual SPI-flash pin range. **Don't
assume this pin mapping carries over to other chips** — ESP32-C3's UART0
default, for example, is GPIO21 TX / GPIO20 RX, not GPIO1/GPIO3. Change
`GPIO_BCK` / `GPIO_WS` / `GPIO_DATA` in `main/app_main.c` for your board.

## DAC/amp compatibility

`i2s_spk` has no part-specific logic at all — it's a generic I2S
transmitter. This example is wired to a **MAX98357A** for the same reason
as the USB-Serial-JTAG variant: 3 signal lines, drives a speaker directly.
A **PCM5102** or **UDA1334A** I2S DAC into a headphone jack should work
identically — same standard Philips I2S format, same 3 signal lines.

## Build and flash

This example vendors a local copy of `i2s_spk` under `components/i2s_spk/`
(via `EXTRA_COMPONENT_DIRS`) so it builds standalone without needing the
component published to the ESP Component Registry first. Requires
ESP-IDF v6.0.x.

```bash
idf.py set-target esp32       # or whichever target has your bridge chip
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Double-check `set-target` matches your actual board — building for the
wrong target produces linker errors for peripheral-specific symbols that
look like a missing dependency but are actually a target mismatch.

Once you see `=== READY, waiting for trigger ===`, quit the monitor
(Ctrl+], or Ctrl+T then Ctrl+X) so the PC script can open the port itself —
the ESP32 and the monitor can't both hold it at once.

## Run the PC-side script

```bash
pip install pyserial numpy

python spk_audio_send.py --port COM5 --wav song.wav
```

(`COM5` on Windows; `/dev/ttyUSB0` or similar on Linux/macOS.)

You can re-run the script (without resetting the board) to play another
file: the firmware goes back to waiting for a trigger, at the safe
handshake baud rate, after each completed stream.

## What's actually happening

1. `main/app_main.c` configures `i2s_spk` for 16 kHz, mono, 16-bit
   samples and starts it immediately at boot; the channel stays enabled
   across playback sessions — only the host handshake repeats.
2. `host_comm_init()` reuses **UART0** — the same peripheral the console
   and programming connection already use — via `uart_vfs_dev_use_driver()`
   to take over its full interrupt-driven driver, initially at a safe
   `HANDSHAKE_BAUD` (115200).
3. The trigger read loops until it sees the specific `TRIGGER_BYTE` value
   (`0xA5`) rather than accepting the first byte received, so a stray or
   noisy byte can't desync the one-shot header that follows.
4. Once triggered, UART0 switches to `STREAM_BAUD` (460800 — see "Flow
   control" above for why not 921600), logging is disabled, and the
   `audio_header_t` is sent once.
5. The chunk/ACK loop runs until the PC sends a 0-length chunk as an
   end-of-stream marker. Each chunk is handed to `i2s_spk_send_buffer()`
   in a loop that accounts for partial transfers, then ACKed.
6. Logging is re-enabled and a summary is printed once the end marker is
   seen: chunks played, `short_write_count` (driver-confirmed partial
   transfers — see `i2s_spk`'s README), and `underrun_estimate_count` (a
   best-effort, timing-based estimate — also see `i2s_spk`'s README, "On
   underrun/overrun tracking," for why this can only be an estimate).
7. UART0 drops back to `HANDSHAKE_BAUD` and the firmware waits for the
   next trigger.

## Known limitations of this example (not of `i2s_spk`)

- One playback session at a time — no mixing, no queueing multiple files.
- No compression, no format negotiation beyond the fixed 16 kHz/16-bit/
  mono the PC script always converts to.
- `underrun_estimate_count` is an estimate, not a hardware-confirmed
  count — see `i2s_spk`'s README for why no confirmed signal exists in V1.
- Sharing UART0 for both the ESP-IDF console and the audio stream means
  you lose console log visibility for the duration of each playback
  session.

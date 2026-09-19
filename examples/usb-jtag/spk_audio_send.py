#!/usr/bin/env python3
"""
spk_audio_send.py — companion PC-side script for the i2s_spk streaming
example (USB-Serial-JTAG variant, for boards with no separate UART bridge
chip — e.g. most ESP32-C3 devkits with a single USB port).

Reads a WAV file, converts/resamples it to the exact format the firmware
expects (16 kHz, 16-bit, mono), and streams it to the ESP32 using a
chunked, ACK-paced protocol: send one bounded chunk, wait for the ESP to
acknowledge it (meaning that chunk has actually been handed off to
i2s_spk_send_buffer(), a call paced by real playback time), then send the
next. This is the flow-control mechanism this example relies on — see the
big comment block at the top of the firmware's main/app_main.c for why a
plain "stream continuously" approach (as used by i2s_mic's examples) does
not work in this direction: here, the PC is the sender and the ESP's
consumption rate is fixed by the sample rate, so an unpaced sender would
eventually overflow the ESP's receive buffer no matter how large it is.

The wire format (handshake sequence, sync bytes, header layout, and the
length-prefixed chunk/ACK protocol) is defined jointly by this script and
the ESP32 example's app_main.c. The i2s_spk *component* has no involvement
in or knowledge of any of this — it only accepts filled buffers inside the
firmware.

There's no baud rate to negotiate here: the ESP's USB-Serial-JTAG endpoint
is a real USB CDC device, not a UART, so pyserial's `baudrate` is accepted
but ignored by the firmware. If your board instead has a separate
CP2102/CH340-style UART bridge (a second, distinct COM port appears when
you plug in), use the sibling `examples/uart-bridge` example and its
script instead, which does negotiate a faster baud rate for streaming.

Usage:
    python spk_audio_send.py --port COM5 --wav song.wav

Dependencies:
    pip install pyserial numpy
"""

import argparse
import struct
import sys
import time
import wave

import numpy as np
import serial

SYNC_BYTE = 0xAA
MAGIC = 0xC0FFEE02
TRIGGER_BYTE = 0xA5   # must match TRIGGER_BYTE in main/app_main.c
ACK_BYTE = 0x06        # must match ACK_BYTE in main/app_main.c
SERIAL_BAUD = 115200   # ignored by native USB-CDC; pyserial's API requires a value anyway
CHUNK_BYTES = 4096     # must match CHUNK_BYTES in main/app_main.c

TARGET_SAMPLE_RATE = 16000
TARGET_SAMPLE_WIDTH = 2   # bytes (16-bit)
TARGET_CHANNELS = 1       # mono

# audio_header_t on the wire: sync[3] + magic(u32) + sample_rate(u32)
#                              + bits_per_sample(u16) + channel_count(u8)
HEADER_TAIL_FMT = "<IHB"          # everything after the 3-byte sync + 4-byte magic
HEADER_TAIL_SIZE = struct.calcsize(HEADER_TAIL_FMT)


# --------------------------------------------------------------------------
# WAV loading / conversion — no dependency on the deprecated `audioop`
# module. Handles 8/16/24/32-bit source WAVs, any channel count, and any
# source sample rate via simple linear-interpolation resampling.
# --------------------------------------------------------------------------

def load_wav_as_target_pcm(path):
    with wave.open(path, "rb") as wf:
        n_channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        n_frames = wf.getnframes()
        raw = wf.readframes(n_frames)

    if n_frames == 0:
        raise ValueError(f"{path}: WAV file has no audio frames")

    if sampwidth == 1:
        # WAV's 8-bit PCM is unsigned, centered at 128.
        data = np.frombuffer(raw, dtype=np.uint8).astype(np.int32)
        data = (data - 128) << 8
    elif sampwidth == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.int32)
    elif sampwidth == 3:
        packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        as_i32 = (
            packed[:, 0].astype(np.int32)
            | (packed[:, 1].astype(np.int32) << 8)
            | (packed[:, 2].astype(np.int32) << 16)
        )
        as_i32 = np.where(as_i32 & 0x800000, as_i32 - 0x1000000, as_i32)  # sign-extend 24-bit
        data = as_i32 >> 8   # scale 24-bit range down to 16-bit range
    elif sampwidth == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.int64)
        data = (data >> 16).astype(np.int32)   # scale 32-bit range down to 16-bit range
    else:
        raise ValueError(f"{path}: unsupported WAV sample width ({sampwidth} bytes)")

    if n_channels > 1:
        data = data.reshape(-1, n_channels).astype(np.float64)
        data = data.mean(axis=1)   # downmix to mono by averaging channels
    else:
        data = data.astype(np.float64)

    if framerate != TARGET_SAMPLE_RATE:
        duration_s = len(data) / framerate
        n_target = max(1, int(round(duration_s * TARGET_SAMPLE_RATE)))
        x_old = np.linspace(0.0, duration_s, num=len(data), endpoint=False)
        x_new = np.linspace(0.0, duration_s, num=n_target, endpoint=False)
        data = np.interp(x_new, x_old, data)
        print(f"  Resampled {framerate} Hz -> {TARGET_SAMPLE_RATE} Hz ({len(data)} samples)")

    data = np.clip(data, -32768, 32767).astype("<i2")
    return data.tobytes(), n_channels, sampwidth, framerate


# --------------------------------------------------------------------------
# Handshake
# --------------------------------------------------------------------------

def wait_for_ready(ser):
    """Wait until the ESP prints its READY banner before sending anything.

    This matters more than it looks: opening a serial port can itself
    trigger a board reset on many USB-serial bridge boards, and even
    without that, the firmware takes real, non-zero time after boot (or
    after finishing a previous playback session) to reach the point where
    it's actually listening for the trigger byte. Sending the trigger
    before that point means it's simply lost forever — there's no retry on
    the firmware side — and everything downstream then hangs waiting for a
    header that will never arrive.
    """
    print("[0] Waiting for READY...")
    old_timeout = ser.timeout
    ser.timeout = 30
    while True:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            print(f"  ESP: {line}")
        if "READY" in line:
            break
    ser.reset_input_buffer()
    ser.timeout = old_timeout


def send_trigger(ser):
    print("[1] Sending trigger...")
    ser.write(bytes([TRIGGER_BYTE]))
    ser.flush()


def read_exact(ser, size):
    data = bytearray()
    start = time.time()
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            if time.time() - start > 10:
                raise TimeoutError(f"Timeout waiting for {size} bytes: {len(data)}/{size}")
            continue
        data.extend(chunk)
    return bytes(data)


def find_sync_and_magic(ser):
    """Scan byte-by-byte for 0xAA 0xAA 0xAA followed by the 4-byte magic."""
    search_buf = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            raise TimeoutError("No sync pattern received")
        search_buf.append(b[0])
        if len(search_buf) >= 7:
            tail = search_buf[-7:]
            if (
                tail[0] == SYNC_BYTE
                and tail[1] == SYNC_BYTE
                and tail[2] == SYNC_BYTE
                and struct.unpack("<I", tail[3:7])[0] == MAGIC
            ):
                return


def read_header(ser):
    find_sync_and_magic(ser)
    rest = read_exact(ser, HEADER_TAIL_SIZE)
    sample_rate, bits_per_sample, channel_count = struct.unpack(HEADER_TAIL_FMT, rest)
    return sample_rate, bits_per_sample, channel_count


# --------------------------------------------------------------------------
# Chunked, ACK-paced streaming
# --------------------------------------------------------------------------

def send_chunk(ser, payload):
    ser.write(struct.pack("<I", len(payload)))
    ser.write(payload)
    ser.flush()

    # Validate the ACK against the specific expected value, rather than
    # accepting any byte — the same defensive principle i2s_mic's examples
    # apply to their trigger byte, applied here to the ACK direction. A
    # stray or corrupted byte accepted as an ACK would let this script race
    # ahead of the firmware's actual playback progress, defeating the whole
    # point of the protocol.
    ack = read_exact(ser, 1)
    if ack[0] != ACK_BYTE:
        raise RuntimeError(
            f"Expected ACK 0x{ACK_BYTE:02X}, got 0x{ack[0]:02X} — protocol desync, aborting"
        )


def send_stream(ser, pcm_bytes):
    total = len(pcm_bytes)
    sent = 0
    last_pct = -1
    while sent < total:
        payload = pcm_bytes[sent:sent + CHUNK_BYTES]
        send_chunk(ser, payload)
        sent += len(payload)
        pct = int(100 * sent / total)
        if pct != last_pct:
            print(f"\r[3] Sending... {pct}%", end="", flush=True)
            last_pct = pct
    print()

    # 0-length chunk marks end-of-stream; no ACK follows it.
    ser.write(struct.pack("<I", 0))
    ser.flush()
    print("[4] Done — end-of-stream marker sent")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("--wav", required=True, help="Path to a WAV file to play")
    args = ap.parse_args()

    print(f"[-] Loading {args.wav}...")
    pcm_bytes, src_channels, src_width, src_rate = load_wav_as_target_pcm(args.wav)
    duration_s = len(pcm_bytes) / (TARGET_SAMPLE_RATE * TARGET_SAMPLE_WIDTH * TARGET_CHANNELS)
    print(f"  Source: {src_rate} Hz, {src_width * 8}-bit, {src_channels} ch")
    print(f"  Converted: {TARGET_SAMPLE_RATE} Hz, 16-bit, mono, {duration_s:.1f}s, "
          f"{len(pcm_bytes)} bytes")

    ser = serial.Serial(args.port, SERIAL_BAUD, timeout=30)
    ser.dtr = False
    ser.rts = False

    wait_for_ready(ser)
    send_trigger(ser)

    print("[2] Waiting for header...")
    sample_rate, bits_per_sample, channel_count = read_header(ser)
    print(f"  Header: {sample_rate} Hz, {bits_per_sample}-bit, {channel_count} ch")

    if (sample_rate, bits_per_sample, channel_count) != (
        TARGET_SAMPLE_RATE, TARGET_SAMPLE_WIDTH * 8, TARGET_CHANNELS
    ):
        print("Warning: firmware header doesn't match this script's assumed format; "
              "sending anyway, but playback will likely sound wrong if it doesn't match.")

    ser.timeout = 5   # each chunk's ACK should arrive quickly once accepted
    send_stream(ser, pcm_bytes)


if __name__ == "__main__":
    main()

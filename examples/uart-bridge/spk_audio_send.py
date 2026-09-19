#!/usr/bin/env python3
"""
spk_audio_send.py — companion PC-side script for the i2s_spk UART-bridge
streaming example (for boards with a separate CP2102/CH340-style USB-to-
UART bridge chip — a second, distinct COM port appears when you plug in,
true of most classic ESP32 devkits).

Reads a WAV file, converts/resamples it to the exact format the firmware
expects (16 kHz, 16-bit, mono), and streams it to the ESP32 using a
chunked, ACK-paced protocol — see the big comment block at the top of the
firmware's main/app_main.c for why this is needed (the PC is the sender
here, and the ESP's own I2S TX consumption rate is fixed by the sample
rate, so an unpaced sender would eventually overflow UART0's RX buffer no
matter how large it is).

If your board has no separate bridge chip (the cable you flash through is
the chip's native USB-Serial-JTAG peripheral), use the sibling
`i2s_spk_usb_jtag_example`'s script instead — that one has no baud switch,
since a native USB-CDC endpoint doesn't have a real baud rate to negotiate,
unlike this real UART.

The wire format (handshake sequence, sync bytes, header layout, and the
length-prefixed chunk/ACK protocol) is defined jointly by this script and
the ESP32 example's app_main.c. The i2s_spk *component* has no involvement
in or knowledge of any of this — it only accepts filled buffers inside the
firmware.

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
TRIGGER_BYTE = 0xA5    # must match TRIGGER_BYTE in main/app_main.c
ACK_BYTE = 0x06         # must match ACK_BYTE in main/app_main.c
HANDSHAKE_BAUD = 115200
STREAM_BAUD = 460800
CHUNK_BYTES = 4096      # must match CHUNK_BYTES in main/app_main.c

TARGET_SAMPLE_RATE = 16000
TARGET_SAMPLE_WIDTH = 2   # bytes (16-bit)
TARGET_CHANNELS = 1       # mono

HEADER_TAIL_FMT = "<IHB"
HEADER_TAIL_SIZE = struct.calcsize(HEADER_TAIL_FMT)


# --------------------------------------------------------------------------
# WAV loading / conversion — identical to the USB-Serial-JTAG variant's
# script; duplicated here rather than shared, matching how i2s_mic's two
# examples each keep their own independent, self-contained PC-side script.
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
        as_i32 = np.where(as_i32 & 0x800000, as_i32 - 0x1000000, as_i32)
        data = as_i32 >> 8
    elif sampwidth == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.int64)
        data = (data >> 16).astype(np.int32)
    else:
        raise ValueError(f"{path}: unsupported WAV sample width ({sampwidth} bytes)")

    if n_channels > 1:
        data = data.reshape(-1, n_channels).astype(np.float64)
        data = data.mean(axis=1)
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
    trigger a board reset on many USB-serial bridge boards (DTR/RTS tied
    to EN), and even without that, the firmware takes real, non-zero time
    after boot (or after finishing a previous session) to reach the point
    where it's actually listening for the trigger byte. Sending the
    trigger before that point means it's simply lost forever — there's no
    retry on the firmware side — and everything downstream then hangs
    waiting for a header that will never arrive.
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

    ser.write(struct.pack("<I", 0))
    ser.flush()
    print("[4] Done — end-of-stream marker sent")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--wav", required=True, help="Path to a WAV file to play")
    args = ap.parse_args()

    print(f"[-] Loading {args.wav}...")
    pcm_bytes, src_channels, src_width, src_rate = load_wav_as_target_pcm(args.wav)
    duration_s = len(pcm_bytes) / (TARGET_SAMPLE_RATE * TARGET_SAMPLE_WIDTH * TARGET_CHANNELS)
    print(f"  Source: {src_rate} Hz, {src_width * 8}-bit, {src_channels} ch")
    print(f"  Converted: {TARGET_SAMPLE_RATE} Hz, 16-bit, mono, {duration_s:.1f}s, "
          f"{len(pcm_bytes)} bytes")

    ser = serial.Serial(args.port, HANDSHAKE_BAUD, timeout=30)
    ser.dtr = False
    ser.rts = False

    wait_for_ready(ser)
    send_trigger(ser)

    # Must wait longer than the ESP's own 600ms delay before it reconfigures
    # UART0's baud rate, or we'll flip ours too early and desync.
    time.sleep(0.7)
    ser.reset_input_buffer()
    ser.baudrate = STREAM_BAUD
    time.sleep(0.1)

    print(f"[2] Switched to {STREAM_BAUD} baud — waiting for header...")
    sample_rate, bits_per_sample, channel_count = read_header(ser)
    print(f"  Header: {sample_rate} Hz, {bits_per_sample}-bit, {channel_count} ch")

    if (sample_rate, bits_per_sample, channel_count) != (
        TARGET_SAMPLE_RATE, TARGET_SAMPLE_WIDTH * 8, TARGET_CHANNELS
    ):
        print("Warning: firmware header doesn't match this script's assumed format; "
              "sending anyway, but playback will likely sound wrong if it doesn't match.")

    ser.timeout = 5
    send_stream(ser, pcm_bytes)


if __name__ == "__main__":
    main()

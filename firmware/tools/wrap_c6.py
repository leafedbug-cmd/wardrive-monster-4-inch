#!/usr/bin/env python3
"""Wrap the ESP32-C6 slave image in a small container.

Why this exists: the C6 image is stored as *data* in the P4's `c6fw`
partition, but it is still a valid ESP firmware image. esptool inspects every
file it is asked to write, sees the 0xE9 magic and a chip ID that is not
ESP32-P4, and refuses:

    A fatal error occurred: ...wardrive_c6_slave.bin is not an ESP32-P4 image.

Prepending 16 bytes hides the magic from that check, and is more honest than
passing --force (which would also disable the check for the real P4 images).
c6_ota.c skips the header when it reads the partition.

Layout:
    0   4   magic  "C6FW"
    4   4   uint32 container version (1)
    8   4   uint32 payload length
    12  4   uint32 reserved / padding
    16  ..  the ESP32-C6 image verbatim
"""
import struct
import sys

MAGIC = b"C6FW"
VERSION = 1
HEADER_LEN = 16


def main(argv):
    if len(argv) != 3:
        print("usage: wrap_c6.py <in.bin> <out.img>", file=sys.stderr)
        return 2

    with open(argv[1], "rb") as fh:
        payload = fh.read()

    if not payload:
        print("wrap_c6: input is empty", file=sys.stderr)
        return 1
    if payload[0] != 0xE9:
        print("wrap_c6: warning: input does not start with 0xE9; "
              "is this really an ESP image?", file=sys.stderr)

    header = MAGIC + struct.pack("<III", VERSION, len(payload), 0)
    assert len(header) == HEADER_LEN

    with open(argv[2], "wb") as fh:
        fh.write(header)
        fh.write(payload)

    print(f"wrap_c6: {len(payload)} bytes -> {argv[2]} "
          f"({len(payload) + HEADER_LEN} total)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

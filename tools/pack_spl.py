#!/usr/bin/env python3
"""Pack checked sunxi SPL images into the ESP32 d1s_spl partition."""

import argparse
from pathlib import Path
import struct
import tempfile


SLOT_SIZE = 128 * 1024
SLOT_COUNT = 2
PARTITION_SIZE = 512 * 1024
CHECKSUM_STAMP = 0x5F0A6C39


def validate_spl(data: bytes, name: str = "SPL") -> bytes:
    if len(data) < 20:
        raise ValueError(f"{name}: truncated SPL header")
    if data[4:12] != b"eGON.BT0":
        raise ValueError(f"{name}: missing eGON.BT0 magic at offset 4")
    checksum, length = struct.unpack_from("<II", data, 12)
    if length == 0 or length % 1024 or length > SLOT_SIZE:
        raise ValueError(f"{name}: SPL length must be 1024-aligned and 1..{SLOT_SIZE} bytes")
    if len(data) < length:
        raise ValueError(f"{name}: header length {length} exceeds file size {len(data)}")
    if any(byte not in (0x00, 0xFF) for byte in data[length:]):
        raise ValueError(f"{name}: non-padding data after SPL; use sunxi-spl.bin, not a combined boot image")
    words = struct.unpack(f"<{length // 4}I", data[:length])
    expected = (sum(words) - checksum + CHECKSUM_STAMP) & 0xFFFFFFFF
    if checksum != expected:
        raise ValueError(f"{name}: checksum mismatch (stored 0x{checksum:08x}, expected 0x{expected:08x})")
    return data[:length]


def pack_spls(images: list[bytes]) -> bytes:
    if not 1 <= len(images) <= SLOT_COUNT:
        raise ValueError(f"provide 1..{SLOT_COUNT} SPL images")
    if len(images) == 1:
        images = images * SLOT_COUNT
    partition = bytearray(b"\xff" * PARTITION_SIZE)
    for slot, data in enumerate(images):
        spl = validate_spl(data, f"slot {slot}")
        start = slot * SLOT_SIZE
        partition[start:start + len(spl)] = spl
    return bytes(partition)


def partition_offset(value: str) -> int:
    try:
        offset = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected an integer, e.g. 0x320000") from error
    if offset < 0:
        raise argparse.ArgumentTypeError("partition offset must be nonnegative")
    return offset


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spl", nargs="+", type=Path, help="1..2 normal sunxi-spl.bin images; a single image fills both slots")
    parser.add_argument("-o", "--output", required=True, type=Path, help="512 KiB raw partition image")
    parser.add_argument("--partition-offset", type=partition_offset, default=0x320000,
                        help="ESP32 flash offset used only in printed instructions (default: 0x320000)")
    args = parser.parse_args()
    temporary = None
    try:
        if args.output.resolve() in (path.resolve() for path in args.spl):
            raise ValueError("output must not overwrite an input SPL")
        if not 1 <= len(args.spl) <= SLOT_COUNT:
            raise ValueError(f"provide 1..{SLOT_COUNT} SPL images")
        images = [validate_spl(path.read_bytes(), str(path)) for path in args.spl]
        if len(images) == 1:
            images *= SLOT_COUNT
        packed = pack_spls(images)
        # Replace only after validation and a complete write so errors preserve an existing image.
        with tempfile.NamedTemporaryFile(dir=args.output.parent, prefix=f".{args.output.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(packed)
        temporary.replace(args.output)
        temporary = None
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"Wrote {args.output}: {PARTITION_SIZE} bytes, {len(images)} occupied slot(s)")
    for index, spl in enumerate(images):
        print(f"  slot {index}: partition +0x{index * SLOT_SIZE:05x}, SPL {len(spl)} bytes")
    print(f"Flash this raw image to ESP32 d1s_spl at offset 0x{args.partition_offset:x}; no flashing was performed.")


if __name__ == "__main__":
    main()

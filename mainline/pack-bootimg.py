#!/usr/bin/env python3
"""Pack an Android boot image for the Xiaomi Pad 5 Pro 5G (enuma).

The stock boot.img on this device is a header-v1 image with an empty cmdline,
`kernel_addr = 0x8000`, `ramdisk_addr = 0x1000000`, `tags_addr = 0x1e00000`
and page size 4096, and the device tree is appended to the kernel blob rather
than being carried in a separate dtb section.  Those are the values reproduced
here; anything else has to be justified by a header dump of a known-booting
image.
"""

import argparse
import hashlib
import struct
import sys

PAGE = 4096

# Field layout from include/uapi/linux/.../bootimg.h (AOSP).
# v0 header = 1632 bytes, v1 adds recovery_dtbo_size/offset + header_size = 1648.
HDR_FMT = "<8s10I16s512s32s1024s"
HDR_V0_LEN = struct.calcsize(HDR_FMT)
V1_TAIL_FMT = "<IQI"
V1_TAIL_LEN = struct.calcsize(V1_TAIL_FMT)


def pad(data: bytes, page: int = PAGE) -> bytes:
    rem = len(data) % page
    return data if rem == 0 else data + b"\0" * (page - rem)


def build_header(*, kernel_size: int, kernel_addr: int, ramdisk_size: int,
                 ramdisk_addr: int, second_addr: int, tags_addr: int,
                 page_size: int, version: int, cmdline: str,
                 name: str) -> bytes:
    hdr = struct.pack(
        HDR_FMT,
        b"ANDROID!",
        kernel_size,
        kernel_addr,
        ramdisk_size,
        ramdisk_addr,
        0,              # second_size
        second_addr,
        tags_addr,
        page_size,
        version,
        0,              # os_version
        name.encode()[:15],
        cmdline.encode()[:511],
        b"\0" * 32,     # id
        b"",            # extra_cmdline
    )
    assert len(hdr) == HDR_V0_LEN
    if version == 0:
        return hdr
    if version != 1:
        raise SystemExit("only header v0/v1 are used on this device")
    hdr += struct.pack(V1_TAIL_FMT, 0, 0, HDR_V0_LEN + V1_TAIL_LEN)
    assert len(hdr) == HDR_V0_LEN + V1_TAIL_LEN
    return hdr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True, help="Image.gz (DTB already appended)")
    ap.add_argument("--ramdisk", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dtb", help="appended to the kernel blob if given")
    ap.add_argument("--cmdline", default="",
                    help="boot header cmdline; the kernel builtin one is authoritative")
    ap.add_argument("--name", default="enuma-mainline")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0x00000000)
    ap.add_argument("--kernel-offset", type=lambda v: int(v, 0), default=0x00008000)
    ap.add_argument("--ramdisk-offset", type=lambda v: int(v, 0), default=0x01000000)
    ap.add_argument("--second-offset", type=lambda v: int(v, 0), default=0x00F00000)
    ap.add_argument("--tags-offset", type=lambda v: int(v, 0), default=0x01E00000)
    ap.add_argument("--header-version", type=int, default=1)
    ap.add_argument("--pagesize", type=int, default=PAGE)
    args = ap.parse_args()

    kernel = open(args.kernel, "rb").read()
    if args.dtb:
        kernel += open(args.dtb, "rb").read()
    ramdisk = open(args.ramdisk, "rb").read()
    if not kernel:
        raise SystemExit("empty kernel")

    page = args.pagesize
    hdr = build_header(
        kernel_size=len(kernel),
        kernel_addr=args.base + args.kernel_offset,
        ramdisk_size=len(ramdisk),
        ramdisk_addr=args.base + args.ramdisk_offset,
        second_addr=args.base + args.second_offset,
        tags_addr=args.base + args.tags_offset,
        page_size=page,
        version=args.header_version,
        cmdline=args.cmdline,
        name=args.name,
    )

    blob = pad(hdr, page) + pad(kernel, page) + pad(ramdisk, page)

    with open(args.out, "wb") as fh:
        fh.write(blob)

    print(f"header   v{args.header_version} {len(hdr)} bytes")
    print(f"kernel   {len(kernel):>10} bytes  @ {args.base + args.kernel_offset:#x}")
    print(f"ramdisk  {len(ramdisk):>10} bytes  @ {args.base + args.ramdisk_offset:#x}")
    print(f"tags     {'':>10}          @ {args.base + args.tags_offset:#x}")
    print(f"cmdline  {args.cmdline!r}")
    print(f"image    {len(blob):>10} bytes  sha256 {hashlib.sha256(blob).hexdigest()}")
    print(f"written  {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
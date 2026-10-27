#!/usr/bin/env python3
"""Minimal Android boot image v1 writer using the A30s stock parameters.

Parameters come from the A30s TWRP/OrangeFox device trees:
  base 0x10000000, kernel_offset 0x8000, ramdisk_offset 0x01000000,
  tags_offset 0x100, pagesize 2048, header_version 1.
Also appends the Samsung "SEANDROIDENFORCE" trailer and can wrap the image
in a tar for Odin (AP slot).
"""
import struct, sys, gzip, tarfile, io, os

BASE = 0x10000000
KERNEL_OFF = 0x00008000
RAMDISK_OFF = 0x01000000
SECOND_OFF = 0x00F00000
TAGS_OFF = 0x00000100
PAGE = 2048


def pad(b, page=PAGE):
    r = len(b) % page
    return b + (b"\0" * (page - r) if r else b"")


def empty_cpio_gz():
    # newc archive containing only the TRAILER!!! entry
    name = b"TRAILER!!!\0"
    hdr = b"070701" + b"".join(b"%08X" % v for v in
          (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    blob = hdr + name
    blob += b"\0" * ((4 - len(blob) % 4) % 4)
    blob += b"\0" * ((512 - len(blob) % 512) % 512)
    return gzip.compress(blob, 9, mtime=0)


def build(kernel, ramdisk, cmdline):
    hdr = b"ANDROID!"
    hdr += struct.pack("<10I",
        len(kernel), BASE + KERNEL_OFF,
        len(ramdisk), BASE + RAMDISK_OFF,
        0, BASE + SECOND_OFF,
        BASE + TAGS_OFF, PAGE,
        1,            # header_version
        0)            # os_version
    hdr += b"A30S-M2D2".ljust(16, b"\0")
    cmd = cmdline.encode()
    assert len(cmd) <= 512 + 1024
    hdr += cmd[:512].ljust(512, b"\0")
    hdr += b"\0" * 32                          # id
    hdr += cmd[512:].ljust(1024, b"\0")        # extra_cmdline
    hdr += struct.pack("<IQI", 0, 0, 1648)     # recovery_dtbo size/offset, header_size
    assert len(hdr) == 1648, len(hdr)
    out = pad(hdr) + pad(kernel) + pad(ramdisk)
    return out + b"SEANDROIDENFORCE"


def main():
    kernel = open(sys.argv[1], "rb").read()
    outp = sys.argv[2]
    img = build(kernel, empty_cpio_gz(), "androidboot.selinux=permissive")
    open(outp, "wb").write(img)
    tarp = outp + ".tar"
    with tarfile.open(tarp, "w", format=tarfile.USTAR_FORMAT) as t:
        ti = tarfile.TarInfo(os.path.basename(outp))
        ti.size = len(img)
        t.addfile(ti, io.BytesIO(img))
    print(outp, len(img), "bytes;", tarp)


if __name__ == "__main__":
    main()

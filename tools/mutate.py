#!/usr/bin/env python3
"""
PE mutation for build uniqueness — the per-build signature rotator.
Levels:
  0  none (identical bytes for identical sources — for debugging)
  1  fresh PE TimeDateStamp only
  2  timestamp + one extra initialized section with random name/content
  3  + section names reshuffled + 2-4 noise sections + optional target size padding

Target size (--target-size-mb X.Y, 0 = off): pads the binary up to the given
size with a medium-entropy section, so output size can mimic installers etc.
No external deps.
"""
import argparse
import os
import random
import struct
import string
import sys
import time


def align(v, a):
    return (v + a - 1) & ~(a - 1)


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.buf = bytearray(f.read())
        if self.buf[:2] != b"MZ":
            sys.exit("not a PE file")
        self.e_lfanew = struct.unpack_from("<I", self.buf, 0x3C)[0]
        if self.buf[self.e_lfanew:self.e_lfanew + 4] != b"PE\0\0":
            sys.exit("bad PE signature")
        self.coff = self.e_lfanew + 4
        self.num_sections = struct.unpack_from("<H", self.buf, self.coff + 2)[0]
        self.size_opt = struct.unpack_from("<H", self.buf, self.coff + 16)[0]
        self.opt = self.coff + 20
        self.section_align = struct.unpack_from("<I", self.buf, self.opt + 0x20)[0]
        self.file_align = struct.unpack_from("<I", self.buf, self.opt + 0x24)[0]
        self.size_headers = struct.unpack_from("<I", self.buf, self.opt + 0x3C)[0]
        self.sec_headers = self.opt + self.size_opt
        self.end_headers = self.sec_headers + self.num_sections * 40

    def last_geometry(self):
        last_va = last_vsize = last_raw = last_rawsize = 0
        for i in range(self.num_sections):
            off = self.sec_headers + i * 40
            vsize = struct.unpack_from("<I", self.buf, off + 8)[0]
            va = struct.unpack_from("<I", self.buf, off + 12)[0]
            rawsize = struct.unpack_from("<I", self.buf, off + 16)[0]
            rawptr = struct.unpack_from("<I", self.buf, off + 20)[0]
            if va >= last_va:
                last_va, last_vsize, last_raw, last_rawsize = va, vsize, rawptr, rawsize
        return last_va, last_vsize, last_raw, last_rawsize

    def add_section(self, name: bytes, size: int, content: bytes):
        last_va, last_vsize, _, last_rawsize = self.last_geometry()
        new_va = align(last_va + max(last_vsize, last_rawsize), self.section_align)
        new_raw_ptr = align(len(self.buf), self.file_align)
        new_raw_size = align(size, self.file_align)
        header_off = self.end_headers
        if header_off + 40 > self.size_headers:
            sys.exit("no room for a new section header")
        raw = (content + os.urandom(max(0, new_raw_size - len(content))))[:new_raw_size]
        header = struct.pack(
            "<8sIIIIIIHHI",
            name[:7] + b"\0" * (8 - min(8, len(name))) if len(name) < 8 else name[:8],
            size, new_va, new_raw_size, new_raw_ptr, 0, 0, 0, 0, 0x40000040,
        )
        self.buf[header_off:header_off + 40] = header
        self.end_headers += 40
        self.num_sections += 1
        struct.pack_into("<H", self.buf, self.coff + 2, self.num_sections)
        size_image = align(new_va + size, self.section_align)
        struct.pack_into("<I", self.buf, self.opt + 0x38, size_image)
        while len(self.buf) < new_raw_ptr:
            self.buf.append(0)
        self.buf.extend(raw)


def rand_name(prefix=b""):
    alphabet = string.ascii_letters + string.digits
    return prefix + bytes(random.choice(alphabet.encode()) for _ in range(random.randint(5, 7)))


def medium_entropy_blob(size):
    # mix of repeated 256-byte blocks and random noise: not high-entropy, not a black hole
    out = bytearray()
    while len(out) < size:
        if random.random() < 0.5:
            out.extend(os.urandom(min(4096, size - len(out))))
        else:
            pat = os.urandom(64)
            reps = random.randint(4, 40)
            out.extend((pat * reps)[:min(len(pat) * reps, size - len(out))])
    return bytes(out[:size])


def rename_sections(pe: PE):
    keep = set()
    for i in range(pe.num_sections):
        off = pe.sec_headers + i * 40
        old = bytes(pe.buf[off:off + 8]).rstrip(b"\0")
        if old in (b".reloc",):  # keep relocation table recognizable for safety
            keep.add(i)
            continue
        new = rand_name(b"." if random.random() < 0.7 else b"")
        new = (new + b"\0" * 8)[:8]
        pe.buf[off:off + 8] = new
    return len(keep)


def mutate(path, level, target_mb, seed=None):
    if level <= 0:
        return {"level": 0}
    if seed is not None:
        random.seed(seed)
    pe = PE(path)
    info = {"level": level, "sections": 0}

    # fresh timestamp (random within the last 30 days)
    now = int(time.time())
    struct.pack_into("<I", pe.buf, pe.coff + 4, random.randint(now - 30 * 86400, now))
    info["timestamp"] = True

    if level >= 2:
        name = rand_name(b".")
        size = random.randint(2048, 65536)
        pe.add_section(name.ljust(8, b"\0"), size, medium_entropy_blob(size))
        info["sections"] += 1

    if level >= 3:
        pe2 = pe  # same buffer keeps geometry fresh after each add
        for _ in range(random.randint(2, 4)):
            n = rand_name(b".")
            s = random.randint(4096, 262144)
            pe2.add_section(n.ljust(8, b"\0"), s, medium_entropy_blob(s))
            info["sections"] += 1
        renamed = rename_sections(pe2)
        info["renamed"] = renamed

    # target size padding
    if target_mb and target_mb > 0:
        target = int(target_mb * 1024 * 1024)
        current = len(pe.buf)
        if target > current:
            pad = target - current
            # account for section header + alignment slack
            pad_needed = max(0, pad - 4096)
            n = rand_name(b".")
            pe.add_section(n.ljust(8, b"\0"), pad_needed, medium_entropy_blob(min(pad_needed, 1 << 20)))
            # extend the final section to the exact target if short
            while len(pe.buf) < target:
                pe.buf.extend(medium_entropy_blob(min(1 << 20, target - len(pe.buf))))
            while len(pe.buf) % pe.file_align:
                pe.buf.append(0)
        info["target_mb"] = target_mb

    with open(path, "wb") as f:
        f.write(pe.buf)
    info["new_size"] = len(pe.buf)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--level", type=int, default=2, choices=[0, 1, 2, 3])
    ap.add_argument("--target-size-mb", type=float, default=0.0)
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    if not os.path.isfile(args.binary):
        sys.exit(f"no such file: {args.binary}")
    info = mutate(args.binary, args.level, args.target_size_mb, args.seed)
    parts = [f"level {info['level']}"]
    if info.get("sections"):
        parts.append(f"+{info['sections']} noise sections")
    if info.get("renamed"):
        parts.append(f"{info['renamed']} sections renamed")
    if info.get("target_mb"):
        parts.append(f"padded to {info['target_mb']} MB")
    parts.append(f"final {info['new_size']} bytes")
    print("[mutate] " + ", ".join(parts))


if __name__ == "__main__":
    main()

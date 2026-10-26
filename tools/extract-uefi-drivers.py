#!/usr/bin/env python3
"""Take the UEFI drivers out of a Qualcomm xbl image, for a Mu-Silicium device port.

An xbl image is an ELF that carries a UEFI firmware volume; the volume holds FFS files, and a
driver's file holds a PE32+ image. This walks those structures the way the UEFI PI specification
lays them out and writes every driver it finds under <outdir>/<name>.inf-less form:

    <outdir>/<GUID>/<Name>/<Name>.efi

usage: extract-uefi-drivers.py XBL.img OUTDIR

Why not UEFIReader: that is a .NET tool and this host has no .NET. The layout is the same either
way -- this is the same set of structures, read with the PI spec's rules (a volume whose
signature is _FVH, FFS files with a 24-byte header, sections of 4 bytes of size + type, and the
EFI_SECTION_PE32 section holding the image).
"""
import os
import struct
import sys

def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def u24(d, o): return d[o] | (d[o+1] << 8) | (d[o+2] << 16)
def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def u64(d, o): return struct.unpack_from('<Q', d, o)[0]

def guid(d, o):
    b = d[o:o+16]
    return '%08X-%04X-%04X-%s-%s' % (u32(b,0), u16(b,4), u16(b,6),
        b[8:10].hex().upper(), b[10:16].hex().upper())

def pe_info(pe):
    """The machine, and the subsystem, of a PE32+ image (0, 0 if it is not one)."""
    if pe[:2] != b'MZ' or len(pe) < 0x40: return 0, 0
    off = u32(pe, 0x3c)
    if pe[off:off+4] != b'PE\0\0': return 0, 0
    machine = u16(pe, off+4)
    opt = off + 24
    subsystem = u16(pe, opt + 68) if u16(pe, opt) == 0x20b else 0
    return machine, subsystem

def sections(d, base, size):
    """(type, offset, size) of each section at base, within size bytes"""
    out, o = [], base
    while o + 4 <= base + size:
        s = u24(d, o)
        t = d[o+3]
        if s == 0xffffff: s = u32(d, o+4); hdr = 8
        else: hdr = 4
        if s < hdr or o + s > base + size: break
        out.append((t, o + hdr, s - hdr))
        o += (s + 3) & ~3
    return out

def files(d, vol, vsize):
    """(guid, offset, size, name-ish) of each FFS file in a volume body"""
    out, o = [], vol
    while o + 24 <= vol + vsize:
        name = d[o:o+16]
        if name == b'\xff' * 16: break                      # free space
        hdr = 24
        size = u24(d, o+20)
        attr = d[o+19]
        if attr & 0x02:                                     # EFI_FFS_FILE_HEADER2: 32-byte header
            hdr = 32
            size = u64(d, o+24)
        if size < hdr or o + size > vol + vsize: break
        out.append((guid(d, o), o + hdr, size - hdr))
        o += (size + 7) & ~7
    return out

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    volumes = [i - 40 for i in range(len(d)) if d[i:i+4] == b'_FVH']
    found = 0
    for v in volumes:
        if v < 0: continue
        vlen = u64(d, v+32)
        if not (0 < vlen <= len(d) - v): continue
        for fguid, foff, fsize in files(d, v + u64(d, v+40), vlen - u64(d, v+40)):
            for stype, soff, ssize in sections(d, foff, fsize):
                if stype != 0x10: continue                  # EFI_SECTION_PE32
                pe = d[soff:soff+ssize]
                machine, subsystem = pe_info(pe)
                if machine != 0xAA64 or subsystem != 0x0A:  # must be an AArch64 EFI application
                    continue
                path = os.path.join(outdir, fguid)
                os.makedirs(path, exist_ok=True)
                with open(os.path.join(path, 'driver.efi'), 'wb') as f:
                    f.write(pe)
                found += 1
    print(f'{found} AArch64 EFI drivers written under {outdir}')

main()

#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from prebuilt/uefi/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

Another device (common/devices/<name>.conf, read by common/build.sh) sets these in the environment:
  UEFI_TARGET   Mu-Silicium device name (default redwood)    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE                              addresses from its UEFI port's memory map
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PREBUILT = os.path.join(HERE, 'prebuilt', 'uefi')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
FD_BASE, FD_SIZE = env_int('FD_BASE', 0x9FD00000), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    # No Mu-Silicium tree next to us, so the EDK2 build is out of reach and the firmware and
    # BootShim that were built from it once are used as they are. The device tree and the ramdisk
    # still come from the caller (or from the tree), because prebuilt/ only ever held redwood's.
    def pick(name, fallback):
        return os.environ.get(name) or fallback

    dtb = pick('DTB', os.path.join(HERE, 'umi.dtb') if TARGET == 'umi' else os.path.join(PREBUILT, 'redwood.dtb'))
    if not os.path.isabs(dtb):
        dtb = os.path.join(HERE, dtb)
    ramdisk = pick('RAMDISK', os.path.join(PREBUILT, 'ramdisk'))
    mkbootimg = pick('MKBOOTIMG', os.path.join(PREBUILT, 'mkbootimg.py'))
    for label, path in (('device tree', dtb), ('ramdisk', ramdisk), ('mkbootimg', mkbootimg)):
        if not os.path.isfile(path):
            raise SystemExit(f'mkxnuimg.py: no {label} at {path}')
    # umi.dtb is the umi tree, so the firmware beside it must be umi's too, not redwood's.
    fd = os.environ.get('UEFI_FD') or os.path.join(HERE, f'prebuilt/uefi/{TARGET}/SILICIUM_UEFI.fd')
    if not os.path.isfile(fd):
        fd = os.path.join(PREBUILT, 'SILICIUM_UEFI.fd')
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': fd,
             'dtb': dtb, 'ramdisk': ramdisk, 'mkbootimg': mkbootimg}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
if len(fd) != FD_SIZE:
    raise SystemExit(
        f'mkxnuimg.py: {parts["fd"]} is {len(fd):#x} bytes but FD_SIZE is {FD_SIZE:#x}.\n'
        f'  The BootShim in {os.path.dirname(parts["shim"])} was assembled for a particular FD_SIZE and both\n'
        f'  have to agree. Either pass FD_SIZE={len(fd):#x} and rebuild the shim, or use a firmware of {FD_SIZE:#x} bytes.')

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')

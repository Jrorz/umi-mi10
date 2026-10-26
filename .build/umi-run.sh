#!/bin/bash
# 在小米 10 上跑一次完整 iOS（用户重启到 fastboot 后执行）
#   .build/umi-run.sh [内核payload]
set -e
cd "$(dirname "$0")/.."
P=${1:-out/umi-touch.bin}
step() { printf '  %-28s' "$1"; }

step "等待 fastboot"
for i in $(seq 60); do fastboot devices 2>/dev/null | grep -q . && break; sleep 1; done
p=$(timeout 10 fastboot getvar product 2>&1 | sed -n 's/^product: //p')
echo "$p"

if [ "$p" != "xnuloader" ]; then
	step "启动 UEFI"
	timeout 90 fastboot boot out/umi-uefi.img 2>&1 | grep -q OKAY && echo OK || { echo FAIL; exit 1; }
	sleep 18
	step "确认 loader"
	timeout 10 fastboot getvar product 2>&1 | grep -q xnuloader && echo OK || { echo FAIL; exit 1; }
fi

step "oem ramdisk"
timeout 30 fastboot oem ramdisk 2>&1 | grep -q OKAY && echo OK || { echo FAIL; exit 1; }
step "system.apfs (2.3GB)"
timeout 900 fastboot stage userland/system.apfs 2>&1 | grep -q OKAY && echo OK || { echo FAIL; exit 1; }
step "内核 payload"
timeout 300 fastboot stage "$P" 2>&1 | grep -q OKAY && echo OK || { echo FAIL; exit 1; }
step "oem run"
timeout 60 fastboot oem run 2>&1 | grep -q OKAY && echo OK || { echo FAIL; exit 1; }
echo "已启动，等约 100 秒进系统"

#!/bin/bash
# 完整测试循环：启动 → 等 100 秒 → 让用户测按键 → oem stop → 取日志
#   .build/umi-test.sh <payload> <等待秒数>
set -e
cd "$(dirname "$0")/.."
P=${1:-out/umi-btl.bin}
WAIT=${2:-100}
step() { printf '  %-26s' "$1"; }

step "fastboot"; for i in $(seq 60); do fastboot devices 2>/dev/null|grep -q . && break; sleep 1; done
p=$(timeout 10 fastboot getvar product 2>&1 | sed -n 's/^product: //p'); echo "$p"
if [ "$p" != "xnuloader" ]; then
	step "启动 UEFI"; timeout 90 fastboot boot out/umi-uefi.img 2>&1|grep -q OKAY && echo OK || { echo FAIL; exit 1; }
	sleep 18
	step "确认 loader"; timeout 10 fastboot getvar product 2>&1|sed -n 's/^product: //p'
fi
step "oem ramdisk"; timeout 30 fastboot oem ramdisk 2>&1|grep -q OKAY && echo OK || { echo FAIL; exit 1; }
step "system.apfs"; timeout 900 fastboot stage userland/system.apfs 2>&1|grep -q OKAY && echo OK || { echo FAIL; exit 1; }
step "内核 payload"; timeout 300 fastboot stage "$P" 2>&1|grep -q OKAY && echo OK || { echo FAIL; exit 1; }
step "oem run"; timeout 60 fastboot oem run 2>&1|grep -q OKAY && echo OK || { echo FAIL; exit 1; }
echo "  --- 等 $WAIT 秒进系统（这期间请在手机上测按键）"
sleep "$WAIT"
step "oem stop"; timeout 60 fastboot oem stop 2>&1|grep -q OKAY && echo OK || echo "（USB 被系统占用，尽量重试）"
sleep 8
step "取日志"; timeout 60 fastboot oem log 2>&1|grep -q OKAY && timeout 60 fastboot get_staged logs/last.log 2>&1|grep -q OKAY && echo OK || echo FAIL

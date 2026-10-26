#!/bin/bash
# 启动后立刻抓 live 日志（趁 USB 还没被 iOS 拿走）
set -e
cd "$(dirname "$0")/.."
P=${1:-out/umi-btnonly.bin}
step(){ printf '  %-22s' "$1"; }
step "fastboot"; for i in $(seq 60); do fastboot devices 2>/dev/null|grep -q . && break; sleep 1; done
p=$(timeout 10 fastboot getvar product 2>&1 | sed -n 's/^product: //p'); echo "$p"
if [ "$p" != "xnuloader" ]; then
	step "UEFI"; timeout 90 fastboot boot out/umi-uefi.img 2>&1|grep -q OKAY && echo OK || { echo FAIL; exit 1; }
	sleep 20
fi
step "ramdisk"; timeout 30 fastboot oem ramdisk 2>&1|grep -q OKAY && echo OK
step "system.apfs"; timeout 900 fastboot stage userland/system.apfs 2>&1|grep -q OKAY && echo OK
step "kernel"; timeout 300 fastboot stage "$P" 2>&1|grep -q OKAY && echo OK
step "run"; timeout 60 fastboot oem run 2>&1|grep -q OKAY && echo OK
# 从第 6 秒开始反复试着抓日志，趁 USB 还是 loader 的
for t in 6 10 15 20 25 30 40 50; do
	sleep 5
	if timeout 15 fastboot oem log 2>&1 | grep -q OKAY; then
		if timeout 20 fastboot get_staged "logs/live-$t.log" 2>&1 | grep -q OKAY; then
			echo "  抓到日志 @${t}s: logs/live-$t.log ($(stat -f%z logs/live-$t.log 2>/dev/null) bytes)"
			exit 0
		fi
	fi
done
echo "  没抓到 live 日志（USB 已被系统拿走）"

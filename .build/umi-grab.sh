#!/bin/bash
# 启动后立刻抢 live 日志：触摸驱动在 bsd_init 早期运行，那时 USB 还是 loader 的
set -e
cd "$(dirname "$0")/.."
P=${1:-out/umi-fts.bin}
step(){ printf '  %-20s' "$1"; }
step fastboot; for i in $(seq 60); do fastboot devices 2>/dev/null|grep -q . && break; sleep 1; done
p=$(timeout 10 fastboot getvar product 2>&1 | sed -n 's/^product: //p'); echo "$p"
if [ "$p" != "xnuloader" ]; then
	step UEFI; timeout 90 fastboot boot out/umi-uefi.img 2>&1|grep -q OKAY && echo OK || exit 1; sleep 20
fi
step ramdisk; timeout 30 fastboot oem ramdisk 2>&1|grep -q OKAY && echo OK
step image;   timeout 900 fastboot stage userland/system.apfs 2>&1|grep -q OKAY && echo OK
step kernel;  timeout 300 fastboot stage "$P" 2>&1|grep -q OKAY && echo OK
step run;     timeout 60 fastboot oem run 2>&1|grep -q OKAY && echo OK
echo "  --- 抢日志（每 1.5 秒试一次，共 20 秒）"
for i in $(seq 14); do
	sleep 1.5
	if timeout 8 fastboot oem log 2>&1 | grep -q OKAY; then
		if timeout 10 fastboot get_staged "logs/live-$i.log" 2>&1 | grep -q OKAY; then
			sz=$(stat -f%z "logs/live-$i.log" 2>/dev/null || echo 0)
			if [ "$sz" -gt 100 ]; then echo "  抓到 @$i: logs/live-$i.log ($sz bytes)"; exit 0; fi
		fi
	fi
done
echo "  没抓到"

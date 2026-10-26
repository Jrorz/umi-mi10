#!/bin/bash
# 从安卓自己的内核日志里取权威硬件事实（root）
cd "$(dirname "$0")/.."
for i in $(seq 40); do adb devices 2>/dev/null | grep -q 'device$' && break; sleep 2; done
q(){ adb shell su -c "$1" 2>/dev/null; }
echo "===== SPMI arbiter 版本与通道 ====="
q 'dmesg | grep -iE "spmi|pmic-arb|arbiter" | head -40'
echo
echo "===== PMIC 型号与 slave ====="
q 'dmesg | grep -iE "pm8150|pm8150b|pm8150l|pmk8002|pm8009|qpnp|pon" | head -30'
echo
echo "===== 触摸芯片初始化 ====="
q 'dmesg | grep -iE "goodix|gt9886|gt9889|fts|focal|touch" | head -40'
echo
echo "===== I2C/SPI 总线与供电 ====="
q 'dmesg | grep -iE "i2c.*a94000|qup|ldoc1|vddio|regulator" | head -30'

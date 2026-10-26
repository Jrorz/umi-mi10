#!/bin/bash
# 从安卓读取 logdump 分区里 loader 写的日志（loader 在运行结束时写入）
cd "$(dirname "$0")/.."
for i in $(seq 30); do adb devices 2>/dev/null | grep -q 'device$' && break; sleep 2; done
adb shell su -c 'dd if=/dev/block/by-name/logdump bs=4096 count=400 2>/dev/null' > logs/dump.raw 2>/dev/null
python3 - <<'PY'
import re
d=open('logs/dump.raw','rb').read().replace(b'\x00',b'').decode('utf-8','replace')
print('有效字节:', len(d))
print('buttons: 行数:', d.count('buttons:'))
print('goodix-i2c: 行数:', d.count('goodix-i2c'))
print('=== 关键行 ===')
for l in d.split('\n'):
    if any(k in l for k in ('buttons:', 'goodix-i2c', 'panic', 'cmdline:', 'Halted')):
        print(l[:180])
PY

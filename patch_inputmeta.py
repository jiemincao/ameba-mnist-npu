# -*- coding: utf-8 -*-
"""把 channel_mean_value.txt 的 mean/scale 填進 inputmeta.yml。

不用 yaml 套件(容器外可能沒裝),純文字改 —— inputmeta.yml 的縮排格式固定,
而且檔頭寫明「This file disallow TABs」,所以只能用空白。
"""
import io
import re
import sys

path = sys.argv[1]
mean_vals = [float(x) for x in sys.argv[2].split()]
scale_val = float(sys.argv[3])

r = io.open(path, encoding='utf-8').read()

# mean: 後面接三行 "- 0"
def repl_mean(m):
    indent = m.group(1)
    body = ''.join('%s- %g\n' % (indent + '  ', v) for v in mean_vals[:3])
    return '%smean:\n%s' % (indent, body)


r2, n_mean = re.subn(
    r'( *)mean:\n(?:\1  - [-\d.eE+]+\n)+',
    repl_mean, r)

r3, n_scale = re.subn(r'( *)scale: [-\d.eE+]+',
                      lambda m: '%sscale: %.10g' % (m.group(1), scale_val),
                      r2)

io.open(path, 'w', encoding='utf-8').write(r3)
print('patched mean blocks=%d scale=%d -> mean=%s scale=%.10g'
      % (n_mean, n_scale, mean_vals[:3], scale_val))

for line in r3.splitlines():
    if 'mean' in line or 'scale' in line or '- 1' in line:
        print('   ' + line)

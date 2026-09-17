# -*- coding: utf-8 -*-
"""把 .nb (NBG) 轉成可以 #include 的 C 陣列。

SD 卡被公司的 removable-storage 政策擋掉,所以模型只能跟程式一起編進 flash。
這跟 MnistNpuBench 的做法一樣,差別只在陣列大小。

用法:  python scripts/nb2header.py <input.nb> <output.h> [symbol]
"""
import os
import sys

src = sys.argv[1]
dst = sys.argv[2]
sym = sys.argv[3] if len(sys.argv) > 3 else "g_nbg"

data = open(src, "rb").read()
n = len(data)

# aligned(64):NBG 會被 NPU 的 DMA 讀,起始位址對齊 cache line 比較保險。
# const -> 放 .rodata -> 留在 flash,不佔 RAM。
out = [
    "// 由 scripts/nb2header.py 自動產生,不要手改\n",
    "// 來源: %s\n" % os.path.basename(src),
    "// 大小: %d bytes\n\n" % n,
    "#ifndef %s_H\n#define %s_H\n\n" % (sym.upper(), sym.upper()),
    "#define %s_SIZE %dU\n\n" % (sym.upper(), n),
    "const unsigned char __attribute__((aligned(64))) %s[%d] = {\n" % (sym, n),
]

PER = 16
for i in range(0, n, PER):
    chunk = data[i:i + PER]
    out.append("".join("0x%02x," % b for b in chunk) + "\n")

out.append("};\n\n#endif\n")

with open(dst, "w") as f:
    f.writelines(out)

print("%s -> %s" % (src, dst))
print("  %d bytes -> C 陣列 %s[%d]" % (n, sym, n))
print("  產生的 .h 檔 %.1f MB" % (os.path.getsize(dst) / 1024.0 / 1024.0))

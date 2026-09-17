# -*- coding: utf-8 -*-
"""比對 float 與 uint8 推論結果的 top-1 正確率。

iter_N 對應 dataset.txt 的第 N 行,而檔名 calib/d<label>_<idx>.png 自帶正確答案,
所以不需要另外對答案表。
"""
import os
import re
import glob

D = os.environ.get("DIR", "/workspace/acuity_examples_c901149/Models/mnist_cnn")
lines = [l.strip() for l in open(os.path.join(D, "dataset.txt")) if l.strip()]
labels = [int(re.search(r"/d(\d)_", l).group(1)) for l in lines]
print("dataset entries =", len(lines))


def load(path):
    txt = open(path).read().split()
    return [float(x) for x in txt]


for dt in ("float", "uint8"):
    files = glob.glob(os.path.join(D, "inf", dt,
                                  "iter_*_attach_softmax_*_1_10.tensor"))
    got = {}
    for f in files:
        n = int(re.search(r"iter_(\d+)_", os.path.basename(f)).group(1))
        v = load(f)
        got[n] = v.index(max(v))
    idx = sorted(got)
    ok = sum(1 for n in idx if got[n] == labels[n])
    print("%-6s : files=%3d  top1 = %d/%d = %.2f%%"
          % (dt, len(files), ok, len(idx), 100.0 * ok / len(idx)))
    wrong = [(n, labels[n], got[n]) for n in idx if got[n] != labels[n]]
    if wrong:
        print("         錯的(iter, 正解, 預測):", wrong[:12])

# float 與 uint8 的預測一致率 —— 量化掉多少直接看這個
fa, ua = {}, {}
for dt, d in (("float", fa), ("uint8", ua)):
    for f in glob.glob(os.path.join(D, "inf", dt,
                                    "iter_*_attach_softmax_*_1_10.tensor")):
        n = int(re.search(r"iter_(\d+)_", os.path.basename(f)).group(1))
        d[n] = load(f)
common = sorted(set(fa) & set(ua))
agree = sum(1 for n in common
            if fa[n].index(max(fa[n])) == ua[n].index(max(ua[n])))
maxdiff = max(max(abs(a - b) for a, b in zip(fa[n], ua[n])) for n in common)
print("float vs uint8 top1 一致 = %d/%d,機率最大絕對誤差 = %.4f"
      % (agree, len(common), maxdiff))

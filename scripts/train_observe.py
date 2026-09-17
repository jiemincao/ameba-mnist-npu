# -*- coding: utf-8 -*-
"""訓練觀察版:跟 train_mnist.py 同一個模型,但把「訓練到底做了什麼」攤出來看。

跟 train_mnist.py 的差別:
  * 不覆蓋 Models/mnist_cnn/ (那份對應著已經燒進板子的 .nb),輸出到 observe_out/
  * 訓練前先評估一次隨機權重,建立「還沒學到東西」的基準線
  * 每個 epoch 都印,並跟 ln(10)=2.3026 這個亂猜基準對照
  * 把 conv1 的 16 個 3x3 filter 存成 PNG,肉眼看它學到什麼
  * 印出各層權重分布,直接接到量化的 scale / zero_point

用法:  sh dk.sh 'python3 /workspace/train_observe.py [epochs]'
"""
import os
import sys
import json
import math
import numpy as np
import tensorflow as tf

EPOCHS = int(sys.argv[1]) if len(sys.argv) > 1 else 1
SEED = 1234
SIZE, CH, NUM_CLASSES = 28, 3, 10
OUT = "observe_out/e%d" % EPOCHS
CHANCE = math.log(NUM_CLASSES)          # 2.3026 = 完全亂猜的 loss

os.makedirs(OUT, exist_ok=True)
tf.keras.utils.set_random_seed(SEED)    # 每次跑初始權重都一樣,不同 epoch 數才可比

print("=" * 66)
print("TF %s  |  epochs = %d  |  seed = %d" % (tf.__version__, EPOCHS, SEED))
print("=" * 66)

(xtr, ytr), (xte, yte) = tf.keras.datasets.mnist.load_data()
to3 = lambda x: np.repeat(x[..., None], CH, axis=-1)
xtr3 = to3(xtr).astype("float32") / 255.0
xte3 = to3(xte).astype("float32") / 255.0

FLAT = (SIZE // 4) * (SIZE // 4) * 32
model = tf.keras.Sequential([
    tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu",
                           input_shape=(SIZE, SIZE, CH), name="conv1"),
    tf.keras.layers.MaxPooling2D(2),
    tf.keras.layers.Conv2D(32, 3, padding="same", activation="relu", name="conv2"),
    tf.keras.layers.MaxPooling2D(2),
    tf.keras.layers.Reshape((FLAT,)),
    tf.keras.layers.Dense(64, activation="relu", name="dense1"),
    tf.keras.layers.Dense(NUM_CLASSES, name="dense2"),
    tf.keras.layers.Softmax(),
])
model.compile(optimizer="adam", loss="sparse_categorical_crossentropy",
              metrics=["accuracy"])

# ---------------------------------------------------------------- 起跑線
print("\n--- 訓練前:權重還是亂數 ---")
l0, a0 = model.evaluate(xte3, yte, verbose=0)
print("  test loss = %.4f   (亂猜基準 ln(10) = %.4f)" % (l0, CHANCE))
print("  test acc  = %.4f   (亂猜基準 1/10 = 0.1000)" % a0)
LAYERS = ["conv1", "conv2", "dense1", "dense2"]
W0 = {n: model.get_layer(n).get_weights()[0].copy() for n in LAYERS}
w0 = W0["conv1"]

# ---------------------------------------------------------------- 訓練
print("\n--- 開始訓練 ---")
hist = model.fit(xtr3, ytr, epochs=EPOCHS, batch_size=128,
                 validation_split=0.1, verbose=2)

loss, acc = model.evaluate(xte3, yte, verbose=0)

# ---------------------------------------------------------------- 逐 epoch
print("\n" + "=" * 66)
print("每個 epoch 的進展")
print("=" * 66)
print("%-7s %10s %10s %10s %12s" % ("epoch", "loss", "val_loss", "val_acc", "距亂猜"))
print("%-7s %10.4f %10s %10.4f %12s" % ("0(亂數)", l0, "-", a0, "0.0%"))
h = hist.history
for i in range(EPOCHS):
    drop = (CHANCE - h["loss"][i]) / CHANCE * 100
    print("%-7d %10.4f %10.4f %10.4f %11.1f%%" %
          (i + 1, h["loss"][i], h["val_loss"][i], h["val_accuracy"][i], drop))
print("-" * 66)
print("最終 test accuracy = %.4f    test loss = %.4f" % (acc, loss))

# ---------------------------------------------------------------- conv1 filter
def save_filter_grid(w, path, scale=24, gap=4):
    """w: (3,3,CH,16) -> 4x4 拼貼 PNG。對輸入通道加總,得到等效的灰階 filter。"""
    k = w.sum(axis=2)                       # (3,3,16) 三個輸入通道是複製的灰階,加總即可
    n = k.shape[-1]
    side = int(math.sqrt(n))
    kh, kw = k.shape[0], k.shape[1]
    th, tw = kh * scale, kw * scale
    H = side * th + (side + 1) * gap
    W = side * tw + (side + 1) * gap
    canvas = np.full((H, W, 3), 128, dtype=np.uint8)
    for f in range(n):
        t = k[:, :, f]
        m = np.abs(t).max() or 1.0          # 每個 filter 各自normalize,才看得出形狀
        img = ((t / m) * 127.0 + 128.0).clip(0, 255).astype(np.uint8)
        big = np.repeat(np.repeat(img, scale, axis=0), scale, axis=1)
        r, c = f // side, f % side
        y = gap + r * (th + gap)
        x = gap + c * (tw + gap)
        canvas[y:y + th, x:x + tw, :] = big[..., None]
    open(path, "wb").write(tf.io.encode_png(canvas).numpy())

w1 = model.get_layer("conv1").get_weights()[0]
save_filter_grid(w0, os.path.join(OUT, "conv1_before.png"))
save_filter_grid(w1, os.path.join(OUT, "conv1_after.png"))
save_filter_grid(w1 - w0, os.path.join(OUT, "conv1_delta.png"))
print("  -> %s/conv1_{before,after,delta}.png" % OUT)

# ------------------------------------------------- 哪一層真的學到東西?
print(chr(10) + "" + "=" * 70)
print("每一層動了多少 (訓練 = 改這些數字,看誰被改最多)")
print("=" * 70)
print("%-8s %9s %12s %12s %11s" % ("層", "權重數", "平均|dw|", "相對初始", "餘弦相似"))
for n in LAYERS:
    a = W0[n].ravel()
    b = model.get_layer(n).get_weights()[0].ravel()
    d = np.abs(b - a).mean()
    rel = d / (np.abs(a).mean() or 1.0) * 100
    cos = float(np.dot(a, b) / ((np.linalg.norm(a) * np.linalg.norm(b)) or 1.0))
    print("%-8s %9d %12.5f %11.1f%% %11.4f" % (n, a.size, d, rel, cos))
print(chr(10) + "餘弦相似 1.0 = 這層的權重方向完全沒變,0.0 = 完全轉向。")

# ---------------------------------------------------------------- 權重分布 / 量化
def ascii_hist(v, bins=25, width=46):
    lo, hi = float(v.min()), float(v.max())
    if hi <= lo:
        return
    cnt, edge = np.histogram(v, bins=bins, range=(lo, hi))
    top = cnt.max() or 1
    for i in range(bins):
        mid = (edge[i] + edge[i + 1]) / 2
        bar = "#" * int(round(cnt[i] / top * width))
        print("    %+8.4f | %s" % (mid, bar))

print("\n" + "=" * 66)
print("各層權重分布 -> 量化參數 (對稱量化,zero_point = 0)")
print("=" * 66)
for name in ["conv1", "conv2", "dense1", "dense2"]:
    w = model.get_layer(name).get_weights()[0].ravel()
    amax = float(np.abs(w).max())
    print("\n[%s]  n=%d   範圍 %+.4f ~ %+.4f" % (name, w.size, w.min(), w.max()))
    print("  scale = max|w| / 127 = %.4f / 127 = %.8f" % (amax, amax / 127.0))
    print("  -> 每一階代表 %.2e,量化誤差最大 +-%.2e" % (amax / 127.0, amax / 254.0))
    ascii_hist(w)

json.dump({"epochs": EPOCHS, "seed": SEED, "pre_loss": float(l0), "pre_acc": float(a0),
           "test_loss": float(loss), "test_acc": float(acc),
           "history": {k: [float(x) for x in v] for k, v in h.items()}},
          open(os.path.join(OUT, "run.json"), "w"), indent=2)
model.save(os.path.join(OUT, "model.h5"), include_optimizer=False)
print("\nDONE -> %s/" % OUT)

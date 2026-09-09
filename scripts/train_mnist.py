# -*- coding: utf-8 -*-
"""在 acuity 容器內訓練 MNIST 0~9 CNN,存成 acuity 吃的 Keras h5。

刻意的設計決定:
1. 輸入 28x28x**3**。裝置端 img_resize_planar() 永遠寫 W*H*3 bytes
   (img_t 沒有 channel 欄位),所以 1 通道模型會讓 tensor buffer 溢出。
2. 不用 Flatten / GlobalPooling,用算好常數的 Reshape，不產生動態 shape。
3. 訓練時輸入是 0~1 float,對應 inputmeta 的 mean=0 / scale=1/255,
   裝置端餵的就是原始 uint8 0~255。
"""
import os
import numpy as np
import tensorflow as tf

SIZE = 28
CH = 3
NUM_CLASSES = 10
OUT = "acuity_examples_c901149/Models/mnist_cnn"
N_CALIB = 200

print("TF %s / Keras %s" % (tf.__version__, tf.keras.__version__))

(xtr, ytr), (xte, yte) = tf.keras.datasets.mnist.load_data()
print("train %s test %s" % (xtr.shape, xte.shape))


def to3ch(x):
    """灰階 -> 三通道(複製)。裝置端 R=G=B 的灰階紙面數字大致就是這樣。"""
    return np.repeat(x[..., None], CH, axis=-1)


xtr3 = to3ch(xtr).astype("float32") / 255.0
xte3 = to3ch(xte).astype("float32") / 255.0

FLAT = (SIZE // 4) * (SIZE // 4) * 32     # 7*7*32 = 1568
print("flat =", FLAT)

model = tf.keras.Sequential([
    tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu",
                           input_shape=(SIZE, SIZE, CH)),
    tf.keras.layers.MaxPooling2D(2),
    tf.keras.layers.Conv2D(32, 3, padding="same", activation="relu"),
    tf.keras.layers.MaxPooling2D(2),
    tf.keras.layers.Reshape((FLAT,)),
    tf.keras.layers.Dense(64, activation="relu"),
    tf.keras.layers.Dense(NUM_CLASSES),
    tf.keras.layers.Softmax(),
])
model.summary()

model.compile(optimizer="adam",
              loss="sparse_categorical_crossentropy",
              metrics=["accuracy"])
model.fit(xtr3, ytr, epochs=6, batch_size=128, validation_split=0.1, verbose=2)

loss, acc = model.evaluate(xte3, yte, verbose=0)
print("=== float test accuracy = %.4f ===" % acc)

if not os.path.isdir(OUT):
    os.makedirs(OUT)
h5 = os.path.join(OUT, "mnist_cnn.h5")
model.save(h5, include_optimizer=False)
print("saved", h5, os.path.getsize(h5), "bytes")

# ---- 校正用圖片:用 test set,每個類別都取一樣多張 ----
calib_dir = os.path.join(OUT, "calib")
if not os.path.isdir(calib_dir):
    os.makedirs(calib_dir)
names = []
per_class = {}
i = 0
while len(names) < N_CALIB and i < len(xte):
    lbl = int(yte[i])
    if per_class.get(lbl, 0) < N_CALIB // NUM_CLASSES:
        per_class[lbl] = per_class.get(lbl, 0) + 1
        fn = "calib/d%d_%04d.png" % (lbl, i)
        png = tf.io.encode_png(to3ch(xte[i:i + 1])[0]).numpy()
        open(os.path.join(OUT, fn), "wb").write(png)
        names.append(fn)
    i += 1
open(os.path.join(OUT, "dataset.txt"), "w").write("\n".join(names) + "\n")
print("calibration images = %d, per class = %s" % (len(names), sorted(per_class.items())))

# mean 0 / scale 1/255,對應訓練時的 x/255
open(os.path.join(OUT, "channel_mean_value.txt"), "w").write("0 0 0 0.00392157\n")
open(os.path.join(OUT, "inputs_outputs.txt"), "w").write("")

# 存一批 golden 資料,之後拿來比對 float vs uint8 的結果
np.save(os.path.join(OUT, "golden_x.npy"), xte3[:100])
np.save(os.path.join(OUT, "golden_y.npy"), yte[:100])
np.save(os.path.join(OUT, "golden_pred.npy"), model.predict(xte3[:100], verbose=0))
print("DONE")

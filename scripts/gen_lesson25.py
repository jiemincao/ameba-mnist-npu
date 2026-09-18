# -*- coding: utf-8 -*-
"""產生 colab/025_retrain_rps.ipynb —— 第 2.5 課:用自己收的資料重訓。

**不要手改 .ipynb**——改這支腳本,然後:
    python scripts/gen_lesson25.py
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "colab", "025_retrain_rps.ipynb")

CELLS = []


def md(s):
    CELLS.append(("md", s.strip("\n")))


def code(s):
    CELLS.append(("code", s.strip("\n")))


# ===================================================================== 0
md(r'''
# 第 2.5 課：用你自己收的資料重訓

第 2 課我們用網路上的資料集訓練，成績 100%，燒進板子之後幾乎全錯。
原因寫在第 2 課的第 13 節：**domain gap**，還有 **softmax 說不出「都不是」**。

這一課把兩個問題一起修掉：

| | 第 2 課 | 這一課 |
|---|---|---|
| 資料哪來 | 網路下載的 CG 算圖 | **你用 AMB82 的鏡頭拍的** |
| 背景 | 純白 | 你辦公室原本的樣子 |
| 類別 | 3 類 | **4 類**（多一個 none） |
| 預期成績 | 1.0000 | **不會是 1.0000，而且這樣才對** |

> **這一課最重要的一件事不在 Colab 裡，在你的桌子上：收資料。**
> 模型的上限是資料決定的。參數調到死，也救不回爛資料。

---

## 0. 先收資料（在你電腦上做，不是在 Colab）

開 GUI：

```
python tools/rps_gui.py COM5
```

畫面下方有四個按鈕 **ROCK / PAPER / SCISSORS / NONE**（快捷鍵 1 2 3 4）。
按一下拍一張，存進 `dataset/<類別>/`。按 **BURST**（B）可以連拍。

### 每類要收多少

**每類至少 150 張，四類加起來 600 張以上。** 少於 100 張會很不穩。

### 收的時候要刻意製造變化 —— 這是重點

如果你 150 張都是同一個姿勢、同一個位置、同一種光線，那等於只有 1 張。
每一類都要涵蓋：

- **距離**：手貼近鏡頭、正常距離、稍遠
- **位置**：畫面中央、偏左、偏右、偏上偏下
- **角度**：手轉一轉，手腕翻一翻，稍微傾斜
- **左右手**：兩隻手都要收
- **光線**：開燈、關一盞燈、拉窗簾、背光 —— 至少兩三種
- **背景**：坐著拍、站著拍、換個方向拍

### none 這一類要收什麼

`none` 是「畫面裡沒有在比手勢」。它要**比其他三類更雜**，
因為它要吃下所有「不是手勢」的情況：

- 空景（鏡頭對著桌子、牆、天花板）
- 你的臉、你的身體
- 手放下去、手在畫面邊緣、手正在移動（比到一半的模糊狀態）
- 隨便一個東西：杯子、手機、滑鼠

> **刻意去拍那些「模型可能誤判成 rock 的畫面」。**
> 我們之前空景會被判成 rock 99%，那就多拍一些空景。

### 收完之後

`dataset/` 按右鍵 → 壓縮成 `dataset.zip`，等一下傳到 Colab。
''')

# ===================================================================== 1
md(r'''
## 1. 環境檢查

跟第 2 課一樣。有沒有 GPU 都跑得完，沒有只是慢一點。
''')

code(r'''
# ---- 環境檢查 ------------------------------------------------------
import tensorflow as tf, numpy as np, random, os
print("TensorFlow :", tf.__version__)
print("GPU        :", tf.config.list_physical_devices("GPU") or "沒有(用 CPU,慢但跑得完)")

SEED = 1234
random.seed(SEED); np.random.seed(SEED); tf.random.set_seed(SEED)

IMG        = 96
NORM_SCALE = 1.0 / 255.0

# none 放最後,所以 rock/paper/scissors 還是 0/1/2 ——
# 板子端那張勝負表完全不用改。這是刻意安排的。
CLASS_NAMES = ["rock", "paper", "scissors", "none"]
NUM_CLASSES = len(CLASS_NAMES)
print("類別       :", CLASS_NAMES)
''')

# ===================================================================== 2
md(r'''
## 2. 上傳你的 dataset.zip
''')

code(r'''
# ---- 上傳並解壓 ----------------------------------------------------
from google.colab import files
import zipfile, shutil, os

shutil.rmtree("dataset", ignore_errors=True)
up = files.upload()                      # 選 dataset.zip
with zipfile.ZipFile(list(up.keys())[0]) as z:
    z.extractall(".")

# 壓縮時常常會多包一層資料夾,這裡自動找到真正裝著 rock/ 的那一層
root = None
for dirpath, dirnames, _ in os.walk("."):
    if "rock" in dirnames and "paper" in dirnames:
        root = dirpath
        break
assert root, "找不到 rock/ paper/ 這些資料夾,檢查一下 zip 的結構"
print("資料根目錄 :", root)
''')

# ===================================================================== 3
md(r'''
## 3. 先看資料，再訓練

**永遠先看資料。** 下面兩格會告訴你資料有沒有問題。
任何一項不對就回去補拍 —— 訓練半小時救不回 5 分鐘就能補好的資料。
''')

code(r'''
# ---- 清點 ----------------------------------------------------------
from PIL import Image
import glob

counts, paths = {}, {}
for c in CLASS_NAMES:
    p = sorted(glob.glob(os.path.join(root, c, "*.jpg")))
    paths[c], counts[c] = p, len(p)

print("每類張數:")
for c in CLASS_NAMES:
    print("  %-9s %4d  %s" % (c, counts[c], "#" * (counts[c] // 5)))

total = sum(counts.values())
mn, mx = min(counts.values()), max(counts.values())
print()
print("總計 : %d 張" % total)

ok = True
if mn < 100:
    print("[!] 最少的那一類只有 %d 張,建議補到 150 張以上" % mn)
    ok = False
if mn and mx > mn * 1.5:
    print("[!] 最多(%d)是最少(%d)的 %.1f 倍,類別不平衡。" % (mx, mn, mx / mn))
    print("    模型會偏心張數多的那一類。補拍少的,或靠等一下的 class_weight。")
    ok = False
if ok:
    print("資料量看起來沒問題。")
''')

code(r'''
# ---- 每類抽 8 張看一眼:確認標籤沒貼錯、變化夠不夠 --------------------
import matplotlib.pyplot as plt

fig, axes = plt.subplots(NUM_CLASSES, 8, figsize=(16, 2.1 * NUM_CLASSES))
for r, c in enumerate(CLASS_NAMES):
    pick = np.linspace(0, counts[c] - 1, 8).astype(int)   # 平均抽,不是只看開頭
    for k, i in enumerate(pick):
        axes[r][k].imshow(Image.open(paths[c][i]))
        axes[r][k].axis("off")
        if k == 0:
            axes[r][k].set_title(c, loc="left", fontsize=12)
plt.tight_layout(); plt.show()

print("看這張圖的時候問自己三件事:")
print("  1. 每一列的標籤都貼對了嗎?(拍錯類別是最常見的錯)")
print("  2. 同一列的 8 張長得像不像?太像 = 變化不夠 = 回去補拍")
print("  3. none 那一列夠雜嗎?它要吃下所有不是手勢的情況")
''')

# ===================================================================== 4
md(r'''
## 4. 切資料：這一格是這堂課的分水嶺

第 2 課我們可以隨機切 train / test。**這次不行。**

你的照片是**連拍**出來的。第 37 張和第 38 張是同一隻手、同一個位置，
差別可能只有幾個像素。如果隨機切，第 37 張進 train、第 38 張進 test，
那 test 裡就躺著 train 的雙胞胎 —— **模型背起來就有滿分**。
你又會拿到一個 1.0000 的假成績，然後燒進板子再失望一次。

### 但「按時間排一排、後面 15% 當 test」也不對

我們第一次就是這樣切的，結果 test accuracy 只有 0.6491。
去看它拿哪些照片當考題才發現：**每一類的 test 都只有「最後 4 秒」**——
同一個姿勢、同一個背景、同一道光。考題只有一個場景，
分數自然又低又不穩，而且低得沒有道理。

```
一整段 30 秒的連拍
├──────────── train 70% ────────────┼─ val ─┼─ test ─┤
                                              ↑
                                     只有最後這 4 秒當考題
```

**一整段連續的連拍，怎麼切都不誠實：**

| 切法 | 問題 |
|---|---|
| 隨機切 | 相鄰兩張幾乎一樣 → 雙胞胎作弊，分數**虛高** |
| 全部排一排、切尾巴 | test 只剩最後幾秒一個畫面 → 分數**虛低且不穩** |

### 正解：先找出「幾段連拍」，每一段各自切

按 BURST 拍一段、停下來換個東西、再拍一段 —— 這些**段**才是資料真正的結構。
檔名裡的時間戳會告訴我們段落在哪：**間隔超過 1.5 秒就是換了一段**。

然後**每一段各自**切 70/15/15，再把各段的同名部分合起來：

```
第 1 段 (正面)   ├── train ──┼ val ┼ test ┤
第 2 段 (轉 45°) ├── train ──┼ val ┼ test ┤
第 3 段 (換背景) ├── train ──┼ val ┼ test ┤
                       ↓        ↓      ↓
                     train     val    test   ← 每一段都有代表
```

這樣 test 涵蓋**所有場景**，不是只有最後一個。

### 還要再加一道保險：段內的接縫

段內切開的地方，train 的最後一張和 val 的第一張還是相鄰的兩幀 —— 又是雙胞胎。
所以每個接縫**丟掉 3 張**當緩衝區。丟掉一點資料，換一個可信的分數，很划算。

> 這個原則叫**不要讓資訊從 train 漏到 test**（data leakage）。
> 只要資料有時間或群組結構，隨機切就是錯的 ——
> 而且「有結構」不只是時間先後，還包括**分段**。
''')

code(r'''
# ---- 找出連拍段落 ----------------------------------------------------
# 檔名 rock_20260917_143022_881.jpg -> 20260917_143022 + 881 毫秒
import datetime

GAP_SEC = 1.5     # 間隔超過這麼久,視為換了一段
GUARD   = 3       # 每個接縫丟掉幾張,避免相鄰幀跨到不同 split

def shot_time(path):
    b = os.path.basename(path).rsplit("_", 3)
    return (datetime.datetime.strptime("_".join(b[-3:-1]), "%Y%m%d_%H%M%S")
            + datetime.timedelta(milliseconds=int(b[-1][:-4])))

def find_bursts(files):
    t = [shot_time(f) for f in files]
    out, start = [], 0
    for i in range(1, len(files)):
        if (t[i] - t[i - 1]).total_seconds() > GAP_SEC:
            out.append(files[start:i]); start = i
    out.append(files[start:])
    return [b for b in out if len(b) >= 8]        # 太短的段不夠切,丟掉

print("連拍段落:")
bursts = {}
for c in CLASS_NAMES:
    bursts[c] = find_bursts(paths[c])
    print("  %-9s %3d 張 -> %d 段  %s"
          % (c, len(paths[c]), len(bursts[c]), [len(b) for b in bursts[c]]))
    if len(bursts[c]) < 2:
        print("     ⚠ 只有 1 段!test 會只涵蓋一個場景,分數不可信。建議分段補拍。")
''')

code(r'''
# ---- 每一段各自切 70 / 15 / 15,接縫留緩衝 ----------------------------
def load_img(p):
    im = Image.open(p).convert("RGB").resize((IMG, IMG), Image.BILINEAR)
    return np.asarray(im, dtype=np.uint8)

xs = {"train": [], "val": [], "test": []}
ys = {"train": [], "val": [], "test": []}
dropped = 0

for ci, c in enumerate(CLASS_NAMES):
    for b in bursts[c]:
        n = len(b)
        i1, i2 = int(n * 0.70), int(n * 0.85)
        parts = (("train", b[:max(1, i1 - GUARD)]),
                 ("val",   b[i1:max(i1 + 1, i2 - GUARD)]),
                 ("test",  b[i2:]))
        dropped += n - sum(len(s) for _, s in parts)
        for split, sub in parts:
            for f in sub:
                xs[split].append(load_img(f)); ys[split].append(ci)

for k in xs:
    xs[k] = np.stack(xs[k]); ys[k] = np.array(ys[k], dtype=np.int32)

x_train, y_train = xs["train"], ys["train"]
x_val,   y_val   = xs["val"],   ys["val"]
x_test,  y_test  = xs["test"],  ys["test"]

print("train : %5d 張   %s" % (len(x_train), np.bincount(y_train, minlength=NUM_CLASSES)))
print("val   : %5d 張   %s" % (len(x_val),   np.bincount(y_val,   minlength=NUM_CLASSES)))
print("test  : %5d 張   %s" % (len(x_test),  np.bincount(y_test,  minlength=NUM_CLASSES)))
print("接縫緩衝丟掉 : %d 張" % dropped)
print()
print("形狀  :", x_train.shape, x_train.dtype)
''')

# ===================================================================== 5
md(r'''
## 5. Augmentation：這次要更兇

第 2 課的資料是白底、光線一致，augmentation 只要翻轉和小幅旋轉。

你的資料是真實相機拍的，而且**上線時的光線一定跟今天不一樣**
（換個時間、換盞燈、窗簾拉不拉）。所以這次要加**亮度和對比**的擾動，
逼模型不要靠「整體亮度」這種靠不住的線索來分類。

老規矩：**augmentation 放在資料管線，不能放進模型**。
放進模型轉檔會失敗，因為 NPU 不認得那些層。
''')

code(r'''
# ---- augmentation 與資料管線 ---------------------------------------
AUTOTUNE = tf.data.AUTOTUNE
BATCH = 32

# 幾何變形用 keras 的層來做,tf.image 沒有旋轉。
# 注意:這兩層只掛在資料管線上,不會被 model.save() 存進去,
# 所以匯出的 .tflite 裡面沒有它們 —— 板子上不會多跑這段。
geom = tf.keras.Sequential([
    tf.keras.layers.RandomRotation(0.08, fill_mode="nearest"),   # +-約 29 度
    tf.keras.layers.RandomZoom(0.20, 0.20, fill_mode="nearest"), # 手的遠近
])

def augment(x, y):
    x = tf.image.random_flip_left_right(x)
    x = tf.image.random_brightness(x, 25.0)          # 光線變化
    x = tf.image.random_contrast(x, 0.75, 1.30)      # 對比變化
    x = tf.image.random_saturation(x, 0.80, 1.20)    # 白平衡漂移
    # 小幅平移:手不會永遠在正中央
    x = tf.image.resize_with_crop_or_pad(x, IMG + 12, IMG + 12)
    x = tf.image.random_crop(x, [tf.shape(x)[0], IMG, IMG, 3])
    # 旋轉 + 縮放:同一個手勢歪一點、遠一點,還是同一個手勢
    x = geom(x, training=True)
    return tf.clip_by_value(x, 0.0, 255.0), y

def norm(x, y):
    return tf.cast(x, tf.float32) * NORM_SCALE, y

ds_tr = (tf.data.Dataset.from_tensor_slices((x_train, y_train))
         .shuffle(len(x_train), seed=SEED).batch(BATCH)
         .map(lambda a, b: augment(tf.cast(a, tf.float32), b), AUTOTUNE)
         .map(norm, AUTOTUNE).prefetch(AUTOTUNE))
ds_va = (tf.data.Dataset.from_tensor_slices((x_val, y_val))
         .batch(BATCH).map(norm, AUTOTUNE).prefetch(AUTOTUNE))
ds_te = (tf.data.Dataset.from_tensor_slices((x_test, y_test))
         .batch(BATCH).map(norm, AUTOTUNE).prefetch(AUTOTUNE))

print("train batch 數 :", len(ds_tr))
print("注意:augmentation 只掛在 ds_tr。val / test 不能動,不然評分就不公平了。")
''')

code(r'''
# ---- 看一下 augmentation 做了什麼:同一張圖跑 8 次 --------------------
one = x_train[0:1].astype("float32")
fig, ax = plt.subplots(1, 8, figsize=(16, 2.2))
for i in range(8):
    a, _ = augment(one, np.array([0]))
    ax[i].imshow(a[0].numpy().astype("uint8")); ax[i].axis("off")
ax[0].set_title("同一張圖,八種變形", loc="left")
plt.tight_layout(); plt.show()
print("每一輪訓練模型看到的都是不一樣的版本 —— 這就是它為什麼比較不會死背。")
''')

# ===================================================================== 6
md(r'''
## 6. 模型：架構跟第 2 課一模一樣，只有最後一層變 4

刻意不改架構。這樣**成績的差別就只能來自資料**。
一次只動一個變因，是做實驗的基本功。
''')

code(r'''
# ---- 建模型 --------------------------------------------------------
from tensorflow.keras import layers, models

model = models.Sequential([
    layers.Input((IMG, IMG, 3)),
    layers.Conv2D(16, 3, padding="same", activation="relu"), layers.MaxPooling2D(),
    layers.Conv2D(32, 3, padding="same", activation="relu"), layers.MaxPooling2D(),
    layers.Conv2D(64, 3, padding="same", activation="relu"), layers.MaxPooling2D(),
    layers.Conv2D(64, 3, padding="same", activation="relu"), layers.MaxPooling2D(),
    layers.Flatten(),
    layers.Dropout(0.3),
    layers.Dense(64, activation="relu"),
    layers.Dense(NUM_CLASSES, activation="softmax"),
])
model.compile(optimizer="adam", loss="sparse_categorical_crossentropy",
              metrics=["accuracy"])
model.summary()
print()
print("參數量 :", model.count_params())
print("比第 2 課多的參數只有 65 個(最後一層多一個輸出:64 個權重 + 1 個 bias)。")
''')

# ===================================================================== 7
md(r'''
## 7. 起跑線

4 類的亂猜是 **25%**（第 2 課是 33%），loss 的起跑線是 `ln(4) = 1.386`。

訓練到一半如果 accuracy 卡在 0.25 附近，那不是「學得慢」，是**根本沒在學**。
''')

code(r'''
# ---- 訓練前的成績 --------------------------------------------------
import math
loss0, acc0 = model.evaluate(ds_va, verbose=0)
print("亂猜的 accuracy : %.4f  (1/%d)" % (1.0 / NUM_CLASSES, NUM_CLASSES))
print("亂猜的 loss     : %.4f  (ln %d)" % (math.log(NUM_CLASSES), NUM_CLASSES))
print()
print("還沒訓練的模型  : accuracy %.4f   loss %.4f" % (acc0, loss0))
print("這兩組數字應該很接近。接下來的進步才是模型真的學到的東西。")
''')

# ===================================================================== 8
md(r'''
## 8. 訓練

用 `class_weight` 補償類別不平衡：張數少的那一類，每張的權重高一點。
這樣就算你某一類少拍了一些，模型也不會乾脆放棄它。
''')

code(r'''
# ---- 訓練 ----------------------------------------------------------
EPOCHS = 45

cnt = np.bincount(y_train, minlength=NUM_CLASSES).astype(np.float64)
class_weight = {i: float(cnt.sum() / (NUM_CLASSES * cnt[i])) for i in range(NUM_CLASSES)}
print("class_weight :", {CLASS_NAMES[i]: round(v, 3) for i, v in class_weight.items()})
print()

hist = model.fit(ds_tr, validation_data=ds_va, epochs=EPOCHS,
                 class_weight=class_weight, verbose=2)
h = hist.history

test_loss, test_acc = model.evaluate(ds_te, verbose=0)
print()
print("val  accuracy : %.4f" % h["val_accuracy"][-1])
print("test accuracy : %.4f" % test_acc)
''')

md(r'''
### 看到成績之後怎麼解讀

| test accuracy | 意思 |
|---|---|
| 0.25 附近 | 沒在學。檢查標籤有沒有貼錯、資料有沒有讀對 |
| 0.70 ~ 0.92 | **正常，而且健康。** 真實資料就該長這樣 |
| 0.95 以上 | 可疑。多半是切資料時漏了（回去看第 4 節），或變化收得不夠 |
| 1.0000 | **紅燈。** 跟第 2 課同一個病，先別高興 |

> 第 2 課是 1.0000，燒進板子全錯。
> 這一課如果是 0.85，燒進板子大概就有 0.8 左右。
> **0.85 的真成績遠比 1.00 的假成績有用。**
''')

code(r'''
# ---- 學習曲線 ------------------------------------------------------
fig, ax = plt.subplots(1, 2, figsize=(13, 4))
ax[0].plot(h["accuracy"], label="train"); ax[0].plot(h["val_accuracy"], label="val")
ax[0].axhline(1.0 / NUM_CLASSES, ls="--", c="gray", label="亂猜")
ax[0].set_title("accuracy"); ax[0].set_xlabel("epoch"); ax[0].legend(); ax[0].grid(alpha=.3)
ax[1].plot(h["loss"], label="train"); ax[1].plot(h["val_loss"], label="val")
ax[1].set_title("loss"); ax[1].set_xlabel("epoch"); ax[1].legend(); ax[1].grid(alpha=.3)
plt.tight_layout(); plt.show()

print("train 和 val 的差距就是「死背的程度」。")
print("如果 val 曲線在某一輪之後就不動了,那之後的 epoch 全是白跑的。")
''')

# ===================================================================== 9
md(r'''
## 9. 混淆矩陣：它到底把什麼看成什麼

總 accuracy 是一個數字，藏了很多東西。混淆矩陣才看得到**它錯在哪裡**。

特別看 **none 那一列和那一行**：

- `none` 被判成 `rock` → 空景還是會誤判，補拍空景
- `rock` 被判成 `none` → 太保守，真的手勢會被忽略
- `paper` 和 `scissors` 互相混 → 正常，這兩個最像
''')

code(r'''
# ---- 混淆矩陣 ------------------------------------------------------
prob = model.predict(ds_te, verbose=0)
pred = np.argmax(prob, axis=1)
conf = prob.max(axis=1)

cm = np.zeros((NUM_CLASSES, NUM_CLASSES), dtype=int)
for t, p in zip(y_test, pred):
    cm[t][p] += 1

print("            " + "".join("%10s" % c for c in CLASS_NAMES) + "      recall")
for i, c in enumerate(CLASS_NAMES):
    rec = cm[i][i] / cm[i].sum() if cm[i].sum() else 0.0
    print("真值 %-7s" % c + "".join("%10d" % v for v in cm[i]) + "      %.3f" % rec)

print()
rec = [cm[i][i] / cm[i].sum() if cm[i].sum() else 0 for i in range(NUM_CLASSES)]
worst = int(np.argmin(rec))
print("最弱的一類是 %s (recall %.3f)。" % (CLASS_NAMES[worst], rec[worst]))
print("補救照這個順序試:1) 補拍這一類 2) 補拍它被搞混的那一類 3) 才輪到調參數")
''')

code(r'''
# ---- 挑出它「錯得最有自信」的幾張來看 --------------------------------
bad = np.where(pred != y_test)[0]
bad = bad[np.argsort(-conf[bad])][:8]

if len(bad) == 0:
    print("test set 全對。先別開心 —— 回去看第 4 節,確認資料沒漏。")
else:
    fig, ax = plt.subplots(1, len(bad), figsize=(2.1 * len(bad), 2.6))
    if len(bad) == 1:
        ax = [ax]
    for k, i in enumerate(bad):
        ax[k].imshow(x_test[i]); ax[k].axis("off")
        ax[k].set_title("真 %s\n猜 %s %.0f%%" % (CLASS_NAMES[y_test[i]],
                        CLASS_NAMES[pred[i]], conf[i] * 100), fontsize=9)
    plt.tight_layout(); plt.show()
    print("這幾張是模型錯得最有自信的。看看它們有什麼共通點 ——")
    print("那個共通點就是你下一批資料要補的東西。")
''')

# ==================================================================== 10
md(r'''
## 10. 信心門檻要設多少

板子端會加一條規則：**最高分低於門檻就回報 unknown，不判勝負。**

門檻不能拍腦袋決定，要從資料量出來。這一格幫你算。

兩件事要權衡：門檻拉高 → 判得比較準，但很多畫面它會拒絕回答；
門檻拉低 → 什麼都敢判，但錯得多。
''')

code(r'''
# ---- 從 test set 量出合理的門檻 -------------------------------------
print(" 門檻     願意判的比例    判了之後的正確率")
print("-" * 48)
best = None
for th in [0.0, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95]:
    keep = conf >= th
    if keep.sum() == 0:
        continue
    cover = keep.mean()
    acc = (pred[keep] == y_test[keep]).mean()
    print("  %.2f       %6.1f%%          %6.1f%%" % (th, cover * 100, acc * 100))
    if cover >= 0.80 and (best is None or acc > best[1]):
        best = (th, acc)

print()
if best:
    print("建議門檻 : %.2f" % best[0])
    print("(在「至少還願意判 80%% 的畫面」的前提下,正確率最高的那一個)")
print("把這個數字填進 RpsGame.ino 的 CONF_THRESHOLD(記得那邊是 0~100 的整數)。")
''')

# ==================================================================== 11
md(r'''
## 11. 匯出，準備轉檔

跟第 2 課一樣的東西，但這次**多存一個 `labels.txt`** ——
類別變 4 個了，板子端得對得起來。
''')

code(r'''
# ---- 產生轉檔需要的全部檔案 ------------------------------------------
import shutil

OUT = "rps4_export"
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(os.path.join(OUT, "calib"))

# 1) tflite —— 第 3 課轉檔吃的是這個,不是 .h5
#    刻意不量化,維持 float32。量化留給 acuity 做。
tfl_path = os.path.join(OUT, "rps4_cnn.tflite")
with open(tfl_path, "wb") as f:
    f.write(tf.lite.TFLiteConverter.from_keras_model(model).convert())
print("tflite    : %.1f KB" % (os.path.getsize(tfl_path) / 1024.0))

# 2) h5 備份(想回頭改架構、接著訓練時要用)
model.save(os.path.join(OUT, "rps4_cnn.h5"), include_optimizer=False)

# 3) 校正圖:每類 10 張,從 test set 挑 —— 要代表上線後會看到的東西
PER_CLASS = 10
lines = []
for c in range(NUM_CLASSES):
    idx = np.where(y_test == c)[0][:PER_CLASS]
    for j, k in enumerate(idx):
        fn = "calib/%s_%02d.png" % (CLASS_NAMES[c], j)
        Image.fromarray(x_test[k]).save(os.path.join(OUT, fn))
        lines.append("./" + fn)
print("校正圖    : %d 張" % len(lines))

with open(os.path.join(OUT, "dataset.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")

# 4) 正規化:把訓練時的 /255 編進模型第一層
with open(os.path.join(OUT, "channel_mean_value.txt"), "w") as f:
    f.write("0 0 0 %.8f\n" % NORM_SCALE)

# 5) 類別順序 —— 板子端必須照這個順序解讀輸出
with open(os.path.join(OUT, "labels.txt"), "w") as f:
    f.write("\n".join(CLASS_NAMES) + "\n")
print("類別順序  :", CLASS_NAMES)

# 6) 成績單,之後跟第 2 課對照
with open(os.path.join(OUT, "train_report.txt"), "w") as f:
    f.write("dataset      : 自己拍的,共 %d 張\n" % total)
    for c in CLASS_NAMES:
        f.write("  %-9s  %d\n" % (c, counts[c]))
    f.write("split        : 按拍攝順序 70/15/15\n")
    f.write("input        : %dx%dx3\n" % (IMG, IMG))
    f.write("classes      : %s\n" % ", ".join(CLASS_NAMES))
    f.write("params       : %d\n" % model.count_params())
    f.write("epochs       : %d\n" % EPOCHS)
    f.write("seed         : %d\n" % SEED)
    f.write("val accuracy : %.4f\n" % h["val_accuracy"][-1])
    f.write("test accuracy: %.4f\n" % test_acc)

shutil.make_archive("rps4_export", "zip", OUT)
print()
print("打包完成  : rps4_export.zip  (%.1f KB)"
      % (os.path.getsize("rps4_export.zip") / 1024.0))
''')

code(r'''
# ---- 下載到你的電腦 --------------------------------------------------
from google.colab import files
files.download("rps4_export.zip")
''')

# ==================================================================== 12
md(r'''
## 12. 這一課你做了什麼

1. **自己收了資料** —— 用真的要部署的那台相機，在真的要部署的環境
2. 加了 `none` 類，讓模型第一次有能力說「畫面裡沒有手勢」
3. 學會**不能隨機切連拍的資料**，否則又會拿到假滿分
4. 加了亮度／對比的 augmentation，因為真實光線會變
5. 用混淆矩陣找出最弱的一類，而且知道**先補資料、再調參數**
6. **從資料量出信心門檻**，不是憑感覺設一個 0.8

## 下一步

1. `rps4_export.zip` → 第 3 課轉檔成 `.nb`
2. 板子端把 `NUM_CLASSES` 改成 4、加上 `CONF_THRESHOLD`
3. 燒錄，然後**跟舊模型比**：站在同一個位置比十次剪刀，各對幾次

## 自己動手

1. **拿掉 none 類重訓一次**（`CLASS_NAMES` 砍成三個），
   然後把空景丟給它 —— 親眼確認第 2 課第 13.3 節講的是真的
2. **只用一種光線的資料訓練**，拿另一種光線的照片測 ——
   這是你自己造一個 domain gap 出來看
3. **每類只用 30 張訓練**，看成績掉多少 ——
   這會告訴你「再多收 100 張值不值得」

> 第 2 課教你怎麼訓練，這一課教你**訓練之前和之後該做什麼**。
> 中間那行 `model.fit()` 反而是最不需要煩惱的。
''')


def build():
    cells = []
    for kind, src in CELLS:
        lines = (src + "\n").splitlines(True)
        if kind == "md":
            cells.append({"cell_type": "markdown", "metadata": {}, "source": lines})
        else:
            cells.append({"cell_type": "code", "metadata": {},
                          "execution_count": None, "outputs": [], "source": lines})
    return {
        "cells": cells,
        "metadata": {
            "colab": {"provenance": [], "toc_visible": True},
            "kernelspec": {"name": "python3", "display_name": "Python 3"},
            "language_info": {"name": "python"},
            "accelerator": "GPU",
        },
        "nbformat": 4,
        "nbformat_minor": 0,
    }


if __name__ == "__main__":
    nb = build()
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(nb, f, ensure_ascii=False, indent=1)
        f.write(chr(10))
    print("wrote %s  (%d cells)" % (os.path.normpath(OUT), len(nb["cells"])))

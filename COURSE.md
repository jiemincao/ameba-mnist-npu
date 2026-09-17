# AMB82 邊緣 AI：從訓練到燒錄

> 這份文件是我們實作過程的整理，不是教科書。
> 每一個數字都是在這張桌子上量出來的，每一個坑都真的踩過。

**怎麼用這份文件**

| 章節 | 內容 | 適合 |
|---|---|---|
| Part 0 | 全景：一張畫面的一生 | 投影片第一張 |
| **Part 0.5** | **給你一顆 NPU，你怎麼把東西弄出來** | ★ 工程師最該看的一章 |
| Part 1 ~ 6 | 觀念：每一站在幹嘛（板子 / 相機 / 模型 / TF / NPU / 轉檔） | 做成投影片 |
| Part 7 ~ 9 | 實作：怎麼訓練、轉檔、燒錄 | 工作坊照著做 |
| Part 10 ~ 11 | 踩過的坑、現在的進度與待辦 | 自己看 |
| Part 12 + 附錄 | 環境怎麼建、檔案在哪、名詞對照 | 換機器 / 新人接手 |

**這份是這個 repo 唯一的正式文件。**
`README.md` 只是入口；`docs/research-log.md` 是研究歷程的原始紀錄
（依時間堆疊，含**後來被推翻的結論**，只當歷史看，不要照著做）。

---

## Part 0. 全景：一張畫面的一生

先看整條路。後面每一章都是在放大其中一站。

```
【PC / Colab】                           【AMB82 板子】

  訓練資料                                  鏡頭 sensor
     ↓                                         ↓
  TensorFlow ────── 訓練 ──────┐            ISP（曝光、白平衡）
     ↓                        │               ↓
  .h5 / .tflite               │             VOE（影像協同處理器）
     ↓                        │               ↓
  acuity 轉檔                  │          channel 0 / channel 3
  （量化 + 編譯）               │               ↓
     ↓                        │            StreamIO
  .nb（NPU 算圖）───── 燒錄 ────┴──────────→  NPU ← 讀 .nb
                                              ↓
                                          每類一個機率
                                              ↓
                                           KM4（你的 sketch）
                                              ↓
                                          UART → PC 的 GUI
```

**兩條線，一個交會點。**

- **左邊（離線、一次性）**：TensorFlow 訓練 → 轉檔 → 產生 `.nb`
- **右邊（上線、每一幀）**：鏡頭 → VOE → NPU → 答案
- **交會點就是 `.nb`**。它是訓練世界交給推論世界的唯一東西。

搞清楚這張圖，你的三個問題就有位置了：
相機在右上、TensorFlow 在左上、NPU 在右下。

---

## Part 0.5. 給你一顆 NPU，你怎麼把東西弄出來

> 這一章是工程師視角。前面那張圖是「結果」，這一章是「你怎麼走到那張圖」。
> 我們真的是照這個順序做的，不是事後整理的。

桌上放著一塊板子，datasheet 說「內建 NPU，0.4 TOPS」。
**除此之外你什麼都不知道。** 接下來要做的事，是一條有嚴格相依順序的問題鏈。

```
Q1 它吃什麼?          ← 不知道這個,訓練什麼都是白訓練
Q2 誰餵它?
Q3 誰做前處理?
Q4 整條路通不通?      ← ★ 用「已知答案」的模型先走一遍
Q5 我的模型塞得進去嗎?
Q6 轉檔有沒有壞?
Q7 板子上跑出來對不對?
Q8 到底快多少?
Q9 上線後為什麼變爛?
```

**倒過來做一定會痛。** 大部分人的直覺是「先訓練一個很棒的模型」——
那是 Q5，前面四題沒答就做 Q5，失敗時你完全不知道是模型錯、轉檔錯、
前處理錯、還是相機根本沒接上。

---

### Q1 這顆 NPU 到底吃什麼？

**方法：不要讀文件，去逆向它自帶的模型。**

SDK 裡附了九個現成模型（`.nb`）。它們能跑，所以它們**就是規格書**。

```bash
./nbinfo -in  img_class_cnn.nb      # 它要什麼進來
./nbinfo -out img_class_cnn.nb      # 它吐什麼出來
```

一跑就拿到四個關鍵事實：

| 問題 | 答案 | 影響 |
|---|---|---|
| 檔案格式 | NBG，不是 `.tflite` 也不是 `.onnx` | 一定要轉檔 |
| 輸入型別 | UINT8 | 要量化 |
| 輸入排列 | **planar**（RRR…GGG…BBB），不是 interleaved | 前處理要對 |
| 輸出型別 | FP16 | 板子端要用 `__fp16` 讀 |

> **這一步是整條路的地基。** 我們就是靠 `nbinfo` 才知道要 planar——
> 這件事文件裡沒寫清楚，寫錯的話模型轉得出來、跑得動、但答案是垃圾。

**通用做法**：任何一個封閉的加速器，先找它「已知能動」的產物，用工具拆開看介面。
能跑的二進位檔比文件誠實。

---

### Q2 誰把資料餵給它？

NPU 不會自己去拿畫面。要找出資料路徑上有幾段、每段誰在管。

我們的做法是**讀範例程式，把每一行對應到硬體上的一個單元**：

```cpp
Camera.configVideoChannel(...)   → VOE 在管
StreamIO.registerInput(...)      → 把某一路接到某個消費者
StreamIO.registerOutput(imgclass)→ 消費者是 NN
```

結論是 Part 2 那張圖：**KM4 從頭到尾沒碰到畫素**。
知道這件事以後，很多怪現象才有解釋
（為什麼 `getImage` 回的是位址、為什麼 RESET 不會重置相機）。

---

### Q3 誰負責前處理？

這題最容易被跳過，也最容易讓你在後面浪費兩天。

訓練時我們做了 `x / 255`。上線時**這件事一定也要有人做**，問題是誰做：

| 可能的執行者 | 怎麼確認 |
|---|---|
| SDK 的 C 程式 | **去讀原始碼** |
| 編進 `.nb` 的第一層 | `nbinfo -in` 看 scale |
| 你自己的 sketch | 你自己寫的，你知道 |

我們去讀了 `classification_preprocess()`：

```c
img_resize_planar(&img_in, roi, &img_out);   // 只有 resize
```

**沒有正規化。** 所以答案是「編進 `.nb`」，這就決定了 `inputmeta.yml` 要開
`add_preproc_node: true`。

> **原則：前處理是訓練跟推論之間的一份「隱形合約」。**
> 合約一定存在，你只是要找出它由誰履行。沒找出來 = 兩邊對不起來 = 數值全錯。

---

### Q4 ★ 整條路通不通？——用一個「你已經知道答案」的模型

**這是整套方法論裡最重要的一步。**

在自己訓練任何東西之前，先讓一個**簡單、已知、你能驗算**的模型走完全程：

```
MNIST → 訓練(6 epoch,1分鐘) → 轉檔 → 燒錄 → 板子上辨識 → 對答案
```

為什麼是 MNIST：

- 你**知道**正確答案應該是 99.5%，跑出 30% 就是管線壞了，不用懷疑模型
- 訓練只要一分鐘，出錯可以重跑二十次
- 夠小，`.nb` 只有 128 KB，燒錄很快

這一步跑通之後，你手上就有一條**可信的基準線**。
之後換自己的模型如果壞了，**問題一定在你換掉的那部分**，因為其他部分剛剛才被證明是好的。

> **這是硬體 bring-up 的通則**：先讓最笨的東西動起來，再加複雜度。
> 跳過這步直接上自己的模型，失敗時有八個嫌疑犯；做了這步，只剩一個。

---

### Q5 我自己的模型塞得進去嗎？

這時候才輪到設計模型。要先確認三件事，**再開始訓練**：

| 檢查 | 怎麼確認 | 不過的話 |
|---|---|---|
| 算子支援嗎 | 轉檔工具的支援清單 | 換層（AMB82 不支援 HardSwish） |
| 輸入尺寸合法嗎 | `nbinfo -in` 看內建模型 | 改網路輸入層 |
| 放得下嗎 | 參數量 × 1 byte vs flash 分區 | 減通道 |

還有一條**架構層級的紀律**：

```python
# ✗ 這樣寫,轉檔會失敗
model = Sequential([ RandomFlip(), RandomBrightness(), Conv2D(...) ])

# ✓ augmentation 放在資料管線,不進模型
ds = ds.map(augment)
model = Sequential([ Conv2D(...) ])
```

**模型裡只能有 NPU 認得的層。** 訓練才需要的東西一律留在資料端。

---

### Q6 轉檔有沒有壞？

轉檔工具**不會因為結果是垃圾而報錯**。它只會說 `Error(0)`。

所以要自己建立驗收條件——**在燒錄之前**：

```
① 介面對嗎     nbinfo -in / -out,跟 Q1 拿到的規格比對
② 數值對嗎     同一批圖,float 模型 vs 量化模型,兩邊準確率跟預測都要接近
```

我們寫了 `verify_mnist.sh` + `score_mnist.py` 就是為了第 ②
（順便發現官方量化腳本寫死只跑 1 個 iteration，校正圖只用到第一張）。

> **原則：每一段管線都要有「不看下游就能判斷自己對不對」的檢查點。**
> 只有端到端一個檢查點的系統，出錯時沒有辦法二分搜尋。

---

### Q7 板子上跑出來對不對？

轉檔驗過了，燒進去還是可能錯——因為板子端有自己的前處理、色彩、排列。

**做法：讓板子把它真正吃到的東西吐出來。**

我們在韌體加了一個 `MODEL IN` 診斷，按一個鍵就把 NPU 輸入張量原封不動傳回 PC 重建成圖片。
如果那張圖是歪的、灰的、顏色錯的，問題就在板子端，跟模型無關。

> **通則：不要用「最終答案錯了」去 debug 一條多段管線。**
> 要有辦法在每一段之間把資料**攔截下來看**。

---

### Q8 到底快多少？

有東西會動之後才值得量，而且**要有對照組**。
我們拿同一個模型在 Pico 2 的 CPU 上跑（float32 / int8 / int8+CMSIS-NN 三種），
才有辦法說「快 99.2 倍」是什麼意思。

量的時候要小心三件事：

1. **時脈域**——NPU 的 cycle counter 數的是 NPU 自己的時鐘，不是 CPU 的
2. **暖機**——第一次推論含載入權重，不能算
3. **端到端 vs 純計算**——差 20 倍以上（我們是 13.4× vs 330×）

結果通常會推翻直覺：我們量出來 **96% 的時間在等作業系統的 1 ms tick**，
NPU 本身只用到 **5.5% 的算力**。

> **原則：先量，再優化。** 沒量之前你會優化錯地方。

---

### Q9 為什麼實驗室 100%、現場全錯？

最後一關，而且是**跟 NPU 完全無關**的一關。

我們的第一版模型 `test accuracy = 1.0000`，燒進板子幾乎全錯。原因：

1. **Domain gap**——訓練用 CG 算圖的手 + 純白背景，現場是辦公室
2. **Data leakage**——連拍的照片隨機切 train/test，雙胞胎躺在兩邊
3. **Softmax 說不出「不知道」**——三類的機率恆等於 1

> **`accuracy 1.0000` 不是成績，是紅燈。**
> 真實問題不會滿分，滿分代表你的測試集在說謊。

這一關的解法全部在**資料**，不在模型也不在硬體：
拿要部署的那台相機收資料、按時間切、加一類 `none`。

---

### 把這九題濃縮成五條原則

| # | 原則 | 我們身上發生的事 |
|---|---|---|
| 1 | **能跑的二進位比文件誠實** | `nbinfo` 告訴我們要 planar，文件沒說 |
| 2 | **先讓已知答案的東西走完全程** | MNIST 先通，才敢做猜拳 |
| 3 | **每一段之間都要能攔下來看** | `nbinfo` 驗介面、`score.py` 驗數值、`MODEL IN` 驗板子 |
| 4 | **一次只動一個變數** | 換模型時其他全部維持原樣 |
| 5 | **量了才知道瓶頸在哪** | 以為瓶頸是 NPU，其實是 1 ms tick |

**這五條跟 NPU 沒關係。** 換成任何一顆你不熟的加速器、DSP、協處理器，
流程都一樣——這才是真正可以帶走的東西。

---

## Part 1. 板子上有什麼

### AMB82-MINI（RTL8735B）

| 單元 | 是什麼 | 誰在用 |
|---|---|---|
| **KM4** | Cortex-M33 @ 500 MHz | 跑你的 sketch |
| **NPU** | VeriSilicon VIP8000 NANO，0.4 TOPS | 跑神經網路 |
| **VOE** | 影像協同處理器 | 管相機，**獨立於 KM4** |
| **ISP** | 自動曝光 / 白平衡 / 去噪 | 在 VOE 前面，硬體做 |
| **DDR** | 外掛記憶體 | 放模型權重和影像緩衝 |
| **Flash** | 其中 `nn` 分區 **10.56 MB**（0x530000, len 0xA90000） | 放 `.nb` |

**重點：這顆晶片裡有三個「處理器」在各做各的。**
KM4 跑你的程式、VOE 管畫面、NPU 算模型。
你的 sketch 大部分時間只是在**協調**它們，不是在做事。

### 跟其他兩塊板子比

| | **AMB82** | **Pico 2** | **RT584** |
|---|---|---|---|
| 核心 | M33 @500 MHz | M33 ×2 @150 MHz | M33 @48 MHz |
| NPU | ✓ 0.4 TOPS | ✗ | ✗ |
| RAM | 外掛 DDR | 520 KB | 192 KB |
| 模型空間 | 10.56 MB | 4 MB flash | 1952 KB flash |
| 相機 | ✓ 內建 | ✗ | ✗ |

**同一個 MNIST 模型跑三顆**（我們實測 + 推算）：

| | 時間 |
|---|---|
| AMB82 NPU | **0.12 ms**（實測） |
| Pico 2 CPU | **40 ms**（實測） |
| RT584 @48 MHz | ~126 ms（推算） |

---

## Part 2. 相機是怎麼接進來的

> 這是你問的第一個問題。答案比想像中「間接」。

### 資料路徑

```
鏡頭 sensor
   ↓  raw pixels
 ISP           自動曝光、白平衡、去噪（純硬體，你管不著）
   ↓
 VOE           ★ 這裡決定「開幾路、每一路什麼格式、什麼解析度」
   ├─ channel 0 : JPEG   320×240   →  我們拿來拍照傳給 PC
   ├─ channel 1 : （沒用）
   └─ channel 3 : RGB    224×224   →  餵給 NPU
   ↓
 StreamIO      ★ 把「某一路」接到「某個消費者」
   ↓
 NNImageClassification → NPU
```

### 關鍵觀念：KM4 從頭到尾沒有碰到畫素

這是最容易誤會的地方。看這行：

```cpp
Camera.getImage(CHANNEL_JPEG, &addr, &len);
```

它回傳的是**一個位址和一個長度**，不是一個陣列。
因為那張圖是 VOE 寫進 DDR 的，KM4 只是拿到「東西在哪裡」。

**同理，NPU 也不是從 KM4 手上拿圖**。StreamIO 把 channel 3 直接接到 NN，
畫素從 VOE 流到 NPU，全程不經過你的 sketch。

所以你的 sketch 實際上只做兩件事：

1. **開機時跟 VOE 說**：我要 channel 0 和 channel 3，各是什麼格式
2. **每一幀從 callback 收結果**：一個陣列，每類一個分數

```cpp
VideoSetting configJPEG(320, 240, 10, VIDEO_JPEG, 1);   // 1 = snapshot 模式
VideoSetting configNN(224, 224, 10, VIDEO_RGB, 0);
StreamIO videoStreamerNN(1, 1);                          // 1 進 1 出
```

### VOE 的三個脾氣（都踩過）

**1. channel 有「群組」，不能單獨開後面那組**

channel 0 屬於 group 0，channel 3 屬於 group 1。
只設定 channel 3 會失敗：

```
hal_video_open fail ret=88201c00, group=1
```

**channel 0 必須也被設定，而且要 `channelBegin`**，就算你根本不用它。

**2. 它會偷改你的解析度**

我們設 channel 0 是 320×240，實際拿到的快照是 **480×360**。
VOE 會把你的要求「吸附」到它支援的規格，而且不會告訴你。

> **所以永遠不要假設你設定的就是你拿到的。** 印出來看。

**3. VOE 不吃 reset**

按板子上的 RESET 只重啟 KM4，**VOE 還停在上一次的狀態**。
相機開不起來、或是行為很怪的時候，要**整個拔掉 USB、等 5 秒、再插回去**。

### StreamIO 還能拿來停 NN

```cpp
videoStreamerNN.pause();    // NN 停了
// ...做你的事...
videoStreamerNN.resume();
```

我們用這招解決了一個很難找的 bug：原廠 SDK 每辨識一幀就 `printf` 一行
`Image Classification tick[]`，跟我們的通訊協定**搶同一條 UART**，
在字元層級交錯（46 行資料壞了 11 行）。
傳圖前 `pause()` 就乾淨了。

---

## Part 3. 模型到底是什麼

### 一句話

> **模型 = 一張算圖 + 一堆數字。**

- **算圖**：先做卷積，再 pooling，再卷積……（結構，是人設計的）
- **數字**：每一層的權重（是訓練出來的）

我們的猜拳模型：

| | |
|---|---|
| 輸入 | 96 × 96 × 3（RGB） |
| 結構 | Conv16 → Pool → Conv32 → Pool → Conv64 → Pool → Conv64 → Pool → Dense64 → Dense3 |
| 參數 | 208,227 個數字 |
| 運算量 | 30.67 M MAC / 張 |
| 輸出 | 3 個機率，加起來 = 1 |

### 三種任務的差別

這決定了你能做什麼，也決定了模型多大。

| | **影像分類** | **物件偵測** | **關鍵點** |
|---|---|---|---|
| 例子 | 我們的猜拳 | YOLO | MediaPipe 手部 |
| 輸出 | 整張圖一個標籤 | 0~N 個框 + 信心 | 21 個座標，或 0 個 |
| 東西在哪裡 | **不知道** | 知道 | 知道 |
| 能說「沒有」嗎 | **不能** ← 見下 | 能 | 能 |
| 標註成本 | 低（分資料夾） | 高（每張畫框） | 零（不用訓練） |
| 模型大小 | 0.2 MB | 4 MB | 2.4 + 2.0 MB |

### ★ 分類模型說不出「我不知道」

這是我們花最久才想通的事。現象：**鏡頭前什麼都沒有，板子還是很有自信地說 `rock 99%`。**

原因不在模型，在數學。最後一層是 softmax：

```
p_i = exp(z_i) / ( exp(z_rock) + exp(z_paper) + exp(z_scissors) )
```

分母是三項的和，所以 **三個機率恆等於 1**。
不管餵它什麼——天花板、你的臉、一片黑——加起來永遠是 1。

> **這個架構在結構上就沒有能力輸出「都不是」。**
> 它只能回答「如果一定要三選一，最像哪一個」。

**兩個解法要一起用：**

1. **加一類 `none`**（治本）——讓「沒有手」變成模型真的學過的答案
2. **加信心門檻**（治標）——最高分低於門檻就回報 unknown

光加門檻不夠，因為神經網路對沒見過的東西**常常錯得很有自信**。

---

## Part 4. TensorFlow 在扮演什麼角色

> 這是你問的第二個問題。最短的答案是：**它只活在左半邊，板子上沒有它。**

### TensorFlow 做四件事

```
   一批圖片
      ↓
  ① 前向：照著算圖算一遍，得到預測
      ↓
  ② 算 loss：預測 跟 正確答案 差多少
      ↓
  ③ 反向：算出「每個權重該往哪邊動」（梯度）
      ↓
  ④ 更新：權重往那個方向動一小步
      ↓
   重複幾千次
```

`model.fit()` 這一行就是在跑這個迴圈。

### 它到哪裡為止

**訓練結束的那一刻，TensorFlow 的工作就完了。**
它交出一個檔案，裡面是那張算圖和那堆訓練好的數字：

```python
model.save("rps_cnn.h5")                                  # 算圖 + 權重
tf.lite.TFLiteConverter.from_keras_model(model).convert() # 同上,換個格式
```

之後的事 TensorFlow 完全不參與。**板子上沒有 TensorFlow，也不需要有。**

### 容易搞混的一點：`.tflite` 不是 runtime

`.tflite` 這個名字會讓人以為板子上要跑 TFLite。**在我們的路徑上不是。**

| 路徑 | `.tflite` 的角色 |
|---|---|
| **AMB82（我們）** | 只是**一種檔案格式**，給 acuity 讀進去轉成 `.nb`。板子不認得它 |
| Pico 2 / RT584 | 這時 **TFLite Micro 才是 runtime**，直接在 CPU 上解譯 `.tflite` |

為什麼我們用 `.tflite` 而不是 `.h5`？因為 Colab 現在是 Keras 3，
寫出來的 `.h5` 用新格式，acuity（停在 TF 2.14）**讀不懂**，
而且**不會當場報錯**，是轉檔時才炸。`.tflite` 是 FlatBuffer，多年沒變過。

### 一個有用的類比

| | 寫程式 | 訓練模型 |
|---|---|---|
| 你寫的 | `.c` 原始碼 | 模型架構 + 資料 |
| 工具 | 編譯器 gcc | TensorFlow |
| 中間產物 | `.o` | `.h5` / `.tflite` |
| 給目標硬體的 | `.bin` | **`.nb`** |
| 目標上跑的 | CPU | NPU |

**TensorFlow 對應的是編譯器，不是執行環境。**

---

## Part 5. NPU 到底在幹嘛

> 這是你問的第三個問題。

### 神經網路幾乎只在做一件事

卷積、全連接，拆開來全是同一個動作：

```
  把一串數字兩兩相乘，然後全部加起來
  acc += a[i] * b[i]
```

這叫 **MAC**（Multiply-Accumulate，乘加）。
我們的猜拳模型每辨識一張圖要做 **30,670,000 次** MAC。

### CPU 和 NPU 的差別

| | CPU（Cortex-M33） | NPU（VIP8000 NANO） |
|---|---|---|
| 怎麼做 | 一次算幾個，靠迴圈重複 | **一整片 MAC 陣列同時算** |
| 我們實測 | 4.50 cycles 做 1 個 MAC | 1 cycle 做 **22 個** MAC |
| 彈性 | 什麼都能跑 | 只能跑它支援的算子 |

差距接近 **100 倍**（時脈歸一化後 99.2×）。

### 實測數字（MNIST，1.34 M MAC）

| | cycles | cycles/MAC | 時間 |
|---|---|---|---|
| Pico 2 float32 naive | 49,148,544 | 36.6 | 328 ms |
| Pico 2 int8 naive | 17,240,686 | 12.8 | 115 ms |
| Pico 2 int8 + CMSIS-NN | 6,038,527 | 4.50 | 40 ms |
| **AMB82 NPU** | **60,900** | **0.045** | **0.12 ms** |

### 「快幾倍」要分四層講

只講一個倍數一定會被誤用：

| 層次 | 倍數 | 意思 |
|---|---|---|
| 端到端 | **13.4×** | 今天真的拿得到的 |
| 純計算 | ~330× | 含 3.33× 的時脈優勢 |
| 時脈歸一化 | **99.2×** | 這才是「NPU 架構本身強多少」 |
| NPU 利用率 | **5.5%** | 22 / 400 MAC per cycle |

**最後一列最重要。** 端到端只有 13.4×，是因為 **96% 的時間在等 FreeRTOS 的 1 ms tick**，
根本不是 NPU 慢。而 99.2× 是在 NPU **嚴重吃不飽**的情況下拿到的
（MNIST 太小：第一層只有 3 個輸入通道，MAC 陣列幾乎整片閒著）。

> 做系統設計時 **13.4× 比 330× 重要**——
> NPU 快到某個程度之後，瓶頸就換到驅動和作業系統了。

### NPU 不會「跑程式」

這是最後一個關鍵觀念，也是 Part 6 的前提：

> **NPU 沒有指令集給你寫。它吃的是一張「已經編譯好的算圖」。**

哪一層接哪一層、每一層的權重在哪、用什麼精度——
全部要事先算好、排好、包成一個檔案。那個檔案就是 `.nb`。

---

## Part 6. 為什麼需要轉檔

現在兩邊都清楚了，中間那個缺口就很明顯：

```
TensorFlow 給你的        NPU 要的
─────────────────       ──────────────────
.h5 / .tflite      ✗    .nb（NBG, network binary graph）
float32 權重        ✗    int8 / uint8
一張抽象的算圖       ✗    編譯好的 NPU 指令序列
```

**轉檔就是在補這個缺口。** 它做三件事：

### ① 編譯算圖

把「Conv → Pool → Conv」翻譯成這顆 NPU 的指令，決定每一層的資料怎麼擺、怎麼搬。

### ② 量化：float32 → uint8

權重從 4 bytes 變 1 byte。好處是**檔案小 4 倍、算得快、省電**。

但要壓得準，工具得先知道「實際跑的時候，每一層的數值大概在什麼範圍」。
它靠的是**拿幾十張真實圖片跑一遍去統計**——這就是**校正圖**（calibration）的用途。

- 挑太少 → 統計不準，精度崩掉
- **從 test set 挑**，因為要代表「上線後會看到的東西」
- 每類都要挑，不能全挑同一類

> ⚠ 官方的 `pegasus_quantize.sh` **寫死只跑 1 個 iteration**，
> 所以 200 張校正圖只會用到第 1 張。這個坑讓我們的 MobileNetV2 精度直接歸零。
> 一定要自己補 `--iterations`。

### ③ 把前處理編進模型

訓練時我們做了 `x / 255`。**這件事在板子上也必須做，否則數值全錯。**

但 SDK 的 `classification_preprocess()` 只做 resize，**完全沒有正規化**：

```c
img_resize_planar(&img_in, roi, &img_out);   // 就這樣,沒了
```

所以 `/255` 必須**編進 `.nb` 的第一層**，由 NPU 自己做（等於免費）。
做法是在 `inputmeta.yml` 裡：

```yaml
scale: 0.00392157          # = 1/255,對應訓練時的 x/255
add_preproc_node: true     # ← 不開的話模型轉得出來,但算出垃圾
preproc_type: IMAGE_RGB888_PLANAR
```

`channel_mean_value.txt` 寫的 `0 0 0 0.00392157` 就是這個意思
（R/G/B 的 mean 都是 0，scale 是 1/255）。

### 為什麼一定要「planar」

NPU 讀的排列是 **R 整張、G 整張、B 整張**，不是一個像素 RGB 連在一起。
`add_preproc_node` 沒開的話，輸出的是 interleaved，維度會變成 `[3, 96, 96]`，
裝置端讀 `dim[0].size[0]` 當寬度就拿到 3 ——幾何全錯。

---

## Part 7. 實作：怎麼訓練

📓 **教材**：[`colab/02_train_rps.ipynb`](colab/02_train_rps.ipynb) · [`colab/025_retrain_rps.ipynb`](colab/025_retrain_rps.ipynb)

### 流程

```
① 收資料      → 分成 dataset/rock, dataset/paper, ...
② 看資料      ← ★ 最常被跳過,也最常出事
③ 切 train / val / test
④ augmentation（只掛在 train）
⑤ 建模型
⑥ 量起跑線    ← 亂猜是多少?
⑦ 訓練
⑧ 誠實評分    ← 用 test set,不是調過參的 val set
⑨ 匯出
```

### 四條規則（每一條都是踩出來的）

**規則 1：augmentation 放資料管線，不能放進模型**

放進模型轉檔會失敗，因為 NPU 不認得那些層。這是能不能轉檔成功的分水嶺。

**規則 2：`test accuracy 1.0000` 是紅燈，不是成績**

我們第一次訓練拿到 `val 1.0000 / test 1.0000`，燒進板子幾乎全錯。

真實世界的分類問題不會 100%。滿分通常代表**測試集太簡單，或跟訓練集太像**。
那份資料是 CG 算圖的手、白底、光線一致——模型只要學會
「白底上那團膚色的輪廓」就能滿分，根本不需要理解手勢。

**規則 3：Domain gap 不能靠訓練更久解決**

| | 訓練資料 | 實際現場 |
|---|---|---|
| 背景 | 純白 | 桌面、螢幕、你的臉 |
| 光線 | 均勻打光 | 頂燈 + 窗戶逆光 |
| 影像 | CG 算圖 | 相機，有雜訊 |

**再訓練 1000 個 epoch 都不會變好**，因為模型從沒看過現場長什麼樣。
唯一的解法是**拿要部署的那台相機去收資料**。

**規則 4：連拍的照片不能隨機切 train/test**

第 37 張和第 38 張是同一隻手、同一個位置，差幾個像素。
隨機切的話 test 裡躺著 train 的雙胞胎 → **又是假滿分**。
要**按拍攝順序切** 70 / 15 / 15。

> 這叫 **data leakage**。只要資料有時間或群組結構，隨機切就是錯的。

### 匯出什麼

| 檔案 | 用途 |
|---|---|
| `rps_cnn.tflite` | 給 acuity 的模型本體 |
| `calib/*.png` + `dataset.txt` | 量化用的校正圖 |
| `channel_mean_value.txt` | `0 0 0 0.00392157` |
| `labels.txt` | 類別順序，板子端要對得起來 |

---

## Part 8. 實作：怎麼轉檔

環境在 WSL2 的 Docker 容器裡（`sh dk.sh '<指令>'`）。

```bash
# ① import：tflite → acuity 中間表示
sh dk.sh 'cd Models/rps_cnn && bash pegasus_import.sh rps_cnn'

# ② ★ 改 inputmeta —— 不改的話後面全錯
#    scale            1.0   → 0.00392157
#    add_preproc_node false → true
#    preproc_type     IMAGE_RGB → IMAGE_RGB888_PLANAR
#    ⚠ 這個檔案不准有 TAB,只能用空白縮排

# ③ quantize（用自己寫的,官方那支漏了 --iterations）
sh dk.sh 'bash /workspace/quant_mnist.sh'

# ④ ★ 驗證數值 —— 不要看到 Error(0) 就以為成功
sh dk.sh 'bash /workspace/verify_mnist.sh'
sh dk.sh 'python3 /workspace/score_mnist.py'
#    要看到 float / uint8 準確率接近、預測一致率高才算過

# ⑤ export → .nb
sh dk.sh 'cd Models/rps_cnn && bash pegasus_export_ovx.sh .'

# ⑥ ★ 檢查介面 —— 唯一能事先抓到「轉得出來但算出垃圾」的方法
./nbinfo -in  network_binary.nb     # 要看到 96,96,3,1 / UINT8 / scale 1.0
./nbinfo -out network_binary.nb     # 要看到 N 個 FP16
#    ⚠ 選項不能合併,-in -out 寫一起只會印第一張表
```

**第 ④ 步不能跳。** 之前做 MobileNetV2 時跳過，結果 `Top5: 0.000000`，
花了很久才發現是量化校正只用了 1 張圖，模型本身沒壞。

---

## Part 9. 實作：怎麼燒錄、板子端怎麼用

### 換模型

```
packages/realtek/tools/ameba_pro2_tools/1.4.7/img_class_cnn.nb      ← ★ 真正的打包來源
hardware/AmebaPro2/4.1.0/variants/common_nn_models/img_class_cnn.nb ← 順手也蓋
```

**為什麼是覆蓋內建模型，而不是用「自訂模型」選項？**

因為 `nn_models.json` 有個原廠缺陷：

```json
"model_mappings": { "CUSTOMIZED_IMGCLASS": "img_class_cnn" }
"nb_file_mapping": { ...沒有 "img_class_cnn" 這個 key... }
```

→ 那幾個 `CUSTOMIZED_*` 列舉根本不能用。
所以我們用 `DEFAULT_IMGCLASS` + 覆蓋掉內建的 `.nb`。

### 進下載模式

> **按住 BOOT（UART_DOWNLOAD） → 按一下 RESET → 放開 BOOT**，然後 Upload。

### 板子端的 sketch 在做什麼

```cpp
// 1) 告訴 VOE 要哪幾路
VideoSetting configJPEG(320, 240, 10, VIDEO_JPEG, 1);
VideoSetting configNN(224, 224, 10, VIDEO_RGB, 0);

// 2) 選模型、設回呼
imgclass.modelSelect(IMAGE_CLASSIFICATION, NA_MODEL, NA_MODEL,
                     NA_MODEL, NA_MODEL, DEFAULT_IMGCLASS);
imgclass.configInputImageColor(1);   // ★ 1 = RGB。傳 0 會偷偷轉灰階
imgclass.setResultCallback(onResult);

// 3) 接線
videoStreamerNN.registerInput(Camera.getStream(CHANNELNN));
videoStreamerNN.setStackSize();
videoStreamerNN.registerOutput(imgclass);
```

### 一個容易中的陷阱

```cpp
imgclass.configInputImageColor(0);   // 巨集叫 IMAGERGB,但傳 0 會呼叫 img_rgb2gray()
```

**名字騙人，RGB 模型一定要傳 1。**

### 分數怎麼讀

`ImageClassificationResult::score()` 回傳 `(int)(prob * 100)`，是 **0~100 的整數**，
不是 0.0~1.0 的浮點數。設信心門檻時要注意。

---

## Part 10. 我們踩過的坑

| # | 坑 | 症狀 | 解法 |
|---|---|---|---|
| 1 | `add_preproc_node` 沒開 | 轉得出來，算出垃圾 | inputmeta 改三行 |
| 2 | 官方 quantize 只跑 1 iteration | 精度歸零 | 自己補 `--iterations` |
| 3 | 看到 `Error(0)` 就以為成功 | 燒進去才發現全錯 | **一定要驗證數值** |
| 4 | Keras 3 的 `.h5` acuity 讀不懂 | 轉檔時才炸 | 改用 `.tflite` |
| 5 | `nn_models.json` 的 CUSTOMIZED 列舉是壞的 | 模型載不起來 | 覆蓋內建 `.nb` |
| 6 | `configInputImageColor(0)` | 偷偷轉灰階 | 傳 1 |
| 7 | VOE 不肯單獨開 group 1 | `hal_video_open fail` | channel 0 也要開 |
| 8 | VOE 不吃 RESET | 相機行為很怪 | 拔 USB 等 5 秒 |
| 9 | 原廠 log 跟我們搶 UART | 46 行壞 11 行 | `pause()` + 整行一次 write |
| 10 | `val acc 1.0000` 當成功 | 上線幾乎全錯 | 看 domain gap |
| 11 | softmax 沒有 unknown | 空景報 `rock 99%` | 加 `none` 類 + 門檻 |

**這張表其實比前面九章都有價值**，因為每一條都是「照文件做會失敗」的地方。

---

## Part 11. 你現在學到哪了

| 階段 | 狀態 |
|---|---|
| 理解 NPU / CPU 的差別，而且有自己量的數字 | ✅ |
| 走完一次 訓練 → 轉檔 → 燒錄 全流程（MNIST） | ✅ |
| 第二次全流程（猜拳，自己設計的模型） | ✅ |
| 在板子上跑起來、接 PC 工具、看到即時結果 | ✅ |
| 診斷並修好一個真的很難找的 bug（UART 交錯） | ✅ |
| 理解模型為什麼在實驗室滿分、在現場失敗 | ✅ |
| **自己收資料、自己重訓** | 🔲 進行中 |
| 加 `none` 類 + 信心門檻 | 🔲 |
| 把重訓的模型轉檔、燒錄、跟舊的比 | 🔲 |

### 還沒解決的（含已知限制）

**板子 / 模型**

- **板子端管線正確性未驗證**——`MODEL IN` 診斷已經燒進去了，
  按一次就能看到 NPU 真正吃到的那張圖，但還沒看過。**這是下一步第一件事。**
- 板子韌體還是 `NUM_CLASSES 3`、沒有 `CONF_THRESHOLD`
  （故意先不改：4 類韌體配 3 類模型會壞，要等新 `.nb` 出來一起換）
- **NPU 只用到 5.5%** 算力——模型太小、通道太窄

**跑分上的硬限制（不是我們的問題，是驅動的）**

- **牆鐘量不到 121.8 µs**：`vpmdENABLE_POLLING = 0`，`vip_run_network()`
  阻塞在 OS wait 上，`configTICK_RATE_HZ = 1000`，所以每次呼叫至少吃 ~3 個 tick
  （實測固定 2,995 µs）。`libnn.a` 是預編的，改不了。
  **單張推論的端到端時間就是 ~3 ms，這是真實的部署限制**，只有 batch 才攤得掉。
- `hw_avg_us` 印 1 是假的（來源是 OS tick，解析度不夠），**要看 `total_cycle`**。
- 「250 MHz」那行 log 只是回顯 `#define`，**不是暫存器回讀**。
  時鐘域的結論靠的是 cycle 數變化（−2.8%），不是那行字。
  要決定性證明就讀回 `SYSON_S_REG_SYS_NN_CTRL`（offset `0x11C`，
  欄位 `SYSON_S_MASK_SYS_NN_SRC_SEL` = `0x3 << 3`）。
- VOE / 相機在跑分 sketch 裡是**繞開**的，不是修好的（跑分不需要相機）。

**環境**

- WiFi 不通（公司 802.1X，`WiFi.begin()` 只吃 WPA2-PSK）→ 靠 RTSP 的範例要用手機熱點
- SD 卡載模型還沒試（有讀卡機了）。⚠ 切成 SD 載入後，開機找不到 `.nb` 會**直接卡住**

---

### 下一步

**主線（重訓，正在做）**

1. 按 `MODEL IN`，確認板子餵給模型的圖是正常的 ← **先做這個，不然後面都是白做**
2. 用 `tools/rps_gui.py` 收真實照片（四類，每類 150 張以上）
3. 跑 `colab/025_retrain_rps.ipynb`
4. 轉檔、燒錄，**跟舊模型對照**——同一個位置比十次剪刀，各對幾次
5. 板子韌體改 `NUM_CLASSES 4` + 加 `CONF_THRESHOLD`

**收尾（隨時可做，不影響任何結論）**

6. 還原 `_backup_amb82/` 的原廠 `.nb`；刪掉 `_bp_yolo*` 這些殘骸 build 目錄
7. 把兩個原廠缺陷補進 `realtek-email-B-bugreport.md` 再寄出：
   量化腳本兩處 `--iterations 1` 寫死、`nn_models.json` 的 `CUSTOMIZED_IMGCLASS`
   對應到沒有 `nb_file_mapping` 的 key
8. 縮 GitHub PAT 權限

**如果要把跑分挖更深（選做，每項都有明確假設要驗）**

9. **Pico 2 權重搬進 SRAM**：驗「4.50 cycles/MAC 是被 flash XIP 拖累」。
   搬進去掉到 2 附近就成立（目前只用 139 KB / 520 KB，搬得進去）
10. **AMB82 上跑同一份 CMSIS-NN**：拿到「同一顆晶片、CPU vs NPU」這個唯一
    完全誠實的加速比。先確認 KM4 有沒有定義 `__ARM_FEATURE_DSP`
11. **讀回 NN 時鐘暫存器**：讓時鐘域從「強烈推論」變成「直接觀測」

---

## Part 12. 環境怎麼建起來

### 需要的東西

| 元件 | 版本 | 怎麼拿 |
|---|---|---|
| Docker（跑 acuity 容器） | 任何近期版本 | Win 公司機：**WSL2 裡的 Docker Engine**（不是 Docker Desktop，會要提權）<br>Mac：Docker Desktop 直接裝 |
| acuity 容器 image | `ghcr.io/ameba-aiot/acuity-toolkit:6.18.8` | `docker pull`，要 GitHub PAT（`read:packages`） |
| acuity 範例包 | `acuity_examples_c901149.tgz`（318 MB） | **不在 repo 裡**，憑 Realtek 邀請自己下載 |
| Verisilicon NBInfo | 1.2.17 | **不在 repo 裡**，隨離線工具包一起取得 |
| Arduino AmebaPro2 board package | 4.1.0 | Arduino IDE 2.x 板子管理員 |

> **為什麼授權包不放 repo**：那是憑邀請才拿得到的，授權範圍不明，不散佈。
> 這也是為什麼這個 repo 必須 **private**。

### 目錄長怎樣

**repo 在一邊，工具包在另一邊**：

```
D:\workdir\ameba\                (Windows，= 這個 git repo)
├── COURSE.md                    ★ 這份文件,唯一的正式文件
├── colab/                       教材 notebook（訓練都在這裡跑）
│   ├── 01_train_mnist.ipynb         Lesson 1  MNIST
│   ├── 02_train_rps.ipynb           Lesson 2  猜拳（含後記：為什麼上線會失敗）
│   └── 025_retrain_rps.ipynb        Lesson 2.5 用自己的相機重訓 + none 類
├── scripts/                     容器內用的腳本 + notebook 產生器
│   ├── dk.sh                        進容器執行指令
│   ├── quant_mnist.sh               ★ 自己寫的量化（官方那支壞的）
│   ├── verify_mnist.sh / score_mnist.py   ★ 數值驗證
│   └── gen_lesson2.py / gen_lesson25.py   ★ notebook 的正本
├── MnistNpuBench/               AMB82 跑分 sketch
├── RpsGame/                     猜拳 sketch（含 MODEL IN 診斷）
├── NbBenchSD/                   從 SD 卡載模型的實驗
├── tools/rps_gui.py             PC 端 pygame 工具（對戰 + 收訓練資料）
├── pico2/MnistPicoBench/        Pico 2 跑分專案（CMake，跟 AMB82 完全獨立）
├── model/ models/               h5 / nb / tflite / inputmeta
├── docs/research-log.md         歷程原始紀錄（有已被推翻的結論，只當歷史看）
├── _backup_amb82/               ★ 板子原廠檔備份，不在 git 裡
└── acuity_examples_c901149.tgz  ★ 授權工具包，不在 git 裡

~/ameba-toolkit/                 (WSL 或 Mac 家目錄，工具包解開的地方)
└── acuity_examples_c901149/Models/<模型名>/    ← 轉檔實際發生的地方
```

⚠ **`scripts/` 不會自動同步。** 在容器裡改了腳本，記得複製回 `scripts/` 再 commit。

### 建置步驟

```bash
# 1. Docker 起來
#    Windows：WSL2 沒有 systemd，systemctl 沒用，要手動跑 dockerd。
#    daemon 會跨 wsl session 存活，只有 wsl --shutdown / 重開機才要再跑一次。
wsl -d Ubuntu -e sh /mnt/d/workdir/ameba/wsl-docker-up.sh
#    Mac：開 Docker Desktop 就好

# 2. 抓 image（PAT 當密碼）
docker login ghcr.io
docker pull ghcr.io/ameba-aiot/acuity-toolkit:6.18.8

# 3. 攤開工具包 + 放好腳本
mkdir -p ~/ameba-toolkit && cd ~/ameba-toolkit
tar xzf /path/to/acuity_examples_c901149.tgz
cp /path/to/repo/scripts/*.sh /path/to/repo/scripts/*.py .
chmod +x *.sh
```

### Pico 2 那邊（只有做跑分對照時才需要）

跟 AMB82 完全獨立，不用 Docker、不用 acuity。

| 元件 | 版本 | 位置 |
|---|---|---|
| pico-sdk | 2.1.1 | `~/.pico-sdk/sdk/2.1.1` |
| arm-none-eabi-gcc | 15_2_Rel1 | `~/.pico-sdk/toolchain/` |
| CMSIS-NN | 7.0.0 | `D:/workdir/pico-deps/CMSIS-NN` |

```bash
cd pico2/MnistPicoBench
cmake -S . -B build -G Ninja \
      -DPICO_SDK_PATH=C:/Users/M90t/.pico-sdk/sdk/2.1.1 \
      -DCMSIS_NN_PATH=D:/workdir/pico-deps/CMSIS-NN
cmake --build build          # 產出 build/mnist_pico_bench.uf2
```

燒錄：按住 BOOTSEL 插 USB → 出現 `RPI-RP2` 磁碟 → 把 `.uf2` 拖進去。

⚠ **VSCode 裡不要按「Import Project」** ——它在 Windows 上是壞的，
實際跑 `pico_project.py --convert`（官方 help 自己寫 "risks data loss"），
會就地改寫我們的 `CMakeLists.txt`。直接 `Open Folder` 開專案本身。

⚠ **跑分數字必須來自同一套 toolchain。** 兩套實測差 0.5%，
不大但足以污染小幅度的比較。目前的數字是 15_2_Rel1。

### 環境限制（不要忘記）

- **公司電腦沒有管理員權限**，不要觸發 UAC：不裝 Docker Desktop、不跑 `winget install`、
  不寫 HKLM / Program Files、不裝服務或驅動。現有的 WSL2 Ubuntu 和裡面的 `apt` 可以用。
- **不要在 Arduino IDE 可能正在編譯時同時用 `arduino-cli`** ——會撞同一個 build 目錄。
  用 cli 一定加 `--build-path` 指到別的地方。
- **PAT 權限偏大**（`delete_repo, repo, write:discussion, write:packages`），
  而且在 `/root/.docker/config.json` 裡是 base64 編碼、**沒有加密**。
  只拉 image 的話 `read:packages` 就夠，建議分兩把。

### Mac 上的注意事項

- **M 系列（ARM）Mac**：acuity image 幾乎確定只有 `linux/amd64`，會用 Rosetta 模擬。
  **能動，但會慢**——訓練那一分鐘可能變三五分鐘。Intel Mac 沒這個問題。
- 板子端完全沒問題，port 名字是 `/dev/cu.usbserial-*`，不是 `COM6`。
- GUI 工具用 **pygame**，不要用 tkinter（Mac 上有問題）。

---

## 附錄 A：兩個「為什麼當初這樣決定」

### 為什麼 MNIST 也要做成 3 通道（不是 1 通道灰階）

因為 SDK 的 `img_t` **沒有 channel 欄位**：

```c
typedef struct { int width; int height; uint8_t *data; } img_t;
int img_resize_planar(img_t *im_in, rect_t *roi, img_t *im_out);
```

所以 `img_resize_planar()` **永遠寫 W×H×3 bytes**。
模型若宣告 1 通道，tensor buffer 只有 W×H，會**溢出 2×W×H**。

裝置端所謂的「灰階」其實是：3 通道 buffer，`configInputImageColor(0)`
事後把 plane 0 覆寫成灰階（plane 1、2 還是原本的 G、B）。
所以自訓模型一律做 3 通道，sketch 裡傳 1。
訓練時用 `np.repeat(x[..., None], 3, axis=-1)` 把灰階攤成 R=G=B。

**代價**：Pico 2 那邊第一層要多算 3 倍。
**為什麼還是接受**：跑分的前提是兩邊跑同一個模型，公平性優先於單邊效率。

### 為什麼 MNIST 在容器裡訓練，猜拳卻在 Colab

acuity 容器裡是 **Python 3.8 / TF 2.10 / Keras 2**，`pegasus import keras` 就是配 Keras 2 寫的。
Colab / Kaggle 現在預設 **Keras 3**，存出來的 `.h5` acuity 讀不懂，而且**不會當場報錯**。

| | 訓練在哪 | 交出去的格式 | 為什麼 |
|---|---|---|---|
| MNIST | 容器裡 | `.h5` | 訓練跟轉檔同一個 TF，沒有版本問題；106K 參數 CPU 一分鐘訓完 |
| 猜拳 | Colab | **`.tflite`** | 要 GPU、要教材形式；`.tflite` 是 FlatBuffer，跨版本穩定 |

> **結論：跨版本要交換模型，用 `.tflite` 不要用 `.h5`。**

---

## 附錄 B：名詞對照

| 名詞 | 是什麼 |
|---|---|
| **MAC** | 乘加運算。神經網路 99% 的工作 |
| **NBG / `.nb`** | Network Binary Graph，NPU 吃的編譯後算圖 |
| **VOE** | AMB82 的影像協同處理器，管相機 |
| **StreamIO** | 把某個 camera channel 接到某個消費者 |
| **量化** | float32 權重壓成 int8/uint8 |
| **校正圖** | 量化時用來統計數值範圍的圖片 |
| **preproc node** | 編進模型第一層的正規化，由 NPU 做 |
| **planar** | R 整張、G 整張、B 整張（相對於 interleaved） |
| **arena** | 推論時存放中間結果的記憶體 |
| **domain gap** | 訓練資料和實際現場的落差 |
| **data leakage** | 資訊從 train 漏到 test，導致假的高分 |

# Ameba Edge AI 練習區 (AmebaPro2 / AMB82-MINI)

## 目前進度
- [x] **Blink 燒錄成功**（2026-09-08，COM51，LED 正常閃爍）
- [x] **第一個 AI 範例：AudioClassification 編譯通過**（84% flash，待燒錄驗證）

## Board package
Additional Boards Manager URL（stable）:
```
https://github.com/Ameba-AIoT/ameba-arduino-pro2/raw/main/Arduino_package/package_realtek_amebapro2_index.json
```
dev 版把 `main` 換成 `dev`、index 檔名換成 `package_realtek_amebapro2_early_index.json`。

## 燒錄流程（最容易踩的一步）
AmebaPro2 不像 ESP32 會自動進 download mode，每次上傳前：
**按住 BOOT(UART_DOWNLOAD) → 按一下 RESET → 放開 BOOT**，然後才按 Upload。
卡在 flash 失敗 99% 是這步漏了。

## 開發環境：Arduino IDE 2.x（已決定）
不走 VSCode。原因：
- `moozzyk.arduino` 0.0.4 只有語法高亮 + snippets，`contributes` 裡沒有 `commands`，
  所以裝了也不會出現任何 Verify / Upload 按鈕（不是裝壞）。
- 官方 `vsciot-vscode.vscode-arduino` 已停止維護，而且它本身不含編譯器，
  只是去呼叫 arduino-cli / Arduino IDE 1.8.x。

### 安裝位置
```
IDE 主程式：%LOCALAPPDATA%\Microsoft\WinGet\Packages\ArduinoSA.IDE.stable_Microsoft.Winget.Source_8wekyb3d8bbwe\Arduino IDE.exe
內建 CLI：  同目錄\resources\app\lib\backend\resources\arduino-cli.exe   (1.5.1)
IDE 設定檔： %USERPROFILE%\.arduinoIDE\arduino-cli.yaml
板子包資料： %LOCALAPPDATA%\Arduino15
sketchbook： %USERPROFILE%\Documents\Arduino
```

重點：Arduino IDE 2.x **內建一份 arduino-cli**，所以不用另外裝。
要用指令列做任何事，都記得帶 `--config-file`，才會跟 IDE 用同一份資料夾：
```powershell
$ide = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\ArduinoSA.IDE.stable_Microsoft.Winget.Source_8wekyb3d8bbwe"
$cli = "$ide\resources\app\lib\backend\resources\arduino-cli.exe"
$cfg = "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml"
& $cli --config-file $cfg board listall ameba
```

### 怎麼開 IDE
winget 裝的是**可攜版**：只在 PATH 加了 `Arduino IDE` 命令別名，
**不會**自動建立開始功能表 / 桌面捷徑（所以一開始會找不到程式）。
捷徑已補建在桌面和開始功能表。也可以直接在終端機打：
```powershell
& "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\ArduinoSA.IDE.stable_Microsoft.Winget.Source_8wekyb3d8bbwe\Arduino IDE.exe"
```

### board package 已預先設好
`~/.arduinoIDE/arduino-cli.yaml` 裡已經寫入 AmebaPro2 的 additional_urls，
所以 IDE 一開就看得到 `realtek:AmebaPro2`（版本 4.1.0），
**不需要**再手動貼 Preferences → Additional Boards Manager URLs。

## AI 範例：怎麼在「沒有 SD 卡 + 不能用 WiFi」的條件下挑
公司網路多為 802.1X 企業認證，Arduino 的 `WiFi.begin(ssid, pass)` 只吃 WPA2-PSK，接不上。
加上手邊沒有 SD 卡，所以範例篩選結果：

| 範例 | SD | WiFi | 可用 |
|---|---|---|---|
| **AudioClassification** | 否 | **否**（include 是殘留，全程沒呼叫）| ✅ **唯一** |
| ObjectDetectionImage | **要**（讀 SD 上的 image_list.txt）| 否 | ❌ |
| ObjectDetectionLoop / Callback | 否 | 要 | ❌ |
| HandGestureDetection | 否 | 要 | ❌ |
| RTSPFaceDetection / Recognition | 否 | 要 | ❌ |
| ObjectDetectionSaveSDCard | 要 | 要 | ❌ |

**注意：不要只 grep `#include` 就判斷依賴。** `ObjectDetectionImage` 沒 include WiFi.h，
但它改用 SD 卡當輸入；`AudioClassification` include 了 WiFi.h 卻從沒呼叫。要看實際呼叫。

### 模型檔位置
```
.../AmebaPro2/4.1.0/variants/common_nn_models/*.nb
  yolov7_tiny / yolov4_tiny / yolov3_tiny   物件偵測
  scrfd_500m_bnkps_*                        人臉偵測
  mobilefacenet_int8 / int16                人臉識別
  yamnet_fp16 / yamnet_s_hybrid             音訊分類
  palm_detection_lite / hand_landmark_lite  手勢
  mobilenetv2 / img_class_cnn               影像分類
```
（不在 `tools/` 底下，是在 `variants/`。）

### 坑：範例的 ClassList 標頭要一起複製
`AudioClassList.h`（30KB，YAMNet 521 類別名 + `filter` 開關）
放在**範例資料夾內**，不在 library 的 src 裡。
把 .ino 複製到自己的專案時，這個標頭必須一起複製，否則：
```
fatal error: AudioClassList.h: No such file or directory
```
物件偵測的 `ObjectClassList.h`、影像分類的 `ClassificationClassList.h` 同理。

`filter` 欄位設 0 就不印那一類，可以用來只留你在意的聲音。

### flash 容量
NN Model Load From 設 Flash 時，模型會跟 firmware 一起打包：
```
Blink                 4788224 bytes  (28%)
AudioClassification  14118912 bytes  (84%)   ← 含 YAMNet
```
單一模型沒問題，但要同時載多個模型很可能爆 16MB，那時才需要 SD 卡。

## 自訂模型 / 轉檔（研究結論，2026-09-08）

### NPU 與模型格式
RTL8735B 的 NPU 是 **VeriSilicon VIP**（SDK 內含 `VIPLiteDrv_2.0.0`），
所以模型格式是 `.nb`（Network Binary）。
**板子包沒有附轉檔器**，轉檔工具要另外從 Realtek 取得。

### 阻擋 1：自訂模型只能從 SD 卡載入
`libraries/NeuralNetwork/src/SD_Model.cpp` 裡路徑是寫死的：
```c
static void *yolov7_get_SD_filename(void) {
    return (void *)"sd:/NN_MDL/yolov7_tiny.nb";
}
```
十個 `CUSTOMIZED_*` 全指向 `sd:/NN_MDL/*.nb`：
```
yolov3_tiny  yolov4_tiny  yolov7_tiny  scrfd  mobilefacenet
yamnet  imgclassification  mobilenetv2_int16  palm_detection  hand_landmark
```
`Tools -> NN Model Load From: Flash` **只對內建預設模型有效**（打包進 firmware）。
自訂模型沒有 flash 路徑 → **沒有 SD 卡就完全不能用自訂模型**。

### 阻擋 2：只有 8 種 pre/post processor，沒有 FOMO
```
yolo  scrfd  mbfacenet  yamnet  classification  mbnv2  palm_detection  hand_landmark
```
`CUSTOMIZED_YOLOV7TINY` 用的仍是 `yolo_preprocess` / `yolo_postprocess`，
代表**自訂模型必須輸出跟 YOLO 相同的張量格式**。

**FOMO 不能用**：它輸出網格中心點熱圖（grid centroid heatmap），
跟 YOLO 的 anchor/box 格式不相容。就算 `.nb` 轉成功，餵進去也只會得到垃圾。
這是架構不相容，不是轉檔問題。要接 FOMO 得自己寫 postprocess，
那要進 C SDK 改 `module_vipnn`，已離開 Arduino 層。

### 官方支援的自訂路徑：重訓 YOLOv7-tiny
SDK 附了腳本：
```
libraries/NeuralNetwork/src/Yolov7_reparam_scripts/
  reparam_yolov7-tiny.py
  yolov7-tiny-deploy.yaml
```
流程：自己資料集重訓 YOLOv7-tiny -> reparam -> 轉 .nb -> 放 SD 卡 `NN_MDL/`
-> sketch 用 `CUSTOMIZED_YOLOV7TINY`。
做「蘋果偵測」要走這條，不是 FOMO。

### 待辦
- **買一張 microSD 卡**，這是任何自訂模型的前提
- 轉檔失敗的實際錯誤訊息還沒拿到，需要知道用的是哪個轉檔器 + 完整錯誤

## ★ 可行路線:改 mnist_train_colab -> 自訂 CNN(2026-09-09 確認)

**這是沒有離線工具時唯一走得通的路。**
官方「改用 MobileNetV2」那句是在「你有離線工具」的前提下講的;
線上轉檔器**不支援 MobileNetV2**,所以線上這條路要用**樸素 Keras CNN**。

### 證據
1. 線上轉檔器輸入格式含 `.h5`(for YAMNet and **CNN models**)-> Sequential CNN 在範圍內
2. SDK 有獨立分支,範例註解直接寫明:
```cpp
imgclass.configInputImageColor(IMAGERGB);   // only valid use for custom CNN model (e.g sequential)
imgclass.modelSelect(IMAGE_CLASSIFICATION, ..., CUSTOMIZED_IMGCLASS);
//   MobileNetV2 -> DEFAULT/CUSTOMIZED_IMGCLASS_MOBILENETV2
//   custom CNN (e.g sequential) -> DEFAULT/CUSTOMIZED_IMGCLASS
```
(`sequential` = Keras Sequential;`IMAGERGB` = 0)
3. **輸入尺寸與類別數都不用遷就內建模型**,runtime 從模型自己讀:
```c
// classification_preprocess:從模型讀輸入尺寸,再把相機畫面 resize 進去
img_out.width  = tensor_param->dim[0].size[0];
img_out.height = tensor_param->dim[0].size[1];
// classification_postprocess:類別數也從模型輸出讀
int class_cnt = fmt.dim->size[0];
```
4. `img_rgb2gray()` 存在 -> 連 1 通道灰階模型都支援(MNIST 形狀本身就在支援範圍)

### notebook 要改的地方
| 項目 | MNIST 原本 | 改成 |
|---|---|---|
| 輸入 | 28x28x1 灰階 | 96x96x3 或 128x128x3 **RGB**(相機餵 planar RGB) |
| 輸出 | 10 類 | 你要的 N 類(自由) |
| 結構 | Conv2D/MaxPool/Dense | **保持樸素**,不要 HardSwish 或花俏算子 |
| 存檔 | — | `model.save('m.h5')` — **要 .h5**,不是 .keras / SavedModel |

### ⚠ 最可能的失敗原因:TensorFlow 版本
官方線上轉檔文件明寫:
> When training model: supported tensorflow version up to **2.14.1**

Colab 預設 TF 遠比這新,新版 Keras 3 連 `.h5` 存檔都已 legacy 化。
**在 notebook 最前面把 TF 釘死 `2.14.1`。** 這很可能就是之前轉不過去的原因。

坑:TF 2.14.1 需要 Python <= 3.11。若 Colab runtime 已是 3.12,
`pip install tensorflow==2.14.1` 會裝不起來 -> 得換 runtime 或改本機訓練。

### 完整流程
1. Colab 釘 TF 2.14.1 -> 改輸入/輸出 -> 訓練 -> 存 `.h5`
2. 準備最多 10 張量化用圖片
3. 上傳線上轉檔器 -> `.nb` 由 **email 寄下載連結**
4. 改名 `imgclassification.nb` -> 放 `sd:/NN_MDL/`
5. sketch 用 `CUSTOMIZED_IMGCLASS` + `configInputImageColor(IMAGERGB)`

**✅ 不需要 SD 卡** —— 見下方「自訂模型走 flash」一節(先前結論已修正)。

參考範例:`libraries/NeuralNetwork/examples/RTSPImageClassification/`
線上轉檔文件:https://ameba-doc-arduino-sdk.readthedocs-hosted.com/en/latest/ameba_pro2/amb82-mini/Other_Guides/AI_Related_Guides/Online%20AI%20Model%20Conversion%20Guide.html

## ◆ 真正的目標:MNIST 跑分,AMB82 vs Pico 2

**目標不是做分類器,是比較推論速度。** 所以:
- 用**原本的 MNIST 0~9 notebook**,不要換成 garbage-cnn 的資料集
- 兩邊必須是**同一個模型**,否則比較沒意義
- 只從 garbage-cnn 借那兩個設定(跟資料集無關):
```python
!pip install tensorflow==2.14.1
model.save('mnist_cnn.h5', include_optimizer=False)
```

MNIST 形狀跟這塊板子天生吻合:28x28x1 灰階
-> `configInputImageColor(0)` 會做 rgb2gray;輸入尺寸與 10 類都從 `.nb` 讀。

### 訓練平台:建議 Kaggle 而非 Colab

> **已被實測推翻**:Kaggle 是 Python 3.12,鎖不了 TF 2.14.1。真正該做的是切回 Keras 2,見本檔最後一節。
唯一的大風險是 `tensorflow==2.14.1` 裝不起來(它需要 Python <= 3.11,
Colab runtime 可能已是 3.12)。Kaggle 可鎖環境版本,且參考 notebook
本來就在 Kaggle 上、已證實可跑。Colab 能裝起來的話也行。

### 測量陷阱(最容易做錯的地方)
| 錯的做法 | 為什麼錯 |
|---|---|
| 數 callback 次數當 FPS | NN 通道被設成 **10 fps**,量到的是節流閥不是 NPU |
| 用內建 `NN Inference` 計時器 | 那個字串屬於 `ainr.c`(nnlite 路徑),**我們這條路碰不到** |

```cpp
VideoSetting configNN(NNWIDTH, NNHEIGHT, 10, VIDEO_RGB, 0);
//                                       ^^ 就是這個 10 fps
```

### 正確量法
1. 把 `configNN` 的 fps 往上調(10 -> 30 -> 60 ...)直到**飽和**
2. callback 裡用 `micros()` 量相鄰兩次間隔
3. 間隔一路下降到某個值不再變小 -> **那個下限就是 NPU 真實吞吐量**
4. Pico 2 用同樣定義量(全速迴圈 Invoke,數每秒次數)

### module_vipnn 自己的 FPS 輸出(可驗證的預測)
`module_vipnn.c.obj` 裡有這些字串:
```
>>> %s FPS = %0.2f
%s tick[%ld]
inference_time
nn_fps
```
所以燒進去後 Serial 上**可能**直接出現 `>>> Image Classification FPS = xx.xx`。
先燒 AudioClassification 就能順便確認有沒有這行。

### 預期結果(心理準備)
MNIST 28x28 太小,**NPU 的優勢在大模型才顯現**。小模型上固定開銷
(驅動呼叫、DMA、tensor 設定)可能佔大部分時間 ->
**NPU 可能不會比 Pico 2 快多少,某些量法下甚至更慢。**
這本身就是有價值的結論,不是實驗失敗。

### 公平性問題
AMB82 這條路每幀含**相機擷取 + resize + 灰階轉換**;
Pico 2 上大概是餵固定陣列 -> **不對等**。
要嘛兩邊都算端到端,要嘛在結論裡講清楚差異。

進階:`model_classification.c` 是原始碼,裡面有被註解掉的
`memcpy(tensor_in_ptr, golden_data_bin, golden_data_bin_len);`
就是用來餵固定資料排除相機變因的。基本流程跑通後再考慮。

## ★★★ 已驗證的成功案例(最重要的一節)

**別自己從 MNIST 重寫 —— 直接複製這份已驗證的 notebook。**

- 論壇成功案例:https://forum.amebaiot.com/t/amb82-mini-garbage-classification/3375
- 訓練 notebook:https://www.kaggle.com/code/rkuo2000/garbage-cnn
- 完整教學(中文):https://github.com/rkuo2000/EdgeAI-AMB82mini `README.md`
- dataset 範例:https://www.kaggle.com/datasets/asdasdasasdas/garbage-classification

### 官方教學原文(rkuo2000/EdgeAI-AMB82mini)
```
required in kaggle for AmebaPro2
1) pip install tensorflow==2.14.1
2) model.save('garbage_cnn.h5', include_optimizer=False)
```

**兩個必要條件,都是不做就會轉檔失敗、而且看不出原因的:**
1. **`tensorflow==2.14.1`** —— 官方上限
2. **`include_optimizer=False`** —— 預設 `model.save()` 會把 Adam 動量等
   optimizer 狀態一起存進 `.h5`,轉檔器讀到會掛

### 轉檔上傳步驟(逐字)
```
1. Download garbage_cnn.h5 from kaggle Output
2. Compress garbage_cnn.h5 to garbage_cnn.zip        <- 要壓成 .zip 才能上傳
3. Go to amebaiot.com/en/amebapro2-ai-convert-model/, fill up your E-mail
4. Upload garbage_cnn.zip
5. Upload one (.jpg) test picture                     <- 量化圖一張就夠
6. Email will be sent to you for the link of `network_binary.nb`
```
收到的檔名是 **`network_binary.nb`**,要自己改名。

### 部署(論壇原案 = SD 卡路線)
```
2. create NN_MDL folder in SDcard, save network_binary.nb under NN_MDL
   folder, and rename it to `imgclassification.nb`
4. modify Sketch: change DEFAULT_IMGCLASS to CUSTOMIZED_IMGCLASS
```
論壇發問者一開始 **core dump**,原因純粹是檔名/目錄錯。
Realtek 的 KaiFai_Y 指出要建 `NN_MDL` 並改名,改完就通。
原話:「after I renamed the file in SDcard to imgclassification.nb it is working fine!」

### ⚠ 檔名絕對不能混
| 路線 | 檔名 | 狀態 |
|---|---|---|
| SD 卡 | `imgclassification.nb` | **已驗證**(論壇成功案例) |
| flash | `img_class_cnn.nb`(放 sketch 資料夾) | 推論,未驗證 |

混用就是那個 core dump。

### ★ Kaggle 而不是 Colab
Kaggle notebook **跑在雲端,不需要管理員權限**,對公司電腦剛好([[avoid-admin-elevation]])。
而且 garbage-cnn 那份**已經內含正確的 TF 版本與存檔參數**,
是已驗證能轉檔成功的版本 -> 用 `Copy & Edit` 複製,只換 dataset 就好,
結構完全不用動。**不要從 mnist_train_colab 重寫,那是重新發明已驗證過的東西。**

### 修正後的計畫
1. Kaggle `Copy & Edit` garbage-cnn
2. 換成自己的 dataset(類別數改掉)
3. 匯出 `.h5`(記得兩個必要條件)-> 壓 zip
4. 線上轉檔器 + 1 張量化圖 -> 等 email
5. 改名 -> flash 走 `img_class_cnn.nb`(SD 卡到了就走已驗證的 SD 路線)
6. sketch 改 Serial 輸出(不要用 RTSP 範例,那要 WiFi + VLC)

## ◆ 完整學習路線圖(目標:訓練 -> 轉檔 -> 燒錄 -> 懂原理)

原則:**每階段先驗證再往下**,這樣任一步失敗時才知道該懷疑哪一段。
最忌諱把「沒驗證的流程」和「沒驗證的模型」擺在一起 -> 出錯時無法定位。

### Stage 0:理解資料流(不用硬體)
```
Camera sensor
  -> planar RGB buffer (CHANNELNN, 例 576x320)
  -> classification_preprocess()
       img_resize_planar()          # 縮到模型輸入尺寸(從 .nb 讀)
       if (color == 0) rgb2gray()   # ★ 陷阱見下
  -> input tensor (uint8)
  -> VIP NPU 跑 .nb
  -> output tensor (__fp16)
  -> classification_postprocess()
       class_cnt = dim->size[0]     # 類別數從模型讀,不寫死
       qsort by prob                # 只是排序,架構無關
  -> ICPostProcess() callback (你的 .ino)
```

**★ 陷阱:範例的 `IMAGERGB` 是 0,而 0 走灰階轉換。**
```c
#define IMAGERGB 0                                  // 範例
if (input_image_color == 0) img_rgb2gray(&img_out); // 0 -> 轉灰階!
```
C 層參數名 `input_image_rgb`,所以 0 = 不是 RGB = 灰階。
**訓練 RGB 模型就要傳非 0,否則板子餵灰階給 RGB 模型,結果全錯且極難查。**

重點觀念:
- `.nb` 是**編譯 + 量化過**的圖,給 VeriSilicon VIP NPU 跑,不像 TFLite 是解譯執行
  -> 所以算子必須在 NPU 支援清單內(HardSwish 就不支援)
- 量化需要**代表性樣本圖**(線上轉檔器讓你上傳最多 10 張)來校準數值範圍
- flash 走 **fwfs**(firmware file system),編譯時只打包 sketch 用到的那一個模型

### Stage 1:驗證燒錄流程(不用 SD 卡、不用訓練)★ 先做這個
拿**原廠** `img_class_cnn.nb` 假裝成自訂模型,證明 flash 自訂流程可行:
1. 複製 `variants/common_nn_models/img_class_cnn.nb` 到 sketch 資料夾
2. `CUSTOMIZED_IMGCLASS` + Tools -> NN Model Load From = flash
3. 編譯 -> 燒錄 -> 應該看到 6 類垃圾分類正常輸出

**為什麼先做:**流程驗證過之後,將來自訂模型出錯就一定是模型的問題。

### Stage 2:訓練(Colab)
- **釘 `tensorflow==2.14.1`**(官方上限;Colab 預設遠比這新)
- 從 `mnist_train_colab` 改:輸入 28x28x1 -> 你要的尺寸;輸出 10 -> N 類
- 灰階最省事(對應 `configInputImageColor(0)`);要 RGB 記得改旗標
- 結構保持樸素 Conv2D/MaxPool/Dense,不要花俏算子
- 存 `model.save('m.h5')` —— 要 `.h5`,不是 `.keras` / SavedModel
- 準備 10 張代表性圖片做量化校準

### Stage 3:轉檔(線上工具)
上傳 `.h5` + 量化圖 -> `.nb` 由 email 寄連結。
失敗時先懷疑:TF 版本、存檔格式、不支援的算子。

### Stage 4:燒入自訂模型
`.nb` 改名 `img_class_cnn.nb` -> 放 sketch 資料夾 -> `CUSTOMIZED_IMGCLASS` -> flash -> 燒。
輸出改成 Serial(不要用 RTSP 範例,那需要 WiFi)。

### 進度
- [ ] Stage 1 流程驗證
- [ ] Stage 2 訓練
- [ ] Stage 3 轉檔
- [ ] Stage 4 燒入自訂模型

## ★★ 修正:自訂模型可以走 flash,不需要 SD 卡(2026-09-09 實證)

**先前 README 寫「所有 CUSTOMIZED_* 只能從 sd:/NN_MDL/ 讀」是錯的。**

### 真正的機制
`NNImageClassification.cpp` 裡 `DEFAULT_IMGCLASS` 與 `CUSTOMIZED_IMGCLASS`
**指向同一個 `&img_classification`**,其取檔名函式回傳:
```c
return (void *)"NN_MDL/img_class.nb";   // flash 檔案系統(fwfs),不是 SD
```
`sd:/NN_MDL/` 那組是另一個 enum(`SD_IMGCLASSIFICATION`),只在 Load From = sd 才走。

**DEFAULT 與 CUSTOMIZED 的差別純粹是編譯期的:**
選 CUSTOMIZED -> prebuild hook `cmodel_backup` 去 **sketch 資料夾**抓你的 `.nb`,
複製進 `variants/common_nn_models/`(原廠檔備份成 `Dbackup_*` / `Cbackup_*`),
再由 `nn_json_modify` 把它加進 FWFS 清單 -> 打包進 flash image。

platform.txt 的三個 prebuild hook(都吃 `{build.model_src}` = flash/sd):
```
prebuild.3  ino_validation_windows.exe    # 解析 .ino 驗證模型選擇
prebuild.4  cmodel_backup_windows.exe     # 自訂模型換入 + 原廠備份
prebuild.5  nn_json_modify_windows.exe    # 改寫 FWFS 打包清單
```

`amebapro2_fwfs_nn_models.json` 的 `"FWFS": {"files": [...]}`
**只會打包 sketch 實際用到的那一個模型**,由 `nn_json_modify` 每次編譯改寫。

### 檔名對應表(來源 `tools/ameba_pro2_tools/1.4.7/misc/nn_models.json`)
```json
"model_mappings": { "CUSTOMIZED_IMGCLASS": "img_class_cnn", ... }
"nb_file_mapping": { "img_class": "img_class_cnn.nb", ... }
```
| CUSTOMIZED 常數 | 你的 .nb 必須叫 |
|---|---|
| `CUSTOMIZED_IMGCLASS` | `img_class_cnn.nb` |
| `CUSTOMIZED_IMGCLASS_MOBILENETV2` | `mobilenetv2_int16.nb` |
| `CUSTOMIZED_YOLOV7TINY` | `yolov7_tiny.nb` |
| `CUSTOMIZED_YAMNET` | `yamnet_fp16.nb` |
| `CUSTOMIZED_SCRFD` | `scrfd_500m_bnkps_640x640_u8.nb` |
| `CUSTOMIZED_MOBILEFACENET` | `mobilefacenet_int16.nb` |
| `CUSTOMIZED_PALMDETECT` | `palm_detection_lite_int16.nb` |
| `CUSTOMIZED_HANDLANDMARK` | `hand_landmark_lite_int16.nb` |

### flash 自訂模型:實際步驟
1. 自訂 `.nb` 改名成 **`img_class_cnn.nb`**(名字錯會噴 `customized model name mismatch`)
2. 放進 **sketch 資料夾**(跟 `.ino` 同層)
3. Tools -> NN Model Load From = **flash**
4. `modelSelect(IMAGE_CLASSIFICATION, ..., CUSTOMIZED_IMGCLASS)`
5. 編譯 -> 燒錄

### 空間估算
原廠 `img_class_cnn.nb` = 6 MB。AudioClassification 打包後 14.1 MB
(其中 yamnet_fp16 佔 8.7 MB)-> app 本體約 5.4 MB。
自己訓練的小 CNN 遠小於 6 MB,16 MB flash 塞得很輕鬆。

### 注意
官方文件(Customized AI Model Installation Guide)**只寫了 SD 卡那條**,
flash 這條沒有文件,是從 platform.txt hook + 工具字串 + nn_models.json 推出來的。
尚未實機驗證,但機制完整且自洽。

## 重要:內建模型可能就夠用(2026-09-09 查證)

### 預設 YOLOv7-tiny = COCO-80,**已包含 apple**
```
{46, "banana", 1},
{47, "apple",  1},
{49, "orange", 1},
```
來源 `libraries/NeuralNetwork/examples/ObjectDetectionCallback/ObjectClassList.h`

**要偵測蘋果 -> 不用轉檔、不用 SD 卡、flash 直接跑。**

### 但內建「分類」模型不是 ImageNet,只有幾類
| 模型 | 類別數 | 內容 |
|---|---|---|
| `img_class_cnn.nb` | 6 | cardboard / glass / metal / paper / plastic / trash |
| `mobilenetv2_int16.nb` | 5 | Daisy / Lavender / Lily / Rose / Sunflower |

來源 `libraries/NeuralNetwork/examples/RTSPImageClassification/ClassificationClassList.h`

注意內建 MobileNetV2 只有 5 類花 -> 它自己就是個 transfer learning 小模型,
**這正是自訂模型該長的形狀,可當範本。**

### 決策分岔
- 「蘋果**在畫面哪裡**」(偵測、要框)-> **內建 YOLO 就有,今天就能跑**
- 「這是**哪一種**蘋果」(細分類)-> 才需要轉檔 + SD 卡

### 官方沒回離線工具申請 -> 還有這些路
卡住的是**repo 授權**,不是環境。按成本排:
1. **去論壇追,別等 email** —— Pammy 是在 forum 回覆的,官方在論壇比信箱活躍。
   直接在 https://forum.amebaiot.com/t/rgb-cnn/3713 公開追申請進度。
2. **線上轉檔器自己實測** —— 官方說不支援 MobileNetV2,但線上版
   **不需授權、不需管理員權限**,零成本,至少能知道它卡在哪個算子。
3. **可能的真正原因**:Rafael Micro 是 IC 設計公司,對 Realtek 算同業,
   這可能才是沒回信的理由。若是,走代理商/FAE 管道或改用學校信箱會比較順。

### 若授權下來,環境不要在公司電腦解
**別裝 Docker Desktop / WSL2(都要管理員)。**
改用 **GitHub Codespaces** 或便宜 VPS —— 手動安裝需 Ubuntu 20.04/22.04,
Codespaces 預設就符合,全程不碰本機提權。

## 轉檔:官方論壇的實戰結論
來源:https://forum.amebaiot.com/t/rgb-cnn/3713 (Realtek staff "Pammy" 回覆)

### 結論 1:AMB82 不支援 HardSwish op
> MobileNetV3 結構的模型**不能**用在 AMB82,請改用 **MobileNetV2**。

發問者換成 MobileNetV2 後轉檔成功。
**這是「轉不過去」最常見的原因:算子不在 VIP NPU 支援清單裡。**

### 結論 2:線上轉檔器功能較弱,要用離線版
> 線上轉檔工具**不支援 MobileNetV2**,請使用**離線**轉檔工具。

2025/09 官方這樣說,2026/03 又有人問,回答**仍然是不支援**。
所以如果用線上轉檔器失敗,可能跟模型無關,是工具支援範圍太窄。

### 離線轉檔器 = Acuity Toolkit(要申請)
https://www.amebaiot.com/en/offline-ai-model-conversion/
- 填申請表,需**公司/學校正式 email**
- 核准後可存取私有 repo `ameba-ai-offline-toolkit`
- 安裝:**Docker(官方推薦)** 或手動(**僅** Ubuntu 20.04 / 22.04)

**⚠ 公司電腦的問題:Docker Desktop 和 WSL2 安裝都需要管理員權限。**
這台沒有管理員權限([[avoid-admin-elevation]]),所以環境可能先卡住。
申請前先確認能不能裝 Docker,否則拿到工具也跑不動。

### 自訂 MobileNetV2 的落點
```c
static void *classification_mobilenetv2_get_SD_filename(void) {
    return (void *)"sd:/NN_MDL/mobilenetv2_int16.nb";
}
```
檔名寫死成 `mobilenetv2_int16.nb`(注意 `int16` = 量化格式線索),
搭配 sketch 裡的 `CUSTOMIZED_IMGCLASS_MOBILENETV2`。

### 所以「蘋果」該怎麼做
- 想知道**「這是不是蘋果」**(分類)-> **MobileNetV2 影像分類**,這是有人做成功的路
  (論壇第 3 樓有人分享 Kaggle 垃圾分類 CNN 範例)
- 想知道**「蘋果在畫面哪裡」**(偵測、要框)-> 重訓 YOLOv7-tiny,用 SDK 附的 reparam 腳本
- **FOMO 不行** -> 沒有對應的 postprocess

### 參考文件
- 轉檔總覽 https://www.amebaiot.com/en/amebapro2-ai-convert-model/
- 離線轉檔 https://www.amebaiot.com/en/offline-ai-model-conversion/
- 線上轉檔 https://ameba-doc-arduino-sdk.readthedocs-hosted.com/en/latest/ameba_pro2/amb82-mini/Other_Guides/AI_Related_Guides/Online%20AI%20Model%20Conversion%20Guide.html
- 自訂模型安裝 https://ameba-doc-arduino-sdk.readthedocs-hosted.com/en/latest/ameba_pro2/amb82-mini/Other_Guides/AI_Related_Guides/Customized%20AI%20Model%20Installation%20Guide.html
- 影像分類範例 https://ameba-doc-arduino-sdk.readthedocs-hosted.com/en/latest/ameba_pro2/amb82-mini/Example_Guides/Neural%20Network/Image%20Classification.html

## 坑：IDE 和 arduino-cli 不能同時編譯
兩者**預設共用同一個建構快取**：
```
%LOCALAPPDATA%\arduino\sketches\<sketch 路徑的 hash>
```
同時編譯會互相踩死，錯誤訊息長這樣（同一件事的兩面）：
```
cannot open ...\core\avr\dtostrf.c.su: No such file or directory     ← 目錄被對方清掉
unlinkat ...\core\ard_socket.c.su: being used by another process      ← 檔案被對方佔住
```
**跟程式碼、工具鏈都無關**，單獨編譯是會過的。

處理方式：
1. 一次只有一邊在編譯。平常就讓 IDE 用預設快取。
2. 指令列編譯時加 `--build-path` 指到別的地方，就永遠不會撞：
   ```powershell
   & $cli --config-file $cfg compile --fqbn $fqbn --build-path $env:TEMP\ameba-build D:\workdir\ameba\Blink
   ```
3. 快取卡在半殘狀態時直接整個刪掉，它會自己重建（純快取，刪了只是下次編久一點）。

## 已確認
- 板子型號：**AMB82-MINI**（RTL8735B）
- **序列埠：CH340（`VID_1A86&PID_7523`）** ← 拔插測試確認
  - **COM 編號會變！** 出現過 COM20、COM51。
    CH340 沒燒獨立序號，Windows 依 root hub 埠位配號，換 USB 孔就會換號。
    每次上傳前先確認：
    ```powershell
    Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'CH340' } | Select-Object Name,DeviceID
    ```
  - COM6 是 FTDI FT230X（序號 DK0BN1X3A），**不是板子**，直連主機板
  - COM68~71 是 FT4232 四通道，另一台工具，無關
  - COM1 是 ACPI 內建

### 判斷埠位的方法
`arduino-cli board list` 對 AmebaPro2 一律顯示「未知的」——認不出板子，
所以 **port 必須手動指定**。要確認是哪個埠，就做拔插測試：
拔線 → 掃一次 → 插回 → 再掃一次，消失又回來的那個就是。

## 環境現況（2026-09-08）
- Arduino IDE 2.3.10：**已裝**
- 內建 arduino-cli 1.5.1：**可用**
- `realtek:AmebaPro2` 4.1.0 core：**已裝**
  - 附帶 `ameba_pro2_toolchain` 1.0.1-p1 / `toolchain2` 1.0.1-p2
  - 附帶 `ameba_pro2_tools` 1.4.7（含燒錄工具）
  - 附帶 `ameba_pro2_nn_models` 1.0.3 ← **AI 範例的模型已經在本機了**
- **FQBN：`realtek:AmebaPro2:Ameba_AMB82-MINI`**
- Blink 編譯：**通過**（4788224 bytes / 16MB，28%）

### 指令列編譯 / 上傳
```powershell
$ide = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\ArduinoSA.IDE.stable_Microsoft.Winget.Source_8wekyb3d8bbwe"
$cli = "$ide\resources\app\lib\backend\resources\arduino-cli.exe"
$cfg = "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml"
$fqbn = "realtek:AmebaPro2:Ameba_AMB82-MINI"
& $cli --config-file $cfg compile --fqbn $fqbn D:\workdir\ameba\Blink
& $cli --config-file $cfg upload  --fqbn $fqbn -p COM6 D:\workdir\ameba\Blink
```
upload 前務必先做 BOOT/RESET 那套動作（見上面「燒錄流程」）。

注意：`arduino-cli board list` 對 AmebaPro2 一律顯示「未知的」，
它認不出板子（沒有 USB VID/PID 對應），所以 **port 必須手動指定**，
不能靠自動偵測。

## 待確認
- 第一個 AI 範例方向：物件偵測 / 人臉識別 / 音訊分類

## ★★ 轉檔實驗紀錄(來自 mnist_train_colab.ipynb 自己的筆記)

這台機器上 `mnist_train_colab.ipynb` 已經試過三條線上轉檔路線,兩條有明確的
Realtek 端 bug。也就是說先前轉檔失敗**不是使用者的操作問題**。

| 路線 | 模型 | 結果 |
|---|---|---|
| CNN-GRAY | 小 CNN 28x28 | `inputmeta.yml` 的 `preproc_type` 是壞的模板值 -> YAML parse 失敗。已回報 Realtek |
| CNN-RGB | 小 CNN | `cannot reshape array of size 1 into shape (1,1280)` |
| CNN-RGB | Keras MobileNetV2 (96x96x3, weights=None, pooling="avg") | 同一個 `(1,1280)` 錯 |
| CNN-RGB | PyTorch MobileNetV2 -> ONNX opset 11 | **notebook cell 22/23 的 output 是空的,還沒跑過** |

> **已被 log 推翻**：見本檔「轉檔失敗的真正原因」一節。下面這段推訊是錯的。

`(1,1280)` 在兩種完全不同拓樸上都一樣 -> 那個 reshape 屬於 Realtek 寫死的模板,
推測是從某個 PyTorch 匯出的 ONNX 圖推導出來的。官方論壇
<https://forum.amebaiot.com/t/mobilenetv2-offline-conversion/4849>
Realtek 人員說只支援 **PyTorch 訓練的** MobileNetV2。

### 線上轉檔器的上傳設定(notebook 已驗證過的填法)

- Model: `CNN-RGB`(不要選 CNN-GRAY,壞的)
- Quantize Type: `UINT8`
- reverse_channel: `false`
- scale: `0.00392156` (= 1/255,要和訓練時的 pixel/255.0 一致;
  這個工具**只吃純 scale**,不支援 mean/offset 相減,所以訓練正規化不能用 [-1,1])
- 上傳的是 `.zip`,裡面包 `.h5`(或 `.onnx`);校準圖另外一個 `.zip`,內含 jpg

### 輸入尺寸硬限制(這條決定了跑分模型的形狀)

Realtek:「Pro2 RGB w/h value from maximum to minimum is 1280x704 to 96x96.
The value is recommended to be the multiples of 32.」

-> **28x28 的 MNIST 模型沒有任何合法的上傳形狀**:RGB 路線最小 96x96,GRAY 路線壞掉。
跑分模型因此必須重訓成 96x96x3 輸入。見 notebook 第 10 節。

## ◆ 跑分模型的定義(notebook 第 10 節,已加好)

96x96x3 輸入的小 CNN,第一層 `Conv2D(8, 3, strides=2)` 先砍半解析度,
最大中間張量 47x47x8 = 17.6 KB,Pico 2 (RP2350, 520 KB SRAM) 吃得下。
刻意**不含任何 augmentation / Rescaling 層** —— 那些層會被烘進存出的圖裡,轉檔器不認得
(原本 cell 8 的 `build_model()` 把 `RandomRotation`/`RandomTranslation` 放進 Sequential
裡面,TFLite 推論時是 no-op 所以沒事,但送去線上轉檔器就是多餘的 op)。

同一組權重兩邊各自匯出:
- AMB82:`.h5` -> zip -> 線上轉 `.nb` -> 改名 `img_class_cnn.nb` -> `CUSTOMIZED_IMGCLASS` + flash
- Pico 2:int8 `.tflite` -> `mnist_bench96_model_data.{h,cc}`

### 量測時的兩個地雷

1. `configInputImageColor(0)` 會呼叫 `img_rgb2gray()` —— 儘管 example 裡那個巨集叫
   `IMAGERGB`。跑分模型吃 RGB,必須傳 `1`。
2. `VideoSetting configNN(NNWIDTH, NNHEIGHT, 10, VIDEO_RGB, 0)` 的 `10` 是 **10 fps 上限**。
   不調高的話量到的是節流後的數字,不是推論速度。要往上加到飽和(fps 不再上升)為止。
   另外 `libarduino.a` 裡那個現成的 `NN Inference: (min/max/avg ...)` 計時器住在
   `ainr.c.obj`(nnlite 路徑),Arduino NN 這條路走不到,不要指望它會印出來。

## 環境:Colab vs Kaggle(實測 2026-09)

文件說線上轉檔器支援到 TF 2.14.1,但 2.14 只有 cp39-cp311 的 wheel:

| 平台 | Python(實測) | 能裝的最低 TF |
|---|---|---|
| Colab | 3.13 | 2.19 |
| Kaggle | 3.12.13 | 2.16 |

**兩邊都裝不了 TF 2.14.1。** 換去 Kaggle 對版本號沒有幫助。

真正會咬人的不是版本號,是 **Keras 3**:TF >= 2.16 預設 Keras 3,
`model.save("x.h5")` 的 HDF5 內部 schema(layer config、model_config JSON)
跟 Keras 2 不同。Realtek 轉檔器是 Keras 2 時代的東西,很可能讀不懂。

**解法(不需降 Python)**:`pip install tf-keras`,並在 `import tensorflow` **之前**
設 `os.environ["TF_USE_LEGACY_KERAS"] = "1"`,`tf.keras` 就會轉回 Keras 2 實作。
notebook 第 0 節的 `USE_LEGACY_KERAS` 開關就是做這件事;
因為 env var 要在 import 前生效,裝完**必須 restart session 再 Run All**。
第 1 格會印 `tf.keras.__version__`,印出 2.x 才算成功。

Kaggle 其他注意事項:

- 沒有 `google.colab.files.download`;第 0 節的 `save_output()` 兩邊通用
- `Accelerator` 選項找不到 = 手機沒驗證(頭像 -> Settings -> Phone verification)。
  跑分模型很小,純 CPU 8 epoch 約 10-20 分鐘,可以不理
- `Internet` 必須手動打開,否則 pip install 直接失敗

## ★★★ 轉檔失敗的真正原因(2026-09-09,有 log 實證)

證據來源:線上轉檔器失敗時提供的 log 包,存在 `C:\Users\M90t\Desktop\cnn-rgb\`
(`information.txt` / `cnn-rgb_import.txt` / `cnn-rgb_quantize.txt` / `cnn-rgb_export.txt`)。
該次上傳的是 **Keras MobileNetV2 的 `.h5`**(`mnist_amb82_mbv2.zip`)。

### 發現 1:後端會先把 h5 轉成 ONNX,再餵 acuity

`cnn-rgb_import.txt` 開頭:

```
=========== Converting cnn_rgb ONNX model ===========
pegasus.py import onnx --model cnn_rgb.onnx --output-model cnn_rgb.json ...
I Current ONNX Model use ir_version 7 opset_version 18
```

上傳的是 `.h5`,但 import 階段跑的是 **onnx** importer、opset **18**(tf2onnx 預設)。
=> h5 路線實際上是 `h5 -> tf2onnx(opset 18) -> acuity import onnx`,
中間多一層使用者控制不到的轉換。**直接上傳 `.onnx` 可以繞掉這層。**

### 發現 2:`(1,1280)` 是使用者自己模型裡的 reshape,不是 Realtek 的模板

```
shape_inference.py line 65, in infer_shape
smart_graph_engine.py line 70, in smart_onnx_scanner
smart_node.py line 48, in calc_and_assign_smart_info
smart_toolkit.py line 1018, in reshape_shape
    reshaped = np.reshape(np.ones(in_shape.shape), new_shape)
ValueError: cannot reshape array of size 1 into shape (1,1280)
[31m import model ERROR ! [0m
```

死在 **shape inference**,不是死在載入模板。`1280` 就是 MobileNetV2 的 feature 維度
(Keras 版 `pooling="avg"`;PyTorch 版 `classifier[1] = nn.Linear(1280, 10)`)。

真正的病灶:**global average pooling 後面那個 flatten/reshape,
它的目標 shape 在 ONNX 圖裡是動態算出來的**(`Shape -> Gather -> Concat -> Reshape`)。
acuity 5.21.1 的 shape inference 折不出那個常數,拿到 size=1 的東西去 reshape 成 (1,1280) 就爆。

=> **舊筆記(notebook 第 6 節)說「reshape 屬於 Realtek 寫死的模板」是錯的**,
本檔先前的敘述也跟著錯。已更正。

### 連鎖失敗(不要被後面兩個 log 誤導)

- `cnn-rgb_quantize.txt`:`FileNotFoundError: 'cnn_rgb.json'` — import 沒產出 json,所以量化沒東西可讀
- `cnn-rgb_export.txt`:`Can not find cnn_rgb_uint8.quantize` — 同上,連鎖

**唯一要看的是 `_import.txt`。**

### 依此診斷修正候選優先順序

| 候選 | 為什麼可能過 |
|---|---|
| 1. PyTorch ONNX opset 11(legacy exporter) | 直接餵 ONNX,繞掉 tf2onnx;opset 11 下 `torch.flatten(x,1)` 通常出靜態 `Flatten` op |
| 2. 第 10 節小 CNN 的 h5 | 沒有 global average pooling、沒有 1280 向量,`Reshape((4*4*32,))` 是固定尺寸,tf2onnx 應折成常數 |
| 3. 上面任一 + `onnx-simplifier` | 把 `Shape/Gather/Concat` 子圖常數折疊,直接消滅動態 Reshape |

**先前寫「小 CNN 勝算低」要反過來** —— 它沒有那個會產生動態 shape 的
pooling->flatten 結構,反而比 MobileNetV2 更容易過。

## torch.onnx.export 的 dynamo 陷阱(已修)

torch >= 2.9 的 `torch.onnx.export()` 預設 `dynamo=True`,會要求 `onnxscript`:

```
ModuleNotFoundError: No module named 'onnxscript'
```

**不要去 `pip install onnxscript`** —— 那會走新的 TorchDynamo 匯出器,
產出的圖結構跟 acuity 5.21.1 預期的不一樣。正解是明確傳 `dynamo=False`
走回舊的 TorchScript 匯出器(`opset_version=11` 這組參數本來就是給它寫的)。
notebook 第 9 節已改成 `dynamo=False` + `TypeError` fallback,實測在 Kaggle 匯出成功。

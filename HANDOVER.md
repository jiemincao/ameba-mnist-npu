# 交接文件 —— 換機器怎麼接上進度

最後更新：2026-09-10

這份文件的用途：**在另一台電腦（公司 Windows / 家裡 Mac）從零把環境接起來，繼續做下去。**
研究過程與踩過的坑寫在 [README.md](README.md)（研究日誌，不是操作手冊）。
這裡只講「怎麼跑起來」和「現在做到哪」。

---

## 一、這個專案在做什麼

**目標：同一個 MNIST（0~9 手寫數字）模型，在兩塊板子上比推論速度。**

| | AMB82-MINI | Raspberry Pi Pico 2 |
|---|---|---|
| 晶片 | RTL8735B | RP2350 |
| 算力來源 | VeriSilicon **VIP NPU**（`VIP8000NANONI_PID0XAD`） | Cortex-M33 純 CPU（CMSIS-NN） |
| 模型格式 | `.nb`（NBG, network binary graph） | int8 TFLite → C array |
| 狀態 | ✅ **結案**：`total_cycle ≈ 60,900` = 121.8 µs | ✅ **結案**：CMSIS-NN 40,253 µs |

**這不是分類器專案**，是跑分專案。所以兩邊必須跑**同一個模型**，
即使那讓 Pico 2 多做 3 倍的第一層運算（見下面「為什麼是 3 通道」）。

同時也是學習專案：目標是走完 **訓練 → 轉檔 → 燒錄 → 懂原理** 全流程，
不是拿到數字就算了。跳步驟會失去意義。

---

## 二、現在做到哪

### 已完成

**模型**（`model/`）
- 架構：`Conv2D(16,3,same) → MaxPool2 → Conv2D(32,3,same) → MaxPool2 → Reshape(1568) → Dense(64) → Dense(10) → Softmax`
- 106,154 參數
- float 測試準確率 **98.77%**
- 轉檔全程 `Error(0), Warning(0)`
- `mnist_cnn.nb` = 123,912 bytes

**數值驗證**（這一步之前做 MobileNetV2 時跳過，結果吃了大虧，見「坑 3」）

| 項目 | 結果 |
|---|---|
| float top-1 | 199/200 (99.5%) |
| uint8 top-1 | 199/200 (99.5%)，錯的是同一張（3 判成 5） |
| float vs uint8 預測一致 | 200/200 |
| 機率最大絕對誤差 | 0.0344 |

**裝置端介面對照**（用 Verisilicon NBInfo 比對內建模型）

| | 內建 `img_class_cnn.nb` | 我們的 `mnist_cnn.nb` |
|---|---|---|
| 輸入 dim | 224, 224, 3, 1 | 28, 28, 3, 1 |
| 格式 | UINT8 / QUANTIZE_NONE / TF Scale 1.0 | **完全相同** |
| 輸出 | 6 類 FP16 | 10 類 FP16 |
| 大小 | 6,002,496 B | 123,912 B |

**換模型已生效**（用檔案大小驗證）
- `nn_model.bin`：6,008,832 → **131,072**
- sketch image：11,448,320 (68%) → **5,570,560 (33%)**

### 兩邊的數字（詳細分析在 README「跑分結果」一節）

| | 牆鐘 µs | cycles | cycles/MAC |
|---|---|---|---|
| Pico 2 float32 naive | 327,653 | 49,148,544 | 36.6 |
| Pico 2 int8 naive | 114,934 | 17,240,686 | 12.8 |
| Pico 2 int8 + CMSIS-NN | 40,253 | 6,038,527 | 4.50 |
| AMB82 NPU | 2,995 ⚠ | 60,900 | 0.045 |

⚠ AMB82 的牆鐘約 96% 是在等 FreeRTOS 1 ms tick（`vpmdENABLE_POLLING=0`，
`libnn.a` 預編改不了），**不是 NPU 的延遲**。cycle 數才是可信的硬體數字。
NPU 真正的計算時間是 60,900 / 500 MHz = **121.8 µs**（時鐘域已實測確認，見下）。

### ★ 最終結論：「快幾倍」必須分四層講

引用單一個倍數一定會被誤用，所以四個數字要一起給：

| 層次 | 數字 | 意思 |
|---|---|---|
| 端到端（單張推論） | **13.4x** | 今天真的拿得到的。其中 96% 時間在等 RTOS 排程 |
| 牆鐘（純計算） | **~330x** | 40,253 µs vs 121.8 µs。含 3.33x 時鐘優勢 |
| 時鐘歸一化（純架構） | **99.2x** | ~330x ÷ 3.33x。這才是「NPU 本身強多少」 |
| NPU 利用率 | **5.5%** | 22 MAC/cycle vs 理論 400 MAC/cycle（0.4 TOPS @ 500 MHz） |

最後一列決定了前面幾個數字能不能外推：**99.2x 是在 NPU 嚴重吃不飽的情況下拿到的**。
MNIST 對這顆 NPU 太小（conv1 的 `C_IN = 3`，MAC array 幾乎整片閒著；只有 8 層、
1.34 M MAC，層間 setup 攤不掉；權重在 DDR）。所以換模型時倍數兩邊都會跑：
通道寬、層數深的模型可以超過 100x；有 NPU 不支援的算子則會 fallback 回 CPU，
可能掉到個位數甚至倒退。

做系統設計時 **13.4x 比 330x 重要** —— NPU 快到某個程度之後瓶頸就換到驅動和 OS。

### 還沒做

1. **Pico 2 權重搬進 SRAM**：4.50 cycles/MAC 對 CMSIS-NN 偏慢，
   懷疑瓶頸是 112 KB 權重放在 QSPI flash、XIP cache 只有 8 KB。
   目前只用 139 KB / 520 KB SRAM，搬得進去。
2. **AMB82 上跑同一份 CMSIS-NN**（它的 CPU 也是 Cortex-M33）。
   這樣才有「同一顆晶片、CPU vs NPU」這個唯一誠實的 NPU 加速比。
   先查 Realtek 的 core 編 KM4 時有沒有定義 `__ARM_FEATURE_DSP`，
   沒開的話 CMSIS-NN 會走純 C 路徑，比較就不成立。
3. **還原 `_backup_amb82/`** 的原廠 `.nb`（跑分做完了，這件事現在就可以做）。
4. **SD 卡已到貨**（2026-09-10），跑分不需要它，但之後玩別的可以用：
   `ObjectDetectionImage` 解鎖、多個 `.nb` 不必全塞 flash、換模型不用重燒整包 firmware。
   ⚠ **WiFi 沒被解開**（公司 802.1X，`WiFi.begin()` 只吃 WPA2-PSK），
   所以靠 RTSP 輸出的那幾個範例還是要用手機熱點。詳見 README「SD 卡到貨後解鎖了什麼」。

### 未解決 / 已知限制

- **牆鐘量不到 121.8 µs，這是驅動的限制不是我們的**：`vpmdENABLE_POLLING = 0`，
  `vip_run_network()` 阻塞在 OS wait 上，`configTICK_RATE_HZ = 1000`，
  所以每次呼叫至少吃 ~3 個 tick（實測固定 2,995 µs）。`libnn.a` 是預編的，
  改不了這個選項。**單張推論的端到端時間就是 ~3 ms，這是真實的部署限制**，
  只有 batch 很多張才能把它攤掉。
- `hw_avg_us` 印 1 是假的（那個數字的來源是 OS tick，解析度不夠），**看 `total_cycle`**。
- 「250 MHz」那行 log 只是回顯 `#define`，**不是暫存器回讀**。時鐘域的結論靠的是
  cycle 數的變化（−2.8%），不是那行字。要決定性證明就讀回
  `SYSON_S_REG_SYS_NN_CTRL`（offset **0x11C**，欄位 `SYSON_S_MASK_SYS_NN_SRC_SEL` = `0x3 << 3`，
  定義在 `.../fwlib/rtl8735b/lib/include/rtl8735b_syson_s_type.h:941-950`）。
- 歷史註記（已不適用於現在的 sketch）：最早那版是掛在相機回呼上量「回呼到回呼」，
  量到的是整條管線的 throughput 而不是 NPU 延遲，而且會被相機 30 fps 夾在 33.3 ms。
  現在的 `MnistNpuBench` 是直接呼叫 VIPLite，沒有相機、沒有 VOE，所以不受這個影響。
- VOE / 相機路徑起不來的問題**沒有解決，是設計上繞開的**。跑分不需要相機。

---

## 三、環境怎麼建起來

### 需要的東西

| 元件 | 版本 | 怎麼拿 |
|---|---|---|
| Docker（跑 acuity 容器） | 任何近期版本 | Win 公司機：**WSL2 裡的 Docker Engine**（不是 Docker Desktop，會要提權）<br>Mac：Docker Desktop 直接裝 |
| acuity 容器 image | `ghcr.io/ameba-aiot/acuity-toolkit:6.18.8` | `docker pull`，需要 GitHub PAT（`read:packages`） |
| acuity 範例包 | `acuity_examples_c901149.tgz`（318 MB） | **不在 repo 裡**，要用自己的 Realtek 邀請重新下載 |
| Arduino AmebaPro2 board package | 4.1.0 | Arduino IDE 2.x 板子管理員 |
| Verisilicon NBInfo | 1.2.17 | **不在 repo 裡**（與 acuity 包同屬原廠授權物）。`Verisilicon_SW_NBInfo_1.2.17_20230412.tgz`，33 KB，隨離線工具包一起取得 |

> **為什麼範例包不放 repo**：那是憑 Realtek 邀請才拿得到的，授權範圍不明，不散佈。
> 這也是為什麼這個 repo 必須是 **private**（`realtek-email-B-bugreport.md` 也有原廠工具的缺陷細節）。

### 目錄長怎樣

環境是這樣分的 —— **repo 在一邊，工具包在另一邊**：

```
D:\workdir\ameba\              (Windows，= 這個 git repo)
├── MnistNpuBench/             AMB82 跑分 sketch
├── pico2/MnistPicoBench/      Pico 2 跑分專案（CMake，跟 AMB82 完全獨立）
├── model/                     h5 / nb / tflite / inputmeta（含 .orig 對照）
├── scripts/                   容器內用的腳本（從 WSL 複製過來的）
├── _backup_amb82/             ★ 板子原廠檔備份，還原用，不在 git 裡
├── acuity_examples_c901149.tgz  ★ 授權工具包，不在 git 裡
└── README.md / HANDOVER.md

~/ameba-toolkit/               (WSL 或 Mac 家目錄，工具包解開的地方)
├── acuity_examples_c901149/
│   └── Models/mnist_cnn/      ← 轉檔實際發生的地方
├── dk.sh                      ← 從 scripts/ 複製進來
├── train_mnist.py
├── quant_mnist.sh
├── verify_mnist.sh
└── score_mnist.py

（Pico 2 那邊的相依不在上面兩處，是這兩個地方）
~/.pico-sdk/                   VSCode Pico 擴充自己下載的 sdk / toolchain / picotool / cmake / ninja
D:\workdir\pico-deps\CMSIS-NN  CMSIS-NN 7.0.0 原始碼（自己 clone 的）
```

⚠ **`scripts/` 不會自動同步。** 在容器裡改了腳本，記得複製回 `scripts/` 再 commit，
不然下次換機器就漏了。（反向也一樣：`git pull` 之後要複製進 `~/ameba-toolkit/`。）

### 建置步驟

```bash
# 1. 抓 repo
git clone <your-repo-url> && cd ameba          # Mac
                                                # Windows 公司機已經在 D:\workdir\ameba

# 2. Docker 起來
#    Windows：WSL2 沒有 systemd，systemctl 沒用，要手動跑 dockerd。
#    daemon 會跨 wsl session 存活，只有 wsl --shutdown / 重開機才要再跑一次。
wsl -d Ubuntu -e sh /mnt/d/workdir/ameba/wsl-docker-up.sh
#    Mac：開 Docker Desktop 就好

# 3. 抓 image（要 GitHub PAT 當密碼）
docker login ghcr.io
docker pull ghcr.io/ameba-aiot/acuity-toolkit:6.18.8

# 4. 攤開工具包 + 放好腳本
mkdir -p ~/ameba-toolkit && cd ~/ameba-toolkit
tar xzf /path/to/acuity_examples_c901149.tgz
cp /path/to/repo/scripts/*.sh /path/to/repo/scripts/*.py .
chmod +x *.sh
```

### 完整轉檔流程（照這個順序）

```bash
cd ~/ameba-toolkit

# ① 訓練（在容器裡，CPU，6 epoch < 1 分鐘）
#    順便產生 200 張校正圖（每類 20 張）與 dataset.txt
sh dk.sh 'python3 /workspace/train_mnist.py'

# ② import：h5 → acuity 中間表示
sh dk.sh 'cd acuity_examples_c901149/Models/mnist_cnn && \
          bash $ACUITY_PATH/../pegasus_import.sh .'

# ③ ★ 改 inputmeta —— 不改的話後面全錯，見「坑 1」
#    改兩個地方：scale 1.0 → 0.00392157（= 1/255，對應訓練時的 x/255）
#              add_preproc_node false → true
#              preproc_type IMAGE_RGB → IMAGE_RGB888_PLANAR
#    改好的版本在 repo 的 model/mnist_cnn_inputmeta.yml，直接覆蓋過去最快。
#    ⚠ 這個檔案「不准有 TAB」（檔頭自己寫的），只能用空白縮排。

# ④ quantize（用自己寫的，不要用官方的，見「坑 2」）
sh dk.sh 'bash /workspace/quant_mnist.sh'

# ⑤ 驗證數值（同樣不要用官方的）
sh dk.sh 'bash /workspace/verify_mnist.sh'
sh dk.sh 'python3 /workspace/score_mnist.py'
#    要看到 float / uint8 都 ~99.5%、一致率 200/200 才算過

# ⑥ export → .nb
sh dk.sh 'cd acuity_examples_c901149/Models/mnist_cnn && \
          bash $ACUITY_PATH/../pegasus_export_ovx.sh .'

# ⑦ ★ 檢查介面對不對 —— 這是唯一能事先抓到「轉得出來但算出垃圾」的方法
#    選項不能合併！-b -in -out 寫在一起只會印第一張表（見「坑 4」）
./Verisilicon_SW_NBInfo_1.2.17_20230412/nbinfo -in network_binary.nb
./Verisilicon_SW_NBInfo_1.2.17_20230412/nbinfo -out network_binary.nb
#    輸入要看到 28, 28, 3, 1 / UINT8 / QUANTIZE_NONE / scale 1.0
#    輸出要看到 10 個 FP16
```

### 燒到板子

```
# 1. 把 .nb 蓋過去（兩個位置都蓋，實際打包來源是 tools 那份）
packages/realtek/tools/ameba_pro2_tools/1.4.7/img_class_cnn.nb          ← ★ 真正的打包來源
hardware/AmebaPro2/4.1.0/variants/common_nn_models/img_class_cnn.nb     ← 順手也蓋

# 2. Arduino IDE 2.x 開 MnistNpuBench/，板子選 realtek:AmebaPro2:Ameba_AMB82-MINI，port COM6
# 3. 編譯 → 燒錄
# 4. 驗證換成功：看 nn_model.bin 大小變了（內建 6,008,832 → 我們的 131,072）
```

**還原原廠模型**：把 `_backup_amb82/tools_1.4.7/` 和 `_backup_amb82/common_nn_models/` 蓋回去。
跑分做完記得還原。

⚠ **不要在 Arduino IDE 可能正在編譯時，同時用 arduino-cli 編譯** ——
會撞同一個 build 目錄。用 cli 時一定加 `--build-path` 指到別的地方。

### Pico 2 那邊怎麼建、怎麼燒

跟 AMB82 完全獨立，不需要 Docker、不需要 acuity。需要三樣東西：

| 元件 | 版本 | 位置（這台機器上） |
|---|---|---|
| pico-sdk | 2.1.1 | `C:/Users/M90t/.pico-sdk/sdk/2.1.1` |
| arm-none-eabi-gcc | 15_2_Rel1 | `~/.pico-sdk/toolchain/` |
| CMSIS-NN | 7.0.0 | `D:/workdir/pico-deps/CMSIS-NN` |
| picotool | 2.3.1 | `~/.pico-sdk/picotool/` |

（`~/.pico-sdk/` 是 VSCode Pico 擴充自己下載的，它**確實**帶 SDK payload。
所以裝了擴充就有這些，不必另外裝 toolchain。）

**指令列建置**：

```bash
cd pico2/MnistPicoBench
cmake -S . -B build -G Ninja \
      -DPICO_SDK_PATH=C:/Users/M90t/.pico-sdk/sdk/2.1.1 \
      -DCMSIS_NN_PATH=D:/workdir/pico-deps/CMSIS-NN
cmake --build build
# 產出 build/mnist_pico_bench.uf2
```

**燒錄**：按住 BOOTSEL 插 USB → 出現 `RPI-RP2` 磁碟 → 把 `.uf2` 拖進去。
（或 `picotool load build/mnist_pico_bench.uf2 -fx`，就是 tasks.json 的 Run Project。）
接著開序列埠（USB CDC 或 UART0 GP0/GP1 都會印同樣內容），
程式會等最多 3 秒讓你連上，然後自己跑完三輪印 `==== 跑完了 ====` 就停。

⚠ **在 VSCode 裡不要按「Import Project」。** 那個功能在 Windows 上是壞的，
而且它實際跑的是 `pico_project.py --convert`（官方 help 自己寫 "risks data loss"），
會就地改寫我們的 `CMakeLists.txt`（裡面有 CMSIS-NN 接線和三個模式的旗標）。
擴充要的東西已經手動補好了（`CMakeLists.txt` 裡那段 DO NOT EDIT header + `.vscode/`），
**直接 `Open Folder` 開 `pico2/MnistPicoBench/` 本身**，Compile / Run 按鈕就會出現。
根本原因與證據鏈寫在 README「Pico 2 專案怎麼建的」一節。

⚠ **跑分數字必須來自同一套 toolchain。** 兩套實測差 0.5%
（GCC 10.3.1：587,188 text / 138,988 bss；GCC 15.2.1：590,108 / 138,984），
不大但足以污染小幅度的比較。目前的數字是 15_2_Rel1 這套。

---

## 四、五個坑（每一個都真的踩過）

### 坑 1：inputmeta 不開 `add_preproc_node`，模型轉得出來但算出垃圾

不開的話匯出的 `.nb` 輸入是 **interleaved NHWC**，NBInfo 會看到 `Dim[0]=3`。
而裝置端 `model_classification.c` 寫的是：

```c
img_out.width  = tensor_param->dim[0].size[0];   // ← width 會變成 3
img_out.height = tensor_param->dim[0].size[1];
img_resize_planar(&img_in, roi, &img_out);       // ← 而且它產生的是 planar，不是 interleaved
```

→ 幾何全錯。開了之後 dim 變成 `[28, 28, 3, 1]`、`UINT8 / QUANTIZE_NONE / scale 1.0`，
跟內建模型一致，而且 mean/scale 正規化被搬進圖裡**由 NPU 做**（等於免費）。
配方在 `acuity_examples/Example/example-3` 的 ReadMe 第 3 條。

### 坑 2：官方 `pegasus_quantize.sh` 和 `pegasus_inference.sh` 都寫死只跑 1 個 iteration

兩支腳本都沒傳 `--iterations`，而預設值是 **1**。
所以 `dataset.txt` 裡的 200 張校正圖，**只有第 1 張會被用到**。
症狀是 log 印 `Running 1 iterations` / `0(100.00%)`。

量化只用 1 張圖校正 → 動態範圍估錯 → 模型精度崩掉。
**這極可能就是之前 MobileNetV2 `Top5: 0.000000` 的真正原因，模型本身沒壞。**

修法：用 repo 裡的 `scripts/quant_mnist.sh` 和 `scripts/verify_mnist.sh`，
它們補上 `--iterations 200 --batch-size 1 --algorithm moving_average`。

### 坑 3：一定要驗證數值，不要看到 `Error(0)` 就以為成功

`Error(0), Warning(0)` 只代表**轉檔工具沒抱怨**，不代表模型還會算。
坑 2 那種問題完全不會產生任何 error。
所以流程裡的 ⑤ 不能跳 —— 要實際跑 float 和 uint8 推論、對答案、比一致率。

### 坑 4：NBInfo 的選項不能合併

`nbinfo -b -in -out x.nb` 只會印第一張表，**而且不報錯**。
要分開跑：`-b`、`-in`、`-out` 各一次。

### 坑 5：`wsl -e bash -lc '...'` 裡面放 heredoc 會死在引號上

症狀：`unexpected EOF while looking for matching '`。
正確做法：檔案寫在 Windows 這邊，再 `cp /mnt/d/... ~/ameba-toolkit/`。
`dk.sh` 就是為了這個而存在 —— 讓容器指令不必再套一層引號。

---

## 五、為什麼是 3 通道（不是 1 通道灰階）

因為 `img_t` 這個結構**沒有 channel 欄位**
（`system/project/realtek_amebapro2_v0_example/src/test_model/img_process/img_process.h`）：

```c
typedef struct { int width; int height; uint8_t *data; } img_t;
int img_resize_planar(img_t *im_in, rect_t *roi, img_t *im_out);
```

所以 `img_resize_planar()` **永遠寫 W×H×3 bytes**。
模型若宣告 1 通道，tensor buffer 只有 W×H，會**溢出 2×W×H**。

裝置端所謂的「灰階」是：3 通道 buffer，`configInputImageColor(0)` 事後把 plane 0 覆寫成灰階
（plane 1、2 仍是原本的 G、B）。所以自訓模型要做 3 通道，sketch 裡設 `IMAGERGB 1`。

訓練時的處理：`np.repeat(x[..., None], 3, axis=-1)`，R=G=B。

**代價**：Pico 2 那邊第一層要多做 3 倍運算。
**為什麼還是接受**：跑分的前提是兩邊跑同一個模型，公平性優先於單邊效率。

---

## 六、為什麼不用 Colab / Kaggle

acuity 容器裡是 **Python 3.8.20 / TF 2.10.0 / Keras 2.10.0** —— 也就是 **Keras 2**，
而 `pegasus import keras` 就是配 Keras 2 的 h5 寫的。

Colab 和 Kaggle 現在都預設 **Keras 3**，之前踩到的都是這個引起的：
- `.keras` 格式不吃 `include_optimizer`
- `tf.keras.Input()` 不能放進 `Sequential([...])` 第一層

在容器裡訓練，訓練和轉檔用的是**同一個 TF**，這些問題不存在。而且少一次搬檔案。

這個模型也不需要 GPU：106K 參數、28×28 輸入，CPU 一分鐘內訓完。

**什麼時候才需要雲端**：模型大到 CPU 訓不動（例如 fine-tune MobileNetV2）。
那時流程是雲端訓練 → 存 **Keras 2 格式的 h5** → 拿進容器轉檔。
訓練和轉檔本來就可以分開，只是版本要自己顧。Kaggle 比 Colab 適合，因為版本能鎖（pin `tensorflow==2.10`）。

`mnist_train_colab.ipynb` 還留在 repo 裡，想用隨時可以用。

---

## 七、環境限制（不要忘記）

- **公司電腦沒有管理員權限**，不要觸發 UAC：
  不裝 Docker Desktop、不跑 `winget install`、不寫 HKLM / Program Files、不裝服務或驅動。
  現有的 WSL2 Ubuntu（root + 免密碼 sudo）和裡面的 `apt` 都可以用，實測不會提權。
- **沒有 SD 卡**，只能燒 flash。
  這也是為什麼一定要走「換 flash 裡的 `.nb`」這條路 —— 沒 SD 卡時
  `DEFAULT_IMGCLASS` 和 `CUSTOMIZED_IMGCLASS` 在 `NNImageClassification.cpp` 裡指向同一個
  `&img_classification`，讀的都是 flash 的 `NN_MDL/img_class.nb`。
- **PAT 權限偏大**（`delete_repo, repo, write:discussion, write:packages`），
  而且它在 `/root/.docker/config.json` 裡是 **base64 編碼、沒有加密**。
  image 已經在本地了，只拉 image 的話縮到 `read:packages` 就夠；
  要用它 push repo 才需要保留 `repo`。建議分開兩把。

---

## 八、Mac 上的注意事項

如果是 **M 系列（ARM）Mac**：`ghcr.io/ameba-aiot/acuity-toolkit:6.18.8` 幾乎確定只有
`linux/amd64`，會用 Rosetta 模擬跑。**能動，但會慢** ——
訓練那一分鐘可能變三五分鐘，轉檔也一樣。不會壞，只是別預期一樣快。
Intel Mac 沒這個問題。

板子的部分 Mac 完全可以：Arduino IDE 2.x 裝 AmebaPro2 4.1.0，
port 名字會是 `/dev/cu.usbserial-*` 之類，不是 COM6。

---

## 九、檔案清單

| 路徑 | 是什麼 |
|---|---|
| `MnistNpuBench/MnistNpuBench.ino` | AMB82 跑分 sketch。直接呼叫 VIPLite，無 camera/VOE/WiFi/RTSP，印 CSV。`NN_CLK_SEL` 掃 NPU 時鐘 |
| `MnistNpuBench/mnist_bench_data.h` | 測試圖（3 通道 planar）+ PC 端期望輸出，兩塊板子吃同一張圖 |
| `pico2/MnistPicoBench/` | ★ Pico 2 跑分專案（CMake + pico-sdk 2.1.1 + CMSIS-NN 7.0.0） |
| `pico2/MnistPicoBench/main.c` | 計時骨架，一支程式跑三種模式，印同格式 CSV |
| `pico2/MnistPicoBench/net_float.c` | float32 naive 基準線（Keras 原始權重排列） |
| `pico2/MnistPicoBench/net_int8.c` | int8 naive + CMSIS-NN 兩份實作 |
| `pico2/MnistPicoBench/model/*.h` | 權重、量化參數、測試圖、TFLite 期望輸出（自動產生） |
| `pico2/MnistPicoBench/kernels.h` | 三個模式的共同介面 |
| `pico2/MnistPicoBench/CMakeLists.txt` | 手寫，含 CMSIS-NN 接線與旗標；**不要讓擴充改它** |
| `pico2/MnistPicoBench/.vscode/` | 擴充要的六個 json（用它自己的產生器產的，不是手抄） |
| `scripts/gen_pico_data.py` | 從 `.tflite` 抽量化參數產上面那三個標頭 |
| `model/mnist_cnn_int8.tflite` | 全整數 int8 量化模型（112,352 B） |
| `model/mnist_cnn.h5` | 訓練好的 Keras 2 模型（448 KB） |
| `model/mnist_cnn.nb` | 轉好的 NBG，可直接蓋成 `img_class_cnn.nb`（123,912 B） |
| `model/mnist_cnn_inputmeta.yml` | ★ 改好的版本 |
| `model/mnist_cnn_inputmeta.yml.orig` | 產生出來的原始版本，`diff` 一下就知道改了什麼 |
| `scripts/dk.sh` | 容器包裝器，所有容器指令都經過它 |
| `scripts/train_mnist.py` | 容器內訓練 + 產 200 張校正 PNG（容器沒有 PIL，用 `tf.io.encode_png`） |
| `scripts/quant_mnist.sh` | 取代官方 quantize，補 `--iterations` |
| `scripts/verify_mnist.sh` | 跑滿 200 張的 float / uint8 推論 |
| `scripts/score_mnist.py` | 對答案、算一致率與最大誤差 |
| `wsl-docker-up.sh` | 在 WSL 裡手動叫起 dockerd（沒 systemd） |
| `patch_inputmeta.py` | 純文字改 inputmeta（不依賴 yaml 套件） |
| `README.md` | 研究日誌 —— 為什麼這樣做、排除過哪些路、每個結論的證據 |
| `realtek-email-A-access.md` | 索取離線工具權限的信（已寄出，已拿到） |
| `realtek-email-B-bugreport.md` | 缺陷回報草稿，**未寄出**。坑 2 的兩處 `--iterations` 寫死值得補進去 |
| `_backup_amb82/` | ★ 板子原廠檔備份，**不在 git 裡**，還原用 |
| `acuity_examples_c901149.tgz` | ★ 授權工具包，**不在 git 裡**，318 MB |

---

## 十、下一步

**主線結案。** 兩塊板子都量完，時鐘域也判定完了，該有的數字都有了：
13.4x（端到端）/ ~330x（牆鐘）/ 99.2x（架構）/ 5.5%（利用率）。
訓練 → 轉檔 → 燒錄 → 懂原理這條學習路線也走完一遍了。

### 收尾（隨時可做，不影響結論）
1. 還原 `_backup_amb82/` 的原廠 `.nb`
2. 把坑 2 補進 `realtek-email-B-bugreport.md` 再寄出
   （兩處 `--iterations 1` 寫死 + `install.sh` 的 `$?` 永遠成功）
3. 縮 GitHub PAT 權限（現在有 `delete_repo, repo, write:discussion, write:packages`，太寬）

### 如果要把跑分挖更深（選做，都有明確假設要驗）
4. **Pico 2 權重搬 SRAM**：驗「4.50 cycles/MAC 是被 flash XIP 拖累」。
   搬進去後掉到 2 附近就成立。
5. **AMB82 跑同一份 CMSIS-NN**：拿到「同一顆晶片、CPU vs NPU」這個唯一
   完全誠實的加速比。先確認 `__ARM_FEATURE_DSP`。
6. **讀回 NN 時鐘暫存器**：讓時鐘域從「強烈推論」變成「直接觀測」。

### 之後玩別的（SD 卡已到貨）
- `ObjectDetectionImage`（要 SD 讀 `image_list.txt`）現在可跑
- 模型放 SD → 不必全塞 flash（YAMNet 一顆就吃 84%），換模型也不用重燒整包
- `AudioClassification` 一直沒燒過，可以補
- ⚠ **WiFi 還是不通**（802.1X）→ RTSP 那幾個範例要用手機熱點
- ⚠ 切成 SD 載入後，開機找不到 `.nb` 會**直接卡住**（不是印警告跳過），
  檔名路徑要對

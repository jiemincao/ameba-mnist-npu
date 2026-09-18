# AmebaPro2 SDK 研讀計畫

> 目標不是「會用 API」，是**看懂 Realtek 的 SDK 長什麼樣、怎麼被包成 Arduino**。
> 優先順序由使用者指定：**video > i2s > spi > SDK 怎麼變成 Arduino**。
>
> 相關文件：[COURSE.md](COURSE.md)（觀念）、[REFERENCE.md](REFERENCE.md)（實測數字與坑）

---

## 0. 先搞清楚：三個 repo 的關係

| repo | 是什麼 | 要不要抓 |
|---|---|---|
| `Ameba-AIoT/ameba-rtos-pro2` | **官方 SDK**，296 MB，VIPLiteDrv 2.0.0（與板子一致） | ✅ **主要教材** |
| `Ameba-AIoT/ameba-arduino-pro2` | Arduino core 的原始碼倉，12.8 GB（歷史含 blob） | ⚠️ 只在 Day 5 用，且只抓 `--depth 1` |
| `Freertos-kvs-LTS/ambpro2_sdk` | 舊官方（`ambiot/ambpro2_sdk` 301 轉到這），2025-06 停更，VIPLiteDrv 1.12.0 | ❌ 版本對不上，不用 |

```bash
cd /d/workdir
git clone --depth 1 https://github.com/Ameba-AIoT/ameba-rtos-pro2.git
```

### 對照表：同一棵樹，兩種樣子

```
ameba-rtos-pro2/component/...          ←→   Arduino15/.../4.1.0/system/component/...
        有 .c(實作)                              只有 .h(4282 個),實作在 variants/common_libs/*.a
```

**研讀方法固定為這一招**：任何主題都同時開兩邊，
左邊看 SDK 的 `.c` 知道它「怎麼做」，右邊看 Arduino 的 `.h` + library 知道它「被包成什麼樣」。

---

## Day 1 — Video（上游）：畫面怎麼來的

**主檔**：`component/video/driver/RTL8735B/video_api.c`

| 要回答的問題 | 為什麼重要 |
|---|---|
| `video_open()` 到底設了什麼？channel 是什麼概念？ | AMB82 一路上有 4 個 channel，猜拳用的是哪個 |
| buffer 從哪裡來、誰負責釋放？ | 這決定了為什麼相機不能同時餵太多消費者 |
| **`configInputImageColor(1)` 的終點在哪？** | ★ 我們踩過的坑：傳 0 會靜默轉灰階 |
| FCS（Fast Camera Start）是什麼？`video_boot.c` | 為什麼開機那麼快、`fcs_data_f37.bin` 是什麼 |

**順帶看**：`isp_ctrl_api.h` —— 沒有實作，但 header 就是 ISP 的**功能清單**（AE/AWB/WDR/降噪有哪些旋鈕）。

**看不到的部分（先講清楚，不要浪費時間找）**
- ISP 本體：跑在 **VOE 協處理器**上的獨立韌體（`firmware.bin` / `isp_iq.bin` / `iq_f37.bin`），不在這棵樹上
- `librtsremosaic.a`：Quad-Bayer remosaic，閉源
- **F37 sensor driver**：AMB82-MINI 用 F37，但公開 SDK 只給 gc4693 / k306p / ps5270 / ps5420；F37 在 `variants/common_sensor_sel_libs/SENSOR_F37/libarduino_sensor_sel.a`

---

## Day 2 — Video（下游）：畫面怎麼進 NPU

**主檔**：`component/media/mmfv2/module_vipnn.c`

這是猜拳專案「相機 → NPU」那條線的**全部膠水**。在 Arduino 裡它被編進 `libmmf.a`，看不到。

| 要回答的問題 |
|---|
| mmfv2 的 module / link / siso 模型是什麼？（Realtek 自己的 pipeline 框架） |
| 相機 buffer 怎麼變成 NPU 的 input tensor？誰做 resize、誰做 planar 轉換？ |
| `.nb` 什麼時候被載入？誰呼叫 nbg_linker？ |
| 推論結果怎麼回到 callback？為什麼會有 OS tick 等待（第 6 章量到的 96% 時間） |

**同時對照**：`libraries/NeuralNetwork/src/NNImageClassification.cpp`（309 行）
→ 看完會發現 Arduino 那層只是薄薄一層膠水。

**牆在哪**：`VIPLiteDrv_2.0.0/` 在官方 SDK 只有 `include/`，Arduino core 多給了
`vip_drv/` `vip_hal/` `nbg_linker/` 三個資料夾但**也只有 .h**。
NPU 暫存器層是 VeriSilicon 的，公開管道拿不到。

---

## Day 3 — I2S（★ 最想學之一）

**這天是暫存器層級，全是原始碼，沒有黑盒。**

```
component/soc/8735b/fwlib/rtl8735b/source/ram/
    hal_i2s.c           ← HAL 層:對外 API
    rtl8735b_i2s.c      ← 暫存器層:真正在寫 register
    hal_sport.c / rtl8735b_sport.c   ← TDM / 多聲道
    hal_audio.c / rtl8735b_audio.c   ← 內建 audio codec
    hal_gdma.c          ← ★ DMA,I2S 的資料靠它搬
include/
    rtl8735b_i2s_type.h ← 暫存器 bitfield 定義
component/audio/driver/i2s/
    alc5640.c alc5651.c sgtl5000.c ...  ← 外接 codec 的 I2C 設定
```

| 要回答的問題 |
|---|
| I2S 的 LRCK / BCLK / MCLK 時脈怎麼算出來的？看 `hal_i2s.c` 的 clock 設定 |
| DMA descriptor 怎麼串？為什麼是 ping-pong buffer？（`hal_gdma.c`） |
| `hal_i2s.c`（HAL）跟 `rtl8735b_i2s.c`（暫存器）的分層界線在哪？**Realtek 所有周邊都是這個雙層結構** |
| 外接 codec 的 `.c`（如 `alc5651.c`）其實只是一張 I2C 暫存器表 —— 跟 sensor driver 同一個模式 |

**Arduino 那邊**：`libraries/` **沒有 I2S library**。音訊只能走 `Multimedia`（mmfv2 的 `module_i2s.c` / `module_audio.c`）。
→ 這正是 Day 6 的題目來源。

---

## Day 4 — SPI

**先分清楚兩個完全不同的東西**（Realtek 命名很容易搞混）：

| | 檔案 | 是什麼 |
|---|---|---|
| **SSI** | `hal_ssi.c` / `rtl8735b_ssi.c` | 通用 SPI 主/從機 ← **Arduino 的 `SPI` 就是這個** |
| **SPIC** | `hal_spic.c` / `hal_spic_ns.c` | SPI **Flash 控制器**,專供外部 flash,有 XIP |

| 要回答的問題 |
|---|
| `hal_ssi.c` 的 polling / interrupt / DMA 三種模式差在哪？什麼時候該用哪個 |
| SPIC 的 XIP 是怎麼做到「flash 當成記憶體直接讀」的？這跟 OTA 分區有什麼關係 |
| `libraries/SPI/` 把 `hal_ssi` 包成 Arduino `SPI.transfer()` 時，**丟掉了哪些能力**？ |
| `hal_spic_ns.c` 的 `_ns` = non-secure —— TrustZone 分割，這顆 M33 有 secure / non-secure 兩側 |

---

## Day 5 — SDK 是怎麼變成 Arduino 的

**這天不讀驅動，讀建置系統。**

### 已經盤過的事實

```
Arduino core = 官方 SDK 砍掉全部 .c 編成 34 個 .a
             + cores/ambpro2/   (語法糖 → libarduino.a)
             + libraries/       (WiFi / NeuralNetwork / Multimedia ...)
             + boards.txt       (13 個選單,每項只是設一個 build.xxx 變數)
             + platform.txt     (編譯 / 連結 / 打包 / 燒錄配方)
```

### 要弄懂的五支閉源 exe（`tools/ameba_pro2_tools/1.4.7/`）

| # | 執行檔 | 做什麼 |
|---|---|---|
| 2 | `prebuild_windows.exe` | 依 camera / NN / OTA 選單換 toolchain 與模型來源 |
| 3 | `ino_validation_windows.exe` | **掃你的 .ino**，看你用了哪些 NN API |
| 4 | `cmodel_backup_windows.exe` | **備份 `model_classification.c`** ← `.orig` 就是它做的 |
| 5 | `nn_json_modify_windows.exe` | **改寫 `nn_models.json`** ← 決定哪些 `.nb` 進 flash |

> ★ 這解釋了 `CUSTOMIZED_IMGCLASS` 那個坑為什麼難查：**邏輯不在任何一份 .c 裡，在 exe 裡。**

### 打包與燒錄

```
sketch.o + 31 個 .a --ld script--> application.ntz
      ↓ postbuild_windows.exe，依 amebapro2_partitiontable.json
boot.bin + partition.bin + firmware.bin + nn_model.bin(fwfs) → flash_ntz.bin
      ↓ image_windows.exe → uartfwburn
0x60000 / 0x460000 / 0x530000
```

**注意**：`.nb` 不在 application 裡，它在獨立的 fwfs 分區 —— 所以換模型不用重編譯。

### 要做的事
1. 抓 `Ameba-AIoT/ameba-arduino-pro2 --depth 1`，看 `cores/` 與 `libraries/` 的原始碼
2. 找出 `.a` 是怎麼從 SDK 編出來的（build script / CI）
3. 回答：**如果我要自己加一個周邊的 Arduino library，要改哪幾個檔？**

---

## Day 6 — 驗收題（選做）：自己補一個 I2S library

把 Day 3 + Day 5 接起來：

> `libraries/` 目前**沒有 I2S**。
> 用 Day 3 學到的 `hal_i2s` API，照 `libraries/SPI/` 的結構，寫一個 `libraries/I2S/`，
> 讓 sketch 可以 `I2S.begin(44100, 16)` 然後 `I2S.write(buf, len)`。

做得出來，就代表你真的懂了「SDK → Arduino」這層殼。

---

## 附錄：可見度總表

| 主題 | 官方 SDK | Arduino core | 備註 |
|---|---|---|---|
| I2S / SPI / GDMA / UART / I2C 等周邊 | ✅ **完整 .c** | ❌ 只有 .h | 最好的教材 |
| video_api / video_boot / snapshot | ✅ .c | ❌ | |
| sensor driver | ✅ 4 顆有 .c | ❌ | **F37 沒有** |
| mmfv2（module_vipnn 等 25 個） | ✅ .c | ❌ | |
| **ISP 本體** | ❌ 只有 .h | ❌ | 跑在 VOE 協處理器，獨立韌體 |
| **VIPLite（NPU driver）** | ❌ 只有 include/ | ❌ 只有 .h | VeriSilicon，要 NDA |
| remosaic | ❌ `.a` | ❌ | |
| Arduino cores / libraries | — | ✅ .c/.cpp | 只有 Arduino 側有 |
| 5 支 prebuild exe | — | ❌ 閉源 exe | 只能黑箱觀察行為 |

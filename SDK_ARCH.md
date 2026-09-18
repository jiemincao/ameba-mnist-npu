# AmebaPro2 SDK 架構

> 這份回答三個問題：**SDK 長什麼樣**、**怎麼被包成 Arduino**、**ISP / Video 這條線要怎麼看**。
> 裡面每一條都是在這台電腦上比對驗證過的，不是照抄文件。
>
> 讀書計畫 → [SDK_STUDY_PLAN.md](SDK_STUDY_PLAN.md)　觀念 → [COURSE.md](COURSE.md)　實測數字 → [REFERENCE.md](REFERENCE.md)

---

## Part 1. 我們手上有什麼

| | 位置 | 內容 |
|---|---|---|
| **官方 SDK** | `D:\workdir\ameba\ameba-rtos-pro2\` | 575 MB，2914 個 `.c` + 3545 個 `.h`，版本 `9.6_r` |
| **Arduino core** | `Arduino15\packages\realtek\hardware\AmebaPro2\4.1.0\` | 241 MB，`system/` 底下 **4173 個 `.h`，0 個 `.c`** |
| **打包/燒錄工具** | `Arduino15\packages\realtek\tools\ameba_pro2_tools\1.4.7\` | 分區表、5 支閉源 exe、`.nb` 模型 |

### 兩棵樹的比對結果（實測）

```
兩邊都有且相同 : 1507
兩邊都有但不同 :   70     ← 僅 4.4%
只有 Arduino 有: 2596
```

**注意**：SDK 全部是 **CRLF**、Arduino core 是 **LF**。直接 `diff` 會每行都報不同（假差異），
**一律要加 `--strip-trailing-cr`**。（第一次比 `video_api.h` 時就被騙了：假差異 1208 行，真差異 40 行。）

「只有 Arduino 有」的 2596 個不是 Realtek 小氣 —— 反過來，**Arduino core 的 header 比公開 SDK 完整**
（`audio/3rdparty` 945、`wifi/driver` 421、`bluetooth/rtk_stack` 244、`video/driver` 97）。
而「只有 SDK 有」的幾乎都是同一套 lib 的多個版本（lwip 2.1.3 / 2.2.0 / 2.2.1、mbedtls 2.4 ~ 4.1），
Arduino 只各挑一版帶走。

> **研讀方法：讀實作（`.c`）看 SDK，查 API 宣告看 Arduino core。兩邊互補。**

---

## Part 2. SDK 架構

```
ameba-rtos-pro2/
├── component/           ← 全部的功能模組
│   ├── soc/8735b/       ★ 晶片層：周邊驅動、CMSIS、開機流程
│   ├── video/           ★ 相機：video_api、sensor driver、ISP 介面
│   ├── media/mmfv2/     ★ 多媒體 pipeline 框架（含跑 NPU 的 module_vipnn）
│   ├── audio/           I2S codec 設定
│   ├── os/freertos      ┐
│   ├── lwip/ ssl/ wifi/ │ 一般 RTOS / 網路元件
│   ├── bluetooth/ usb/  │
│   ├── file_system/     ┘
│   └── example/         Realtek 自己的範例（KVS WebRTC 等）
├── project/realtek_amebapro2_v0_example/
│   └── src/             主程式、test_model、VIPLiteDrv（只有 include）
└── tools/               編譯與打包腳本
```

### ★ 記住這個：Realtek 所有周邊都是雙層結構

```
hal_xxx.c            ← HAL 層：對外 API、參數檢查、狀態機
rtl8735b_xxx.c       ← 暫存器層：真正在寫 register
rtl8735b_xxx_type.h  ← 暫存器 bitfield 定義
```

看懂一組（建議從 I2S 開始），其他全部同構。實際存在的：

```
component/soc/8735b/fwlib/rtl8735b/source/ram/
  hal_i2s.c   rtl8735b_i2s.c      ← I2S
  hal_ssi.c   rtl8735b_ssi.c      ← 通用 SPI 主/從機（Arduino 的 SPI 就是包這個）
  hal_spic.c                      ← SPI Flash 控制器，含 XIP（完全不同的東西，別搞混）
  hal_sport.c rtl8735b_sport.c    ← TDM / 多聲道音訊
  hal_audio.c rtl8735b_audio.c    ← 內建 codec
  hal_gdma.c                      ← DMA，I2S / SPI 都靠它搬資料
  hal_i2c.c  hal_uart.c  hal_gpio.c  hal_pwm.c  hal_adc.c  hal_timer.c
  hal_flash.c hal_snand.c hal_sdhost.c hal_crypto.c hal_eth.c ...
```

檔名帶 `_ns` 的是 TrustZone **non-secure** 側的版本（例如 `hal_spic_ns.c`），不是另一顆晶片。

**版本對齊狀況（實測）**：`hal_i2s.h` / `rtl8735b_i2s.h` / `hal_ssi.h` / `rtl8735b_ssi.h` /
`hal_audio.h` / `hal_spic.h` 與 Arduino core **零差異** ✅。只有 `hal_gdma.h` 有漂移。
→ **周邊驅動是最乾淨的教材，沒有黑盒、沒有版本風險。**

### 看不到的三塊

| | 為什麼 |
|---|---|
| **VIPLite（NPU driver）** | 官方 SDK 只有 `VIPLiteDrv_2.0.0/include/`；Arduino core 多給 `vip_drv/` `vip_hal/` `nbg_linker/` 但一樣只有 `.h`。VeriSilicon 的 IP，要 NDA |
| **ISP 本體** | 不是 KM4 的程式碼，是 **VOE 協處理器的獨立韌體**（見 Part 4） |
| **remosaic** | `librtsremosaic.a`，Quad-Bayer 去馬賽克 |

---

## Part 3. 怎麼包成 Arduino

### 3.1 不是「砍掉 .c」，是「同一份原始碼切兩個版本」

原始碼裡直接埋著 Arduino 分支，core 裡共 **22 個檔案**帶 `ARDUINO_SDK`：

```c
// component/video/driver/RTL8735B/video_api.h
#ifdef ARDUINO_SDK
#ifndef __cplusplus
#include <stdatomic.h>
#else
#include <atomic>                       // sketch 是用 C++ 編的
#define atomic_bool std::atomic_bool
#endif
#else
...
#endif

#ifdef ARDUINO_SDK
void set_video_logging(int enable);     // ← boards.txt 的 "Multimedia logs" 選單
#endif
```

開關在 `boards.txt:29`：
```
Ameba_AMB82-MINI.build.extra_flags=-DARDUINO_SDK -DARDUINO_AMBPRO2 -DBOARD_AMB82_MINI ...
```

這個分支被編進 34 個 `.a`，然後**只出貨 header**。所以 Arduino core 不是 SDK 的子集，
是 SDK 的**另一個編譯組態**。

### 3.2 四層殼

```
Arduino core = SDK（開 ARDUINO_SDK 編成 34 個 .a）
             + cores/ambpro2/    語法糖 Serial / pinMode / delay → libarduino.a
             + libraries/        WiFi / NeuralNetwork / Multimedia / SPI ...
             + boards.txt        13 個選單，每項其實只是設一個 build.xxx 變數
             + platform.txt      編譯 / 連結 / 打包 / 燒錄的配方
```

例：`boards.txt:64-65`
```
menu.04_CameraOption.f37.build.camera_opt=SENSOR_F37
menu.04_CameraOption.f37.build.camera_flags=-DARDUINO_SENSOR_F37
```
IDE 上那個下拉選單，本體就是一個 `-D`。

### 3.3 ★ 最髒的部分：5 支閉源 exe

按下編譯後，**在 gcc 跑起來之前**先執行（`platform.txt:135-153` 的 `recipe.hooks.prebuild.*`）：

| # | 執行檔 | 做什麼 |
|---|---|---|
| 1 | `chmod` | 只有 linux / mac 用得到 |
| 2 | `prebuild_windows.exe` | 依 camera / NN / OTA 選單換 toolchain 與模型來源 |
| 3 | `ino_validation_windows.exe` | **掃描你的 `.ino`**，判斷你用了哪些 NN API |
| 4 | `cmodel_backup_windows.exe` | **備份 `model_classification.c`** ← 那個 `.orig` 就是它做的 |
| 5 | `nn_json_modify_windows.exe` | **改寫 `nn_models.json`** ← 決定哪些 `.nb` 進 flash |

> **這解釋了 `CUSTOMIZED_IMGCLASS` 那個坑為什麼查不到：邏輯不在任何一份 `.c` 裡，在 exe 裡。**
> Arduino IDE「自動幫你挑模型」的代價是：**它會在你背後改你的原始碼。**

### 3.4 連結與打包

```
sketch.o + 31 個 .a（libnn.a / libmmf.a / libvideo_ntz.a / libarduino.a …）
   │  platform.txt:169 recipe.c.combine
   │  ld script = variants/ameba_amb82-mini/linker_scripts/gcc/
   ▼
application.ntz
   │  platform.txt:172  postbuild_windows.exe
   │  依 amebapro2_partitiontable.json 排版
   ▼
boot.bin + partition.bin + firmware.bin + nn_model.bin(fwfs) → flash_ntz.bin
   │  image_windows.exe → uartfwburn
   ▼
flash 0x60000 / 0x460000 / 0x530000      （platform.txt:211）
```

**`.nb` 不在 `application.ntz` 裡**，它在獨立的 fwfs 分區 —— 所以換模型不用重編譯，
也所以 `nn_json_modify` 要另外存在。

### 3.5 ⚠️ 最重要的一條：ABI 不相容，SDK 只能當教材

新 SDK 的 `video_api.h` 跟 Arduino 4.1.0 差 40 行，其中有**結構體佈局變動**：

```diff
- video_params_t *param;     // Arduino 4.1.0：指標
+ video_params_t  param;     // 新 SDK：整個內嵌 ★ 結構體大小完全不同

- uint32_t     voe_scale_up_en;
- video_roi_t  voe_scale_up_roi;
+ struct { uint32_t enable; uint32_t use_roi; video_roi_t roi; } scale_up_info;
```

新 SDK 多出來、Arduino 4.1.0 沒有的東西：
`VIDEO_SET_DYN_ROI` / `VIDEO_GET_ROI_STAT`、`isp_gain_mode` / `isp_gain`、`dyn_scale_up_en`、
`video_get_isp_info()` / `video_get_isp_info_status()` / `video_reset_isp_info_status()`

> **絕對不要拿新 SDK 的 `.c` 去配 Arduino 的 `.a`。** 結構體佈局已經不同，
> 編得過、跑起來會直接踩爛記憶體，而且症狀會很詭異。
> **SDK 是閱讀教材，不是補丁來源。**

---

## Part 4. ISP / Video 怎麼看

### 4.1 先建立正確的心智模型：這是兩顆 CPU

```
┌─────────────── KM4 (Cortex-M33 @500MHz) ────────────────┐
│  你的 sketch                                             │
│  libraries/NeuralNetwork/   libraries/Multimedia/        │
│  media/mmfv2/module_video.c   module_vipnn.c             │
│  video/driver/RTL8735B/video_api.c       ← ★ 讀這個      │
│  soc/.../ram/video/  hal_video.h  hal_voe.h              │
└───────────────────────┬─────────────────────────────────┘
                        │  命令 + 共享記憶體（不是函式呼叫！）
┌───────────────────────▼─────────────────────────────────┐
│  VOE 協處理器                                             │
│  voe.bin / voeiram.bin / voeirom.bin / voedrom.bin       │
│  ISP：demosaic、AE、AWB、降噪、WDR                        │
│  H.264 / HEVC / JPEG 編碼器                               │
└─────────────────────────────────────────────────────────┘
```

**ISP 沒有原始碼，不是因為 Realtek 小氣 —— 是它根本不在這棵樹上。**
它是另一顆 CPU 的韌體 binary。KM4 這邊只負責「下命令、收結果」。

### 4.2 但 KM4↔VOE 的指令集是完整公開的

`component/soc/8735b/fwlib/rtl8735b/lib/source/ram/video/rtl8735b_voe_cmd.h`

```c
#define VOE_OPEN_CMD                0x206
#define VOE_CLOSE_CMD               0x207
#define VOE_OUT_CMD                 0x20B
#define VOE_ROI_REGION_CMD          0x211
#define VOE_YUV_OUT_CMD             0x217
#define VOE_ISP_CTRL_GET_CMD        0x240
#define VOE_ISP_CTRL_SET_CMD        0x241
#define VOE_ISP_TUNING_GET_IQ       0x242
#define VOE_ISP_TUNING_GET_STATIS   0x244
#define VOE_ISP_TUNING_READ_VREG    0x249   // ★ 直接讀 ISP 暫存器
#define VOE_ISP_TUNING_WRITE_VREG   0x24A   // ★ 直接寫
#define VOE_ISP_SET_SENSOR_MODE     0x24B
#define VOE_ISP_GET_3A_STATIS       0x24D   // ★ AE / AWB / AF 統計值
#define VOE_ISP_GET_REAL_FPS        0x24E
```

共 23 條 `VOE_ISP_*` 指令，`video/driver/RTL8735B/isp_ctrl_api.h` 再往上包成 **54 支 API**。

> **對「調畫質」來說，這比看 ISP 原始碼還實用** —— 你要的本來就不是 demosaic 演算法怎麼寫，
> 而是「有哪些旋鈕、怎麼轉、轉完怎麼讀回來」。這些全都在。

### 4.3 sensor driver 是編譯好的 VOE 模組（含程式碼 + 暫存器表）

`.../lib/source/ram/video/voe_bin/` 底下約 190 個 blob。VOE 韌體本體四份
（`voe.bin` / `voeirom.bin` / `voeiram.bin` / `voedrom.bin`），再加 40 顆 sensor 各三份：

```
sensor_f37.bin    ← 暫存器初始化序列（AMB82-MINI 用的就是 F37）
iq_f37.bin        ← 畫質調校參數
fcs_data_f37.bin  ← 快速開機資料（916 B）
```

`fcs_data_f37.bin` 與 Arduino `tools/ameba_pro2_tools/1.4.7/` 那份**位元組完全相同** ✅
→ camera 這條線的資料是版本對齊的（只有 API header 漂移）。

`sensor_f37.bin` **不是純資料**——它是一個編譯好的模組，載入到 VOE 的 `0x70000000` 執行：

```
0x00 .. 0x1F   檔頭：magic ea07061d + "RTL8735B_VOE_1.7.1.0" + payload 長度
0x20 .. 0x4B   兩個 0x7000xxxx 位址欄 + 補零
0x4C .. 0x23B  ★ 暫存器初始化表（124 筆，8-bit 位址）
0x23C .. 尾     ARM Thumb-2 機器碼（sensor driver 本體，跑在 VOE 上）
```

暫存器表的記錄格式是 `(u16 位址 LE, u16 值 LE)`：

```
12 00  40 00   →  reg 0x12 = 0x40    進 standby（第 0 筆）
0e 00  19 00   →  reg 0x0e = 0x19  ┐
0f 00  04 00   →  reg 0x0f = 0x04  │  PLL
10 00  24 00   →  reg 0x10 = 0x24  │
11 00  80 00   →  reg 0x11 = 0x80  ┘
...
12 00  00 00   →  reg 0x12 = 0x00    開始串流（第 119 筆）
```

解出來的關鍵數字：**VTS = 1125**（1080p 標準幀長）、**HTS = 1280**。
反組譯 0x23C 之後的程式碼，`mov.w r2,#1920 / mov.w r3,#1080` 直接出現在立即數裡。

85 顆 sensor 全部同格式，只有位址寬度不同（8-bit 49 顆 / 16-bit 36 顆，
IMX307 用 `0x3005`、GC4653 用 `0x03fe`）。

> 工具 → [scripts/decode_sensor_bin.py](scripts/decode_sensor_bin.py)
> 結果 → [docs/f37_regs.txt](docs/f37_regs.txt)
> **完整推導過程與方法 → [SENSOR_BIN.md](SENSOR_BIN.md)**

而 `video_api.c` 的 `video_init()` 正是把這兩個 blob 搬進 VOE，整條線在這裡閉合：

```c
// video_api.c:2681
hal_video_adapter_t *video_init(int iq_start_addr, int sensor_start_addr)
{
    voe_info.iq_addr     = iq_start_addr;
    voe_info.sensor_addr = sensor_start_addr;

    if (hal_voe_ready() != OK) {
        extern int __voe_code_start__[];
        if (!hal_voe_fcs_check_OK()) {
            iq_addr     = video_load_iq(iq_start_addr);
            sensor_addr = video_load_sensor(sensor_start_addr);
            ...
            hal_video_load_iq((voe_cpy_t)hal_voe_cpy,     (int *)iq_addr,     __voe_code_start__);
            hal_video_load_sensor((voe_cpy_t)hal_voe_cpy, (int *)sensor_addr, __voe_code_start__);
        }
    }
}
```

### 4.4 `video_api.c` 該怎麼讀（2700+ 行，別從第一行開始看）

`component/video/driver/RTL8735B/` 整個目錄 7912 行。按這個順序挑函式讀：

| 順序 | 函式 | 行 | 為什麼 |
|---|---|---|---|
| 1 | `video_init()` | 2681 | VOE 開機、載入 iq / sensor blob。**整條線的起點** |
| 2 | `video_open()` | 2043 | 開串流、設 channel、掛 callback。528 行，最肥但最核心 |
| 3 | `video_ctrl()` | 519 | 所有執行期控制的總入口 |
| 4 | `video_buf_calc()` / `video_buf_heap_calc()` | 1194 / 1405 | **記憶體怎麼算的** —— 解釋為什麼 channel 開太多會失敗 |
| 5 | `isp_ctrl_cmd()` / `iq_tuning_cmd()` | 352 / 389 | ISP 調校的命令介面 |
| 6 | `video_i2c_cmd()` | 463 | 直接對 sensor 下 I2C，接得回 4.3 那張表 |
| 7 | `video_get_isp_info()` | 1100 | 讀曝光 / 增益（**Arduino 4.1.0 沒有這支**） |
| — | `video_close()` | 2571 | 收尾對照組 |

輸出格式（`video_api.h`）：
```c
#define VIDEO_HEVC_OUTPUT  0x20
#define VIDEO_H264_OUTPUT  0x21
#define VIDEO_JPEG_OUTPUT  0x22
#define VIDEO_NV12_OUTPUT  0x23
#define VIDEO_RGB_OUTPUT   0x24   // ★ 餵 NN 走這個
#define VIDEO_NV16_OUTPUT  0x25
```

### 4.5 完整追蹤範例：`configInputImageColor` 那個坑

這是我們真的踩過的。從 sketch 一路追到 NPU 前一刻：

```
① RpsGame.ino
   imgclass.configInputImageColor(1);
        ↓
② libraries/NeuralNetwork/src/NNImageClassification.cpp
   void NNImageClassification::configInputImageColor(int color) {
       get_input_image_color(color);        // 1: RGB   0: BW
   }
        ↓
③ libraries/NeuralNetwork/src/model_classification.c:2342
   void get_input_image_color(int input_image_rgb) {
       input_image_color = input_image_rgb;   // 只是存起來
   }
        ↓
④ model_classification.c:2360  classification_preprocess()
   img_resize_planar(&img_in, roi, &img_out);   // 相機圖 → 96x96 planar RGB

   if (input_image_color == 0) {
       img_rgb2gray(&img_out);                  // ★★ 犯罪現場
   }
   dcache_clean_by_addr(...);                   // 之後就進 NPU 了
        ↓
⑤ model_classification.c:2347
   static void img_rgb2gray(img_t *img) {
       uint8_t *r    = img->data;
       uint8_t *g    = img->data + w*h;
       uint8_t *b    = img->data + w*h*2;
       uint8_t *gray = img->data;               // ← gray 就是 r plane！
       for (i...)  gray[i] = (r[i]*19595 + g[i]*38469 + b[i]*7472) >> 16;
   }
```

**為什麼傳 0 會安靜地壞掉**：它把灰階值就地寫進 **R plane**，
**G / B plane 原封不動留著原本的彩色值**。
送進 NPU 的張量因此變成 `[灰階, G, B]` —— 既不是 RGB 也不是灰階，
而且不會報任何錯。模型照樣跑完、照樣給你一個信心值，只是全是垃圾。

> **這就是「讀 SDK / 讀 core 原始碼」的價值**：同樣這個坑，
> 看官方文件永遠看不出來（文件只寫 "1: RGB, 0: BW"），
> 追四層原始碼十分鐘就水落石出。

---

## 附錄 A：可見度總表（已驗證）

| 主題 | 官方 SDK | Arduino core | 備註 |
|---|---|---|---|
| I2S / SPI / GDMA / UART / I2C / PWM / ADC… | ✅ **完整 `.c`** | ❌ 只有 `.h` | header 零差異，**最佳教材** |
| `video_api.c` / `video_boot.c` | ✅ `.c` | ❌ | header 差 40 行，**ABI 不相容** |
| sensor driver（C 版） | ✅ 4 顆 | ❌ | gc4693 / k306p / ps5270 / ps5420 |
| **sensor driver 模組（bin）** | ✅ **85 顆** | 部分在 tools/ | 程式碼+暫存器表，格式已解出 → [SENSOR_BIN.md](SENSOR_BIN.md) |
| mmfv2（25 個 module） | ✅ `.c` | ❌ | `module_vipnn.h` 零差異 |
| **VOE / ISP 指令集** | ✅ header 完整 | ✅ header | 23 條指令 → 54 支 API |
| ISP 實作 | ❌ | ❌ | VOE 韌體 binary，另一顆 CPU |
| VIPLite（NPU driver） | ❌ 只有 include/ | ❌ 只有 `.h` | VeriSilicon，要 NDA |
| remosaic | ❌ | ❌ | `librtsremosaic.a` |
| Arduino `cores/` `libraries/` | — | ✅ `.c` / `.cpp` | 只有 Arduino 側有 |
| 5 支 prebuild exe | — | ❌ 閉源 exe | 只能黑箱觀察行為 |

## 附錄 B：repo 對照

| repo | 狀態 |
|---|---|
| `Ameba-AIoT/ameba-rtos-pro2` | ✅ **現行官方 SDK**，296 MB，我們 clone 的就是這個 |
| `Freertos-kvs-LTS/ambpro2_sdk` | ⚠️ 舊分流（`ambiot/ambpro2_sdk` 轉過來），2025-06 停更，VIPLiteDrv **1.12.0**（板子上是 2.0.0） |
| `Ameba-AIoT/ameba-arduino-pro2` | Arduino core 的原始 repo，12.8 GB（含 `.a` 的歷史版本，不建議 clone） |

## 附錄 C：常見陷阱速查

| 陷阱 | 症狀 | 解法 |
|---|---|---|
| CRLF vs LF | diff 顯示整檔不同 | `diff --strip-trailing-cr` |
| 拿新 SDK `.c` 配舊 `.a` | 記憶體莫名損毀 | 不要做。SDK 只讀不用 |
| `configInputImageColor(0)` | 推論結果全錯但不報錯 | 3 通道模型一律傳 1 |
| `model_classification.c` 被改 | 你的修改消失 | 是 `cmodel_backup.exe` 做的，看 `.orig` |
| SSI / SPIC 搞混 | 找錯檔案 | SSI = 通用 SPI；SPIC = SPI Flash |

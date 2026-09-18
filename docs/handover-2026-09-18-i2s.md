# 交接 — AMB82 I2S / 音訊收音（2026-09-18）

> 目標：走到 **KWS（關鍵詞偵測）**。
> 現在卡在第 1 步：確認板子收音是乾淨的。

---

## 1. 現在到哪了

```
麥克風 ──▶ I2S/PDM ──▶ DMA ring ──▶ MFCC ──▶ CNN ──▶ yes/no
   └─────── 在這 ───────┘           └─── 還沒開始 ───┘
```

| 步驟 | 狀態 |
|---|---|
| 1. I2S 收音正確 | ⏳ **進行中**（改版 sketch 還沒燒） |
| 2. 錄訓練資料 | ✗ |
| 3. Colab 訓練 MicroSpeech | ✗ |
| 4. 轉檔燒錄 | ✗ |
| 5. 板上即時辨識 | ✗ |

---

## 2. ⭐ 下一個動作（就這一件）

檔案：`I2sRingProbe/I2sRingProbe.ino`（已改好，**未編譯未燒錄**）

```
1. Arduino IDE 開 I2sRingProbe/I2sRingProbe.ino
2. Verify（打勾）
3. Upload（箭頭）
4. Serial Monitor，115200
5. 對板子唸幾次 "yes" / "no"
6. 看 50 行輸出的兩欄：DC、speech 時的 peak
```

判讀：
- **DC 欄應該接近 0**。偏離代表 HPF 沒作用 → 會污染 MFCC 第 0 係數（能量項），聽不出來但模型會被騙。
- **speech peak 落在 8000~25000** 算合適。太小 → 增益改 `MIC_30DB`；貼到 32767 → 退回 `MIC_0DB`。

---

## 3. 硬體限制（重要）

**RT584 EVK 沒有外接麥克風**，`i2s-mic` 範例跑不動（它要 SPH6405）。
所有動手實驗都在 **AMB82-MINI**（板上有類比 + PDM 麥克風）。
RT584 的 code 只當**閱讀教材**。

---

## 4. 實機量到的三個結論（已驗證）

| # | 結論 | 證據 |
|---|---|---|
| 1 | **取樣率就是 16.000 kHz** | 25 頁→496ms、50 頁→996ms，誤差固定 −4ms（不隨時間放大 → 是固定偏移不是速率誤差）。25→50 頁剛好 500ms = 20.00ms/page |
| 2 | **開機 pop 要 200ms 才收斂** | peak: 6859→2571→941→…→13，第 10~11 頁進底噪。指數衰減 τ≈20~30ms |
| 3 | **底噪 ≈ 13/32767 ≈ −68 dBFS** | 前端乾淨 |

### 結論 2 的後果 ★

原廠 `module_audio.c:21` 是 `AUDIO_DROP_NUM 2`（只丟 40ms），
那時 peak 還有 941 ≈ **70 倍底噪**。

> 丟 2 頁對「人耳聽不到爆音」夠，對 **KWS 訓練資料不夠**。
> Sketch 裡已改成 `#define DROP_PAGES 10`（200ms）。
> **錄訓練資料時務必沿用 200ms。** 同事的錄音工具也要同步改。

原廠自己的註解證實了 pop 的存在（`module_audio.c:299`）：
```c
// disable the first frame to prevent "pop" sound
```

---

## 5. 參數是什麼意思

整條**類比訊號鏈**，每個常數管一段：

```
麥克風 ─▶ MICBST ─▶ ADC ─▶ 數位音量 ─▶ HPF ─▶ DMA page
          類比放大          數位乘法    去直流
          MIC_GAIN          ADC_VOL     HPF_FS_3
```

| 常數 | 管哪段 | 說明 |
|---|---|---|
| `AUDIO_CODEC_2p8V` | 供電 | codec 類比電源 |
| `MIC_SINGLE_EDNED` | 麥克風接法 | 單端（1 線）vs 差動（2 線）；板上麥克風是單端 |
| `OUTPUT_SINGLE_EDNED` | 喇叭接法 | 同上，輸出側；不用但 init 要填 |
| `MIC_20DB` | **類比**放大 | 在 ADC **之前** → 提高訊噪比 |
| `DVOL_ADC_0DB` (0x2F) | **數位**放大 | 在 ADC **之後** → 雜訊一起放大 |
| `HPF_FS_3` | 高通濾波 | 去 DC offset |

> **原則：先催類比（MIC_GAIN），數位（ADC_VOL）留 0dB。**
> 上次爆掉就是 `MIC_40DB` + `ADC 0x7F` 兩段同時開到最大。

---

## 6. 怎麼自己追一個常數（方法）

以 `MIC_20DB` 為例，四步：

```
1. grep enum 定義        → 真值多少
2. grep 函式名往下追     → hal_xxx 找不到就找 hal_rtl_xxx
3. 找到 si_read/si_write → 記下暫存器編號
4. grep 那個 MASK/SHIFT  → 註解裡有硬體真相
```

實際結果：

| 層 | 檔案 | 內容 |
|---|---|---|
| enum | `include/rtl8735b_audio.h:325` | `AUDIO_MIC_20DB = 0x1` ← **是索引不是 dB 數字** |
| mbed API | `mbed/targets/hal/rtl8735b/audio_api.c:332` | 純轉手；**init 沒做會靜默不動作** |
| hal | `source/ram/hal_audio.c:1136` | 跳 stub table 進 ROM |
| 真正寫暫存器 | `source/ram/rtl8735b_audio.c:2294` | read-modify-write codec index `0x03` |
| bitfield | `include/rtl8735b_audio_codec_type.h:92` | `MICBST_GSELL` bit[1:0]：`00:0dB 01:20dB 10:30dB 11:40dB` |

一行指令：
```bash
cd ameba-rtos-pro2/component/soc/8735b/fwlib/rtl8735b
grep -rn "MICBST_GSELL" .
```

`si` = **serial interface**。audio codec 是掛在 SoC 旁的獨立區塊，不是記憶體映射，
所以才有「codec 自己的暫存器編號 index 0x03」。

---

## 7. 踩過的坑（別再踩）

| 坑 | 現象 | 解法 |
|---|---|---|
| `PAGE_SIZE` 巨集撞名 | `"PAGE_SIZE" redefined` | 改名 `AUD_PAGE_SIZE`。**這個 core 的短名巨集都被佔了，一律加前綴** |
| `Serial.printf` 不存在 | `LOGUARTClass has no member printf` | 直接用裸 `printf()`，走 LOGUART |
| `%f` 印不出來 | 亂碼/空白 | rtl 精簡版 printf 浮點不可靠。改整數百分之一毫秒 + 自寫 `print_ms()` |
| `printf("#")` 逐字元 | 完全沒輸出 | 先組進 `char buf[70]` 再一次印 |
| 忘記 `dcache_invalidate_by_addr` | 讀到舊音訊，靜默錯誤 | ISR 第一行就要 invalidate。**RT584 沒 D-cache，AMB82 有** |
| 忘記 `audio_set_rx_page()` | 收幾頁就停 | ISR 結尾一定要還頁 |

---

## 8. 還沒解決 / 不確定

- **−4ms 固定開機偏移原因不明**（不影響取樣率正確性）
- 上次爆掉是 MIC gain / ADC vol / HPF **三個同時改**才修好，**不知道是哪一個起作用**（方法論錯誤，改一個變數就好）
- RT584 有一個未文件化的 enum 值（細節見本機筆記）→ 用前先問設計team

### RT584 時脈鏈

涉及私有內部 SDK，**不放進本 repo**。筆記在本機 `D:\workdir\584_doc\`。

---

## 9. 剩下的範圍

使用者設定的學習範圍：**I2S（進行中）→ SPI → ISP**

### 舊 backlog（未取消，降優先）
- 還原 `model_classification.c`（從 `.orig`）
- `CUSTOMIZED_IMGCLASS` 的 `nn_models.json` 缺陷補進 `realtek-email-B-bugreport.md`
- 還原 `_backup_amb82/` 原廠 `.nb`
- 刪掉 `_bp_yolo*` 舊目錄（180 MB）
- 縮小 PAT 權限範圍
- `AudioClassification` 範例還沒燒過

---

## 10. 注意事項

- **RT584 資料來自私有內部 SDK**（`Rafael-IoT-SDK-Internal`、`584_doc`）
  → **不可** commit 進 GitHub repo，**不可**送到任何外部服務
- repo 必須保持 **private**
- 公司電腦無管理員權限，不要觸發 UAC

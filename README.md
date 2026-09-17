# Ameba Edge AI (AmebaPro2 / AMB82-MINI)

在一塊有 NPU 的板子上，從零走完 **訓練 → 轉檔 → 燒錄 → 上線** 的完整紀錄。
附 Raspberry Pi Pico 2（純 CPU）作為對照組。

## 兩份文件，看你是哪一種讀者

| | 給誰 | 內容 |
|---|---|---|
| 👉 [**COURSE.md**](COURSE.md) | **沒做過的人** | 從「照片在電腦裡是什麼」開始，一路到燒錄。故事線、比喻、每章一張圖 |
| [REFERENCE.md](REFERENCE.md) | 已經在做的人 | 分區表、指令、實測數字、踩過的坑 |

第一次來 → COURSE.md。
想知道「給你一顆陌生的 NPU 該怎麼開始」→ REFERENCE.md **Part 0.5**。

## 這裡面有什麼

| 目錄 | 內容 |
|---|---|
| [`colab/`](colab/) | 教材 notebook：MNIST、猜拳、用自己相機重訓 |
| [`scripts/`](scripts/) | 轉檔與驗證腳本（含修好的量化流程）、notebook 產生器 |
| [`RpsGame/`](RpsGame/) | 猜拳 sketch（含 NPU 輸入張量診斷） |
| [`MnistNpuBench/`](MnistNpuBench/) | AMB82 NPU 跑分 |
| [`pico2/`](pico2/) | Pico 2 CPU 跑分（float32 / int8 / int8+CMSIS-NN） |
| [`tools/`](tools/) | PC 端 pygame 工具：對戰 + 收訓練資料 |
| [`docs/`](docs/) | 歷程原始紀錄（含已被推翻的結論，只當歷史看） |

## 一句話結論

同一個 MNIST 模型：**Pico 2 CPU 40 ms，AMB82 NPU 0.12 ms** ——
架構上快 99.2×，但端到端只快 13.4×，因為 96% 的時間在等作業系統的 tick。
細節與四層拆解見 [REFERENCE.md](REFERENCE.md) Part 5。

> ⚠ 這個 repo 必須維持 **private**：含原廠工具缺陷細節，
> 且授權工具包（acuity / NBInfo）不得散佈，不進 git。

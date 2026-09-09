/*
 * AMB82-MINI - AI 範例：音訊分類 (YAMNet)
 *
 * 為什麼選這個：沒有 SD 卡、也不需要 WiFi 就能跑。
 *   - ObjectDetectionImage 要讀 SD 卡上的 image_list.txt  -> 出局
 *   - 其他 NN 範例都要 WiFi.begin() 連網 (公司網路多為 802.1X，接不上) -> 出局
 *   - 這個原版有 #include "WiFi.h"，但全程沒呼叫任何 WiFi 函式，是殘留標頭，已移除
 *
 * 模型：variants/common_nn_models/yamnet_fp16.nb，由 Tools -> NN Model Load From: Flash 載入
 *
 * === 輸出為什麼跟原範例不一樣 ===
 * 原範例把每次 callback 裡「所有」通過的類別全印出來，而 AudioClassList.h 裡
 * 521 個類別的 filter 全部是 1(全開)，所以會嚴重洗版，根本看不出辨識對不對。
 * 這裡改成：
 *   1. 每次 callback 只取「分數最高」的那一個類別
 *   2. 分數低於 MIN_SCORE 視為沒把握，歸類為安靜
 *   3. 只有在類別「改變」時才印一行；同一個類別持續出現則每 REPEAT_MS 才補印一次
 * 這樣拍一次手就是一行 Clapping，安靜時不洗版。
 *
 * 板子設定：
 *   Tools -> NN Model Load From      : Flash
 *   Tools -> System: Multimedia logs : NN/OSD logs only
 *   Tools -> Port                    : CH340 那個 (編號會浮動，上傳前先確認)
 *
 * 上傳前務必：按住 BOOT -> 點 RESET -> 放開 BOOT -> 才按 Upload
 */

#include "StreamIO.h"
#include "NNAudioClassification.h"
#include "AudioClassList.h"

// 分數門檻：低於這個值就當作沒把握。想更靈敏就調低，想更安靜就調高。
#define MIN_SCORE  40
// 同一個類別持續出現時，每隔這麼久補印一次，讓你知道還活著
#define REPEAT_MS  3000

// NN 音訊分類固定要 16KHz
// 若完全沒反應或永遠只有 Silence，把 USE_AUDIO_AMIC 改成 USE_AUDIO_DMIC
AudioSetting configA(16000, 1, USE_AUDIO_AMIC);
Audio audio;
NNAudioClassification audioNN;
StreamIO audioStreamerNN(1, 1);

static int           lastClass   = -1;
static unsigned long lastPrintMs = 0;

void setup()
{
    Serial.begin(115200);
    Serial.println("=== AMB82-MINI audio classification ===");
    Serial.print("min score = ");
    Serial.println(MIN_SCORE);
    Serial.println("對著板子說話 / 拍手 / 敲桌子，只在類別改變時印一行");
    Serial.println("----------------------------------------");

    audio.configAudio(configA);
    audio.begin();

    audioNN.configAudio(configA);
    audioNN.setResultCallback(ACPostProcess);
    audioNN.modelSelect(AUDIO_CLASSIFICATION, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_YAMNET);
    audioNN.begin();

    audioStreamerNN.registerInput(audio);
    audioStreamerNN.registerOutput(audioNN);
    if (audioStreamerNN.begin() != 0) {
        Serial.println("StreamIO link start failed");
    }
}

void loop()
{
    // 全部靠 callback
}

void ACPostProcess(std::vector<AudioClassificationResult> results)
{
    int count = audioNN.getResultCount();

    // 1. 自己找分數最高的，不假設 results 已排序
    int bestClass = -1;
    int bestScore = -1;
    for (int i = 0; i < count; i++) {
        AudioClassificationResult item = results[i];
        int class_id = (int)item.classID();
        if (class_id < 0 || class_id >= 521) {
            continue;
        }
        if (!audioNames[class_id].filter) {
            continue;
        }
        int score = item.score();
        if (score > bestScore) {
            bestScore = score;
            bestClass = class_id;
        }
    }

    // 2. 分數不夠就當安靜
    if (bestScore < MIN_SCORE) {
        bestClass = -1;
    }

    // 3. 只在改變時印；沒變則每 REPEAT_MS 補印一次
    unsigned long now = millis();
    bool changed = (bestClass != lastClass);
    bool timeup  = (now - lastPrintMs >= REPEAT_MS);
    if (!changed && !timeup) {
        return;
    }

    if (bestClass < 0) {
        printf("[%7.1fs] ---- (安靜)\r\n", now / 1000.0);
    } else {
        printf("[%7.1fs] %-32s score %3d   (class %d)\r\n",
               now / 1000.0, audioNames[bestClass].audioName, bestScore, bestClass);
    }

    lastClass   = bestClass;
    lastPrintMs = now;
}

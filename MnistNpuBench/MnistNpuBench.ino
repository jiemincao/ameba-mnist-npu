/*
  MnistNpuBench —— AMB82-MINI 上量 MNIST 0~9 模型的 NPU 推論速度。

  刻意拿掉 WiFi / RTSP / OSD,管線只留:
      camera(CHANNELNN, RGB) --StreamIO--> NNImageClassification(VIP NPU) --callback--> Serial

  量的是「回呼到回呼」的間隔,也就是整條管線的 throughput(取影像 + resize + NPU + 後處理),
  不是純 NPU 時間。要把 NPU 那一段拆出來,方法是換模型跑第二輪 ——
  同樣的 camera 設定下,大模型與小模型的差值就是模型計算的部分。

  重要:如果印出來的平均間隔剛好卡在 1000/NNFPS ms,那就是被相機 fps 限住,
  不是 NPU 的極限,要把 NNFPS 調高再量。

  模型:mnist_cnn.nb (28x28x3 uint8 planar in / 10 類 fp16 out)
  燒錄前必須把它換成 tools/ameba_pro2_tools/1.4.7/img_class_cnn.nb
  (沒有 SD 卡時 DEFAULT_IMGCLASS 與 CUSTOMIZED_IMGCLASS 走的是同一條 flash 路徑)
*/

#include "StreamIO.h"
#include "VideoStream.h"
#include "NNImageClassification.h"

// 相機 RGB 通道的解析度。模型輸入是 28x28,img_resize_planar() 會從這個尺寸縮下去。
#define NNWIDTH  224
#define NNHEIGHT 224
// 刻意調高:範例給的 10 fps 會讓量到的是節流後的間隔而不是 NPU 的極限
#define NNFPS    30

#define CHANNELNN 3

// 我們的模型是 3 通道 RGB(而且訓練時 R=G=B),不要讓底層做 rgb2gray
#define IMAGERGB 1

#define NUM_CLASSES 10
#define REPORT_EVERY 100

VideoSetting configNN(NNWIDTH, NNHEIGHT, NNFPS, VIDEO_RGB, 0);
NNImageClassification imgclass;
StreamIO videoStreamerNN(1, 1);

// 回呼在 NN task 裡跑,和 loop() 不同 context,所以共用變數要標 volatile
volatile uint32_t g_frames = 0;
volatile uint32_t g_last_us = 0;
volatile uint32_t g_sum_us = 0;
volatile uint32_t g_min_us = 0xFFFFFFFF;
volatile uint32_t g_max_us = 0;
volatile int g_last_class = -1;
volatile int g_last_score = 0;

void ICBench(std::vector<ImageClassificationResult> results)
{
    uint32_t now = micros();

    if (imgclass.getResultCount() > 0) {
        g_last_class = (int)results[0].classID();
        g_last_score = (int)results[0].score();
    }

    if (g_last_us != 0) {
        uint32_t d = now - g_last_us;    // micros() 溢位時無號相減依然正確
        g_sum_us += d;
        if (d < g_min_us) {
            g_min_us = d;
        }
        if (d > g_max_us) {
            g_max_us = d;
        }
        g_frames++;
    }
    g_last_us = now;
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== MnistNpuBench ===");
    Serial.print("camera RGB channel : ");
    Serial.print(NNWIDTH);
    Serial.print("x");
    Serial.print(NNHEIGHT);
    Serial.print(" @ ");
    Serial.print(NNFPS);
    Serial.println(" fps");
    Serial.println("model input        : 28x28x3 uint8 planar");
    Serial.println("model output       : 10 classes fp16");

    Camera.configVideoChannel(CHANNELNN, configNN);
    Camera.videoInit();

    imgclass.configVideo(configNN);
    imgclass.configInputImageColor(IMAGERGB);
    imgclass.useModelMetaData(0);    // 我們的 .nb 沒有內嵌 metadata
    imgclass.setResultCallback(ICBench);
    imgclass.modelSelect(IMAGE_CLASSIFICATION, NA_MODEL, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_IMGCLASS);
    imgclass.begin();

    videoStreamerNN.registerInput(Camera.getStream(CHANNELNN));
    videoStreamerNN.setStackSize();
    videoStreamerNN.setTaskPriority();
    videoStreamerNN.registerOutput(imgclass);
    if (videoStreamerNN.begin() != 0) {
        Serial.println("StreamIO link start failed");
    }

    Camera.channelBegin(CHANNELNN);

    Serial.println("frames,avg_us,min_us,max_us,fps,top_class,score");
}

void loop()
{
    static uint32_t reported = 0;

    uint32_t n = g_frames;
    if (n >= reported + REPORT_EVERY) {
        // 取快照再算,避免中途被回呼改掉
        uint32_t sum = g_sum_us;
        uint32_t mn = g_min_us;
        uint32_t mx = g_max_us;
        uint32_t avg = (n > 0) ? (sum / n) : 0;

        Serial.print(n);
        Serial.print(",");
        Serial.print(avg);
        Serial.print(",");
        Serial.print(mn);
        Serial.print(",");
        Serial.print(mx);
        Serial.print(",");
        Serial.print((avg > 0) ? (1000000.0f / (float)avg) : 0.0f, 2);
        Serial.print(",");
        Serial.print(g_last_class);
        Serial.print(",");
        Serial.println(g_last_score);

        reported = n;
    }
    delay(200);
}

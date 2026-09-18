/*
  RpsGame -- 剪刀石頭布,用自己訓練的 CNN,由 PC 上的 tools/rps_gui.py 操控。

  模型:  colab/02_train_rps.ipynb 訓練 -> tflite -> acuity -> rps_cnn.nb (222 KB)
  輸入:  96x96x3 RGB
  輸出:  3 個機率  0=rock 1=paper 2=scissors

  模型怎麼進板子:
    rps_cnn.nb 已經複製成 tools/1.4.7/img_class_cnn.nb 和
    variants/common_nn_models/img_class_cnn.nb ——
    也就是我們「頂替」了原廠的 image classification 模型檔。
    按 Upload 時 IDE 看到下面的 DEFAULT_IMGCLASS,
    就會把 img_class_cnn.nb 打包成 NN_MDL/img_class.nb,
    燒進 flash 的 nn 分割區 (0x530000)。
    -> IDE 的 Tools > NN Model Load From 必須選 "Flash",不是 "SD Card"。

    為什麼不用 CUSTOMIZED_IMGCLASS?
    原廠 tools/1.4.7/misc/nn_models.json 裡:
      model_mappings["CUSTOMIZED_IMGCLASS"] = "img_class_cnn"
      但 nb_file_mapping 只有 key "img_class",沒有 "img_class_cnn"
    查表查不到 -> ino_validation 一定報
    "Model (.nb file) missing or customized model name mismatch"。
    這是原廠 JSON 的 bug,不管 .nb 放哪、叫什麼名字都過不了。

  ---------------------------------------------------------------- 通訊協定
  這一版韌體不自己跑遊戲,它是 PC 的「感測器 + 攝影機」。
  所有輸出都加 "RPS:" 前綴,因為原廠 libarduino.a 會一直吐
  "Image Classification tick[0]" 這種關不掉的 log,PC 端要靠前綴濾掉。

  PC -> 板子 (單一字元):
    G  開始一局   (倒數 3 秒 -> 讀結果 -> 拍照 -> 回報勝負)
    S  只拍一張   (收集訓練資料用)
    V  開始連續送預覽影像
    X  停止預覽
    T  送出模型真正吃到的 tensor (除錯用)
    P  ping

  板子 -> PC (每行一則):
    RPS:READY                       開機完成
    RPS:PONG                        回應 ping
    RPS:SCORE <r> <p> <s> <top>     三個機率(0~100)與目前最高分的類別,每 200ms
    RPS:CNT <n>                     倒數 3/2/1/0
    RPS:RESULT <you> <board> <verdict> <r> <p> <s>
                                    verdict 0=平手 1=你贏 2=你輸
    RPS:IMG <bytes>                 接下來是一張 JPEG,原始長度 bytes
    RPS:D <base64>                  影像資料,多行
    RPS:IMGEND
    RPS:TIMG <w> <h> <c> <bytes>    接下來是 planar 的模型輸入 tensor
    RPS:TEND
    RPS:ERR <訊息>

  ---------------------------------------------------------------- 速度
  115200 baud 下一張 320x240 的 JPEG 大約要 1 秒才傳得完,所以預覽只有 ~1 fps。
  要順的話把下面 SERIAL_BAUD 改成 921600,
  並且 tools/rps_gui.py 最上面的 BAUD 也要改成一樣的值。
  (開機那段原廠 log 固定在 115200,改完之後開機訊息會變亂碼,那是正常的。)
*/

#include "VideoStream.h"
#include "StreamIO.h"
#include "NNImageClassification.h"

#define SERIAL_BAUD 921600    // GUI 的 BAUD 必須一樣。開機那段原廠 log 固定 115200,
                              // 所以重開機時前幾行會是亂碼,那是正常的。

// ------------------------------------------------------------------ 相機設定
// channel 0 = group 0,channel 3 = group 1。
// VOE 不接受「只開 group 1」—— 第一版就是這樣掛的:
//   hal_video_open fail ret=88201c00, group=1
// 所以 channel 0 一定要開。這一版把它設成 JPEG snapshot 模式,
// 一來讓 group 0 活著,二來剛好拿來拍照給 PC 看。
// snapshot 模式不接 StreamIO,也就不會再洗 "CH 0 MMF ENC Queue full"。
#define CHANNEL_JPEG 0
#define CHANNELNN    3

#define JPEG_W 320
#define JPEG_H 240

// 這是「相機串流」的解析度,不是模型輸入尺寸。
// 模型要的 96x96 由 SDK 自己從 NBG 讀出來再縮。
#define NNWIDTH  224
#define NNHEIGHT 224

// 1 = RGB,0 = 灰階。官方巨集叫 IMAGERGB 但預設值 0 會去呼叫 img_rgb2gray(),
// 我們的模型吃 3 通道 RGB,必須是 1。
#define IMAGERGB 1

VideoSetting configJPEG(JPEG_W, JPEG_H, 10, VIDEO_JPEG, 1);    // 最後那個 1 = 開 snapshot
VideoSetting configNN(NNWIDTH, NNHEIGHT, 10, VIDEO_RGB, 0);
NNImageClassification imgclass;
StreamIO videoStreamerNN(1, 1);

// ------------------------------------------------------------------ 類別定義
// 順序必須跟訓練時的 CLASS_NAMES 一致:["rock", "paper", "scissors", "none"]
// none 排在最後,所以 0/1/2 還是原來的猜拳順序,下面那張勝負表不用動。
#define NUM_CLASSES 4
#define CLASS_NONE  3      // 「畫面裡沒有手」

// 最高分低於這個值就當作沒認出來。softmax 一定會挑一個最大的出來,
// 就算畫面裡是一杯咖啡它也會說「這是 rock,信心 34」。門檻是第二道防線,
// 第一道是上面那個 none 類別 —— 兩個都要有。
#define CONF_THRESHOLD 60  // 0~100

// 勝負表  RESULT[你出的][板子出的]  0=平手 1=你贏 2=你輸
const int RESULT[3][3] = {
    /* 你 ROCK     */ {0, 2, 1},
    /* 你 PAPER    */ {1, 0, 2},
    /* 你 SCISSORS */ {2, 1, 0},
};

// ------------------------------------------------- callback 跟 loop 之間的橋
// callback 在 NN 的 task 裡跑,loop 在另一個 task,所以用 volatile。
volatile int      g_score[NUM_CLASSES] = {0, 0, 0, 0};    // 0~100
volatile int      g_top = -1;
volatile uint32_t g_frames = 0;

void ICPostProcess(std::vector<ImageClassificationResult> results)
{
    int n = imgclass.getResultCount();
    if (n <= 0) {
        return;
    }

    int tmp[NUM_CLASSES] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        int id = results[i].classID();
        if (id >= 0 && id < NUM_CLASSES) {
            tmp[id] = results[i].score();
        }
    }
    for (int c = 0; c < NUM_CLASSES; c++) {
        g_score[c] = tmp[c];
    }

    int top = results[0].classID();    // results[0] 是分數最高的
    g_top = (top >= 0 && top < NUM_CLASSES) ? top : -1;
    g_frames++;
}

// ------------------------------------------------------------------ base64
// 一次編 192 bytes -> 256 個字元一行,前綴成本才夠低。
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 整行(含前綴與換行)一次 write 出去。分成 print + println 兩次呼叫的話,
// 中間那個空檔就是原廠 log task 插隊的機會。
static void sendBase64Line(const uint8_t *p, int n)
{
    char out[280];
    int  o = 0;
    int  i = 0;
    out[o++] = 'R'; out[o++] = 'P'; out[o++] = 'S'; out[o++] = ':';
    out[o++] = 'D'; out[o++] = ' ';
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
    }
    int rem = n - i;    // 0, 1 或 2
    if (rem == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o++] = '\r';
    out[o++] = '\n';
    Serial.write((const uint8_t *)out, o);
}

// 拍一張 JPEG,整張用 base64 送出去。
// 注意:Camera.getImage() 在影像回來之前會一直等,相機掛了它就不會回來。
static void sendSnapshot(void)
{
    // 原廠 libarduino.a 的 vipnn 每辨識一幀就 printf 一行 "Image Classification tick[]",
    // 關不掉。它跟我們在同一條 LOG UART 上,字元層級交錯 ——
    // 實測 46 行 base64 有 11 行被插壞(24%),連 "RPS:IMG <len>" 那行都被吃掉。
    // 短行如 RPS:SCORE 運氣好還能活,但一張圖要 65 行,不可能全身而退。
    // 解法:傳圖這半秒把餵給 NN 的 StreamIO 停掉,NN 不跑就不印。
    // 結果早在倒數結束時就存進 g_top 了,這段時間不需要繼續辨識。
    videoStreamerNN.pause();
    delay(150);    // 等在途的那一兩幀跑完、log 吐乾淨

    uint32_t addr = 0, len = 0;
    Camera.getImage(CHANNEL_JPEG, &addr, &len);
    if (addr == 0 || len == 0) {
        videoStreamerNN.resume();
        Serial.println("RPS:ERR snapshot failed");
        return;
    }

    char hdr[32];
    int  hn = snprintf(hdr, sizeof(hdr), "RPS:IMG %lu\r\n", (unsigned long)len);
    Serial.write((const uint8_t *)hdr, hn);

    const uint8_t *p = (const uint8_t *)addr;
    uint32_t       sent = 0;
    while (sent < len) {
        int chunk = (len - sent > 192) ? 192 : (int)(len - sent);
        sendBase64Line(p + sent, chunk);
        sent += chunk;
    }
    Serial.write((const uint8_t *)"RPS:IMGEND\r\n", 12);
    videoStreamerNN.resume();
}

// ------------------------------------------------- 除錯:模型真正看到的那張圖
// dbg_tensor 由我們改過的 SDK model_classification.c 填,
// 內容是 img_resize_planar() 之後、進 NPU 之前的最終 tensor。
// 排列是 planar:先整張 R,再整張 G,再整張 B。
extern unsigned char dbg_tensor[];
extern volatile int  dbg_tensor_w, dbg_tensor_h, dbg_tensor_c, dbg_tensor_len;

static void sendTensor(void)
{
    videoStreamerNN.pause();
    delay(150);

    int len = dbg_tensor_len;
    if (len <= 0) {
        videoStreamerNN.resume();
        Serial.println("RPS:ERR no tensor yet");
        return;
    }

    char hdr[64];
    int  hn = snprintf(hdr, sizeof(hdr), "RPS:TIMG %d %d %d %d\r\n",
                       dbg_tensor_w, dbg_tensor_h, dbg_tensor_c, len);
    Serial.write((const uint8_t *)hdr, hn);

    int sent = 0;
    while (sent < len) {
        int chunk = (len - sent > 192) ? 192 : (len - sent);
        sendBase64Line(dbg_tensor + sent, chunk);
        sent += chunk;
    }
    Serial.write((const uint8_t *)"RPS:TEND\r\n", 10);
    videoStreamerNN.resume();
}

static void sendScore(void)
{
    char line[64];
    snprintf(line, sizeof(line), "RPS:SCORE %d %d %d %d %d",
             g_score[0], g_score[1], g_score[2], g_score[3], g_top);
    Serial.println(line);
}

// ------------------------------------------------------------------ 一局
static void playRound(void)
{
    // 板子先決定要出什麼,再倒數 —— 這樣才沒有「看完你的手才決定」的嫌疑。
    int me = random(0, 3);

    for (int i = 3; i >= 1; i--) {
        Serial.print("RPS:CNT ");
        Serial.println(i);
        delay(600);
    }
    Serial.println("RPS:CNT 0");
    delay(250);    // 讓 callback 有時間跑到「出拳後」的畫面

    int you = g_top;
    int s0 = g_score[0], s1 = g_score[1];
    int s2 = g_score[2], s3 = g_score[3];

    sendSnapshot();    // 先送圖,GUI 才能把畫面跟判定對起來

    if (you < 0) {
        Serial.println("RPS:ERR no result");
        return;
    }
    // ★ you 可能是 3(none),而 RESULT 只有 3x3。
    //   不擋的話 RESULT[3][me] 會讀到陣列外面 —— 讀到什麼都有可能。
    if (you == CLASS_NONE) {
        Serial.println("RPS:ERR no hand");
        return;
    }
    if (g_score[you] < CONF_THRESHOLD) {
        Serial.println("RPS:ERR low confidence");
        return;
    }

    char line[80];
    snprintf(line, sizeof(line), "RPS:RESULT %d %d %d %d %d %d %d",
             you, me, RESULT[you][me], s0, s1, s2, s3);
    Serial.println(line);
}

// ------------------------------------------------------------------ setup
void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(1000);

    Camera.configVideoChannel(CHANNEL_JPEG, configJPEG);    // group 0,拍照用
    Camera.configVideoChannel(CHANNELNN, configNN);         // group 1,餵給 NN
    Camera.videoInit();

    imgclass.configVideo(configNN);
    imgclass.configInputImageColor(IMAGERGB);
    imgclass.useModelMetaData(0);    // 我們的 .nb 沒有內嵌類別名稱
    imgclass.setResultCallback(ICPostProcess);
    imgclass.modelSelect(IMAGE_CLASSIFICATION, NA_MODEL, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_IMGCLASS);
    imgclass.begin();

    videoStreamerNN.registerInput(Camera.getStream(CHANNELNN));
    videoStreamerNN.setStackSize();
    videoStreamerNN.setTaskPriority();
    videoStreamerNN.registerOutput(imgclass);
    if (videoStreamerNN.begin() != 0) {
        Serial.println("RPS:ERR StreamIO link start failed");
    }
    Camera.channelBegin(CHANNEL_JPEG);    // 先起 group 0
    Camera.channelBegin(CHANNELNN);       // 再起 group 1

    randomSeed(micros());
    Serial.println("RPS:READY");
}

// ------------------------------------------------------------------ loop
uint32_t last_score = 0;
bool     preview = false;

void loop()
{
    // ---- 收 PC 的指令 ----
    while (Serial.available() > 0) {
        int c = Serial.read();
        switch (c) {
            case 'G':
                preview = false;
                playRound();
                break;
            case 'S':
                sendSnapshot();
                break;
            case 'V':
                preview = true;
                break;
            case 'X':
                preview = false;
                break;
            case 'T':
                sendTensor();
                break;
            case 'P':
                Serial.println("RPS:PONG");
                break;
            default:
                break;    // '\r' '\n' 之類的忽略
        }
    }

    // ---- 定時回報機率 ----
    uint32_t now = millis();
    if (now - last_score >= 200) {
        last_score = now;
        if (g_frames > 0) {
            sendScore();
        }
    }

    // ---- 預覽 ----
    if (preview && g_frames > 0) {
        sendSnapshot();
    }

    delay(5);
}

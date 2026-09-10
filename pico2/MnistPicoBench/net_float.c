/*
  float32 的「純軟體、零優化」基準線。

  這一份存在的目的不是要跑快,而是要重現「還沒量化、還沒用 SIMD」時的數字。
  所以刻意寫成最直白的巢狀迴圈:
    - 不做 im2col、不重排資料
    - 不展開迴圈、不預先算索引
    - 每個 MAC 都是一次 float 乘加

  RP2350 的 Cortex-M33 是有單精度硬體 FPU 的(build 時的
  -march=armv8-m.main+fp+dsp 就是證據),所以這裡的 float 乘加是真的
  一條 VMLA,不是軟體模擬。也就是說 float 版之所以慢,不是因為「浮點很貴」,
  而是因為:
    - 一次只算一個元素,吃不到 SIMD(DSP 擴充的 SIMD 是整數的,幫不上 float)
    - 每個元素 4 bytes,記憶體流量是 int8 的四倍
    - 索引計算、邊界判斷全部留在最內層迴圈裡
  這正好是我們想量的東西 —— 「什麼都沒做」的起點。

  權重的排列直接沿用 Keras 的原始 layout(產生器沒有轉置):
    conv kernel : [KH][KW][C_IN][C_OUT]
    dense kernel: [C_IN][C_OUT]
  注意這跟 int8 那邊(TFLite 轉成 [C_OUT][...][C_IN])不一樣,索引要分開寫。
*/
#include <math.h>
#include "kernels.h"
#include "model/mnist_bench_input.h"
#include "model/mnist_model_float.h"

#define C1_OUT  16
#define C2_OUT  32
#define D1_OUT  64
#define FLAT    (7 * 7 * C2_OUT)

// 中間結果。float 版最大那塊是 conv1 的輸出 28*28*16*4 = 50KB,
// RP2350 有 520KB SRAM,放得下。
static float f_in[28 * 28 * BENCH_IN_C];
static float f_c1[28 * 28 * C1_OUT];
static float f_p1[14 * 14 * C1_OUT];
static float f_c2[14 * 14 * C2_OUT];
static float f_p2[7 * 7 * C2_OUT];
static float f_d1[D1_OUT];
static float f_logit[BENCH_NUM_CLASS];

// 3x3 same-padding 卷積 + ReLU。in/out 都是 NHWC。
static void conv3x3_f(const float *in, int iw, int ih, int ic,
                      const float *w, const float *b,
                      float *out, int oc_n, int relu)
{
    for (int oy = 0; oy < ih; oy++) {
        for (int ox = 0; ox < iw; ox++) {
            for (int oc = 0; oc < oc_n; oc++) {
                float acc = b[oc];
                for (int ky = 0; ky < 3; ky++) {
                    const int iy = oy - 1 + ky;
                    if (iy < 0 || iy >= ih) {
                        continue;               // same pad,補的是 0.0
                    }
                    for (int kx = 0; kx < 3; kx++) {
                        const int ix = ox - 1 + kx;
                        if (ix < 0 || ix >= iw) {
                            continue;
                        }
                        const float *ip = in + (iy * iw + ix) * ic;
                        // Keras layout: w[ky][kx][k][oc]
                        const float *wp = w + ((ky * 3 + kx) * ic) * oc_n + oc;
                        for (int k = 0; k < ic; k++) {
                            acc += ip[k] * wp[k * oc_n];
                        }
                    }
                }
                if (relu && acc < 0.0f) {
                    acc = 0.0f;
                }
                out[(oy * iw + ox) * oc_n + oc] = acc;
            }
        }
    }
}

static void maxpool2x2_f(const float *in, int iw, int ih, int ch, float *out)
{
    const int ow = iw / 2, oh = ih / 2;
    for (int oy = 0; oy < oh; oy++) {
        for (int ox = 0; ox < ow; ox++) {
            for (int c = 0; c < ch; c++) {
                float m = in[((oy * 2) * iw + ox * 2) * ch + c];
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        const float v =
                            in[((oy * 2 + dy) * iw + ox * 2 + dx) * ch + c];
                        if (v > m) {
                            m = v;
                        }
                    }
                }
                out[(oy * ow + ox) * ch + c] = m;
            }
        }
    }
}

// Keras dense kernel 是 [n_in][n_out]
static void dense_f(const float *in, int n_in, const float *w, const float *b,
                    float *out, int n_out, int relu)
{
    for (int o = 0; o < n_out; o++) {
        float acc = b[o];
        for (int i = 0; i < n_in; i++) {
            acc += in[i] * w[(size_t)i * n_out + o];
        }
        if (relu && acc < 0.0f) {
            acc = 0.0f;
        }
        out[o] = acc;
    }
}

int infer_float(int *prob_permille)
{
    // 輸入還原:g_input_int8 = pixel - 128,而訓練時的前處理是 pixel / 255。
    // 這樣兩條路徑吃到的是同一張圖的同一種縮放,比較才公平。
    for (int i = 0; i < 28 * 28 * BENCH_IN_C; i++) {
        f_in[i] = (float)((int)g_input_int8[i] + 128) / 255.0f;
    }

    conv3x3_f(f_in, 28, 28, BENCH_IN_C, f_conv2d_w, f_conv2d_b, f_c1, C1_OUT, 1);
    maxpool2x2_f(f_c1, 28, 28, C1_OUT, f_p1);
    conv3x3_f(f_p1, 14, 14, C1_OUT, f_conv2d_1_w, f_conv2d_1_b, f_c2, C2_OUT, 1);
    maxpool2x2_f(f_c2, 14, 14, C2_OUT, f_p2);
    dense_f(f_p2, FLAT, f_dense_w, f_dense_b, f_d1, D1_OUT, 1);
    dense_f(f_d1, D1_OUT, f_dense_1_w, f_dense_1_b, f_logit, BENCH_NUM_CLASS, 0);

    // softmax(減最大值是為了不讓 expf 溢位,這是標準做法)
    float mx = f_logit[0];
    for (int i = 1; i < BENCH_NUM_CLASS; i++) {
        if (f_logit[i] > mx) {
            mx = f_logit[i];
        }
    }
    float sum = 0.0f;
    float p[BENCH_NUM_CLASS];
    for (int i = 0; i < BENCH_NUM_CLASS; i++) {
        p[i] = expf(f_logit[i] - mx);
        sum += p[i];
    }

    int top = 0;
    for (int i = 0; i < BENCH_NUM_CLASS; i++) {
        prob_permille[i] = (int)(p[i] / sum * 1000.0f + 0.5f);
        if (p[i] > p[top]) {
            top = i;
        }
    }
    return top;
}

/*
  int8 推論的兩個版本:手寫 naive(參考實作) 與 CMSIS-NN。

  網路結構(跟 AMB82 跑的是同一個 .h5 轉出來的):

    input   28x28x3  int8, zp=-128, scale=1/255
    conv1   3x3, 16 ch, same pad, ReLU 融合   -> 28x28x16
    pool1   2x2 max, stride 2                 -> 14x14x16
    conv2   3x3, 32 ch, same pad, ReLU 融合   -> 14x14x32
    pool2   2x2 max, stride 2                 ->  7x7x32
    flatten                                   -> 1568
    dense1  1568 -> 64, ReLU
    dense2  64 -> 10                          (logits)
    softmax                                   -> int8, zp=-128, scale=1/256

  幾個關鍵的量化細節:

  1. ReLU 不需要另外做。conv1/conv2/dense1 的輸出 zero point 都是 -128,
     也就是「量化值 -128 代表實數 0」,所以 clamp 到 int8 下限就等於 ReLU。
     這不是巧合 —— TFLite 對 ReLU 輸出的校正範圍是 [0, max],
     所以 zp 必然落在 int8 的下限。

  2. maxpool 不改變 scale/zp(產生器裡有 assert 驗證過),所以 pool 不需要重量化。

  3. 重量化用的是 gemmlowp 那套定點乘法:
       out = round(acc * mult * 2^(shift-31)) + out_zp
     mult 是 Q0.31,shift 正數代表左移。CMSIS-NN 的 arm_nn_requantize()
     和 TFLite 的 MultiplyByQuantizedMultiplier 用的是同一個約定,
     所以產生器算出來的參數兩邊通用。

  4. softmax 兩個版本都呼叫 arm_softmax_s8。理由:softmax 只有 10 個元素,
     計算量可以忽略(佔不到千分之一),但 TFLite 的定點 exp 實作相當複雜,
     自己重寫一份只會增加出錯機會而量不到任何東西。
     所以「參考實作」指的是 conv / pool / dense 這些真正吃時間的層。
*/
#include <string.h>
#include "arm_nnfunctions.h"
#include "kernels.h"
#include "model/mnist_bench_input.h"
#include "model/mnist_model_int8.h"

#define C1_OUT  16
#define C2_OUT  32
#define D1_OUT  64
#define FLAT    (7 * 7 * C2_OUT)   // 1568

// 中間結果。跟 float 版比起來小很多(int8 一個元素 1 byte)。
static int8_t buf_c1[28 * 28 * C1_OUT];   // 12544
static int8_t buf_p1[14 * 14 * C1_OUT];   //  3136
static int8_t buf_c2[14 * 14 * C2_OUT];   //  6272
static int8_t buf_p2[7 * 7 * C2_OUT];     //  1568
static int8_t buf_d1[D1_OUT];
static int8_t buf_logit[BENCH_NUM_CLASS];
static int8_t buf_prob[BENCH_NUM_CLASS];

// CMSIS-NN 的暫存區(im2col 之類)。實際需求會在 setup 時查詢並檢查。
static int16_t cmsis_scratch[4096];

/* ------------------------------------------------------------------ */
/* 定點重量化 —— 手寫版本,語意跟 CMSIS-NN 的 arm_nn_requantize() 相同   */
/* ------------------------------------------------------------------ */
static inline int32_t requantize(int32_t acc, int32_t mult, int32_t shift)
{
    const int32_t left  = shift > 0 ? shift : 0;
    const int32_t right = shift > 0 ? 0 : -shift;

    // 先左移,再做 Q0.31 的 saturating rounding doubling high multiply。
    // 用 int64 是為了不在中間溢位;真正的 CMSIS-NN 在 M33 上會用 SMMULR 一條指令做完。
    int64_t v = (int64_t)acc << left;
    int64_t p = v * (int64_t)mult;
    int64_t nudge = (p >= 0) ? (1LL << 30) : (1LL - (1LL << 30));
    int32_t hi = (int32_t)((p + nudge) >> 31);

    // rounding right shift(往最近的整數,.5 往遠離零的方向)
    if (right > 0) {
        const int32_t mask = (1 << right) - 1;
        const int32_t rem = hi & mask;
        const int32_t thresh = (mask >> 1) + ((hi < 0) ? 1 : 0);
        hi = (hi >> right) + ((rem > thresh) ? 1 : 0);
    }
    return hi;
}

static inline int8_t sat8(int32_t v)
{
    if (v < -128) {
        return -128;
    }
    if (v > 127) {
        return 127;
    }
    return (int8_t)v;
}

/* ------------------------------------------------------------------ */
/* 手寫參考實作                                                        */
/* ------------------------------------------------------------------ */

// 3x3 same-padding 卷積。輸入/輸出都是 NHWC。
static void conv3x3_ref(const int8_t *in, int iw, int ih, int ic,
                        const int8_t *w, const int32_t *b,
                        const int32_t *mult, const int32_t *shift,
                        int32_t in_offset, int32_t out_zp,
                        int8_t *out, int oc_n)
{
    for (int oy = 0; oy < ih; oy++) {
        for (int ox = 0; ox < iw; ox++) {
            for (int oc = 0; oc < oc_n; oc++) {
                int32_t acc = b[oc];
                for (int ky = 0; ky < 3; ky++) {
                    const int iy = oy - 1 + ky;      // same pad -> 上下各補 1
                    if (iy < 0 || iy >= ih) {
                        continue;                     // padding 的值等於 zero point,
                    }                                 // 加上 in_offset 後是 0,可以直接跳過
                    for (int kx = 0; kx < 3; kx++) {
                        const int ix = ox - 1 + kx;
                        if (ix < 0 || ix >= iw) {
                            continue;
                        }
                        const int8_t *ip = in + (iy * iw + ix) * ic;
                        const int8_t *wp = w + ((oc * 3 + ky) * 3 + kx) * ic;
                        for (int k = 0; k < ic; k++) {
                            acc += ((int32_t)ip[k] + in_offset) * (int32_t)wp[k];
                        }
                    }
                }
                acc = requantize(acc, mult[oc], shift[oc]) + out_zp;
                out[(oy * iw + ox) * oc_n + oc] = sat8(acc);
            }
        }
    }
}

// 2x2 stride 2 max pool。int8 直接比大小即可(單調),不必反量化。
static void maxpool2x2_ref(const int8_t *in, int iw, int ih, int ch, int8_t *out)
{
    const int ow = iw / 2, oh = ih / 2;
    for (int oy = 0; oy < oh; oy++) {
        for (int ox = 0; ox < ow; ox++) {
            for (int c = 0; c < ch; c++) {
                int8_t m = in[((oy * 2) * iw + ox * 2) * ch + c];
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        const int8_t v = in[((oy * 2 + dy) * iw + ox * 2 + dx) * ch + c];
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

// 權重是 [out][in] row-major,跟 CMSIS-NN 的期待一致
static void dense_ref(const int8_t *in, int n_in,
                      const int8_t *w, const int32_t *b,
                      int32_t mult, int32_t shift,
                      int32_t in_offset, int32_t out_zp,
                      int8_t *out, int n_out)
{
    for (int o = 0; o < n_out; o++) {
        int32_t acc = b[o];
        const int8_t *wp = w + (size_t)o * n_in;
        for (int i = 0; i < n_in; i++) {
            acc += ((int32_t)in[i] + in_offset) * (int32_t)wp[i];
        }
        acc = requantize(acc, mult, shift) + out_zp;
        out[o] = sat8(acc);
    }
}

int infer_int8_ref(int *prob_permille, int8_t *q_out)
{
    conv3x3_ref(g_input_int8, 28, 28, BENCH_IN_C,
                conv1_w, conv1_b, conv1_mult, conv1_shift,
                -ZP_INPUT, ZP_CONV1, buf_c1, C1_OUT);
    maxpool2x2_ref(buf_c1, 28, 28, C1_OUT, buf_p1);
    conv3x3_ref(buf_p1, 14, 14, C1_OUT,
                conv2_w, conv2_b, conv2_mult, conv2_shift,
                -ZP_CONV1, ZP_CONV2, buf_c2, C2_OUT);
    maxpool2x2_ref(buf_c2, 14, 14, C2_OUT, buf_p2);
    // flatten 是 no-op:pool2 已經是 NHWC 連續排列,而 TFLite 的 reshape
    // 也是照同樣順序攤平,所以 dense 的權重欄位順序直接對上。
    dense_ref(buf_p2, FLAT, dense1_w, dense1_b,
              dense1_mult[0], dense1_shift[0],
              -ZP_CONV2, ZP_DENSE1, buf_d1, D1_OUT);
    dense_ref(buf_d1, D1_OUT, dense2_w, dense2_b,
              dense2_mult[0], dense2_shift[0],
              -ZP_DENSE1, ZP_LOGIT, buf_logit, BENCH_NUM_CLASS);
    arm_softmax_s8(buf_logit, 1, BENCH_NUM_CLASS,
                   SM_MULT, SM_SHIFT, SM_DIFF_MIN, buf_prob);

    int top = 0;
    for (int i = 0; i < BENCH_NUM_CLASS; i++) {
        // softmax 輸出固定 zp=-128、scale=1/256 -> 機率 = (q + 128) / 256
        prob_permille[i] = ((int)buf_prob[i] + 128) * 1000 / 256;
        q_out[i] = buf_prob[i];
        if (buf_prob[i] > buf_prob[top]) {
            top = i;
        }
    }
    return top;
}

/* ------------------------------------------------------------------ */
/* CMSIS-NN                                                           */
/* ------------------------------------------------------------------ */

int cmsisnn_scratch_needed(void)
{
    cmsis_nn_conv_params cp;
    cmsis_nn_dims i1 = {1, 28, 28, BENCH_IN_C}, f1 = {C1_OUT, 3, 3, BENCH_IN_C};
    cmsis_nn_dims o1 = {1, 28, 28, C1_OUT};
    cmsis_nn_dims i2 = {1, 14, 14, C1_OUT}, f2 = {C2_OUT, 3, 3, C1_OUT};
    cmsis_nn_dims o2 = {1, 14, 14, C2_OUT};
    cmsis_nn_dims fd1 = {FLAT, 1, 1, D1_OUT}, fd2 = {D1_OUT, 1, 1, BENCH_NUM_CLASS};

    memset(&cp, 0, sizeof(cp));
    cp.stride.w = 1;
    cp.stride.h = 1;
    cp.padding.w = 1;
    cp.padding.h = 1;
    cp.dilation.w = 1;
    cp.dilation.h = 1;
    cp.activation.min = -128;
    cp.activation.max = 127;

    int32_t n = arm_convolve_wrapper_s8_get_buffer_size(&cp, &i1, &f1, &o1);
    int32_t m = arm_convolve_wrapper_s8_get_buffer_size(&cp, &i2, &f2, &o2);
    int32_t a = arm_fully_connected_s8_get_buffer_size(&fd1);
    int32_t b = arm_fully_connected_s8_get_buffer_size(&fd2);
    if (m > n) {
        n = m;
    }
    if (a > n) {
        n = a;
    }
    if (b > n) {
        n = b;
    }
    return (int)n;
}

int infer_cmsisnn(int *prob_permille, int8_t *q_out)
{
    cmsis_nn_context ctx = {cmsis_scratch, (int32_t)sizeof(cmsis_scratch)};

    cmsis_nn_conv_params cp;
    memset(&cp, 0, sizeof(cp));
    cp.stride.w = 1;
    cp.stride.h = 1;
    cp.padding.w = 1;       // same padding,3x3 -> 各邊補 1
    cp.padding.h = 1;
    cp.dilation.w = 1;
    cp.dilation.h = 1;
    cp.activation.min = -128;   // = ReLU,因為輸出 zp 就是 -128
    cp.activation.max = 127;

    cmsis_nn_pool_params pp;
    memset(&pp, 0, sizeof(pp));
    pp.stride.w = 2;
    pp.stride.h = 2;
    pp.padding.w = 0;
    pp.padding.h = 0;
    pp.activation.min = -128;
    pp.activation.max = 127;

    cmsis_nn_dims pool_f = {1, 2, 2, 1};

    /* conv1: 28x28x3 -> 28x28x16 */
    {
        cmsis_nn_dims id = {1, 28, 28, BENCH_IN_C};
        cmsis_nn_dims fd = {C1_OUT, 3, 3, BENCH_IN_C};   // [C_OUT, KH, KW, C_IN]
        cmsis_nn_dims bd = {1, 1, 1, C1_OUT};
        cmsis_nn_dims od = {1, 28, 28, C1_OUT};
        cmsis_nn_per_channel_quant_params q = {(int32_t *)conv1_mult,
                                               (int32_t *)conv1_shift};
        cp.input_offset = -ZP_INPUT;
        cp.output_offset = ZP_CONV1;
        arm_convolve_wrapper_s8(&ctx, &cp, &q, &id, g_input_int8,
                                &fd, conv1_w, &bd, conv1_b, &od, buf_c1);
    }

    /* pool1: 28x28x16 -> 14x14x16 */
    {
        cmsis_nn_dims id = {1, 28, 28, C1_OUT};
        cmsis_nn_dims od = {1, 14, 14, C1_OUT};
        arm_max_pool_s8(&ctx, &pp, &id, buf_c1, &pool_f, &od, buf_p1);
    }

    /* conv2: 14x14x16 -> 14x14x32 */
    {
        cmsis_nn_dims id = {1, 14, 14, C1_OUT};
        cmsis_nn_dims fd = {C2_OUT, 3, 3, C1_OUT};
        cmsis_nn_dims bd = {1, 1, 1, C2_OUT};
        cmsis_nn_dims od = {1, 14, 14, C2_OUT};
        cmsis_nn_per_channel_quant_params q = {(int32_t *)conv2_mult,
                                               (int32_t *)conv2_shift};
        cp.input_offset = -ZP_CONV1;
        cp.output_offset = ZP_CONV2;
        arm_convolve_wrapper_s8(&ctx, &cp, &q, &id, buf_p1,
                                &fd, conv2_w, &bd, conv2_b, &od, buf_c2);
    }

    /* pool2: 14x14x32 -> 7x7x32 */
    {
        cmsis_nn_dims id = {1, 14, 14, C2_OUT};
        cmsis_nn_dims od = {1, 7, 7, C2_OUT};
        arm_max_pool_s8(&ctx, &pp, &id, buf_c2, &pool_f, &od, buf_p2);
    }

    /* dense1: 1568 -> 64 */
    {
        cmsis_nn_fc_params fp;
        cmsis_nn_dims id = {1, 1, 1, FLAT};
        cmsis_nn_dims fd = {FLAT, 1, 1, D1_OUT};     // [N=累加深度, C=輸出深度]
        cmsis_nn_dims bd = {1, 1, 1, D1_OUT};
        cmsis_nn_dims od = {1, 1, 1, D1_OUT};
        cmsis_nn_per_tensor_quant_params q = {dense1_mult[0], dense1_shift[0]};
        memset(&fp, 0, sizeof(fp));
        fp.input_offset = -ZP_CONV2;
        fp.filter_offset = 0;        // int8 對稱量化,權重 zp 一定是 0
        fp.output_offset = ZP_DENSE1;
        fp.activation.min = -128;
        fp.activation.max = 127;
        arm_fully_connected_s8(&ctx, &fp, &q, &id, buf_p2,
                               &fd, dense1_w, &bd, dense1_b, &od, buf_d1);
    }

    /* dense2: 64 -> 10 (logits,沒有 activation) */
    {
        cmsis_nn_fc_params fp;
        cmsis_nn_dims id = {1, 1, 1, D1_OUT};
        cmsis_nn_dims fd = {D1_OUT, 1, 1, BENCH_NUM_CLASS};
        cmsis_nn_dims bd = {1, 1, 1, BENCH_NUM_CLASS};
        cmsis_nn_dims od = {1, 1, 1, BENCH_NUM_CLASS};
        cmsis_nn_per_tensor_quant_params q = {dense2_mult[0], dense2_shift[0]};
        memset(&fp, 0, sizeof(fp));
        fp.input_offset = -ZP_DENSE1;
        fp.filter_offset = 0;
        fp.output_offset = ZP_LOGIT;
        fp.activation.min = -128;
        fp.activation.max = 127;
        arm_fully_connected_s8(&ctx, &fp, &q, &id, buf_d1,
                               &fd, dense2_w, &bd, dense2_b, &od, buf_logit);
    }

    arm_softmax_s8(buf_logit, 1, BENCH_NUM_CLASS,
                   SM_MULT, SM_SHIFT, SM_DIFF_MIN, buf_prob);

    int top = 0;
    for (int i = 0; i < BENCH_NUM_CLASS; i++) {
        prob_permille[i] = ((int)buf_prob[i] + 128) * 1000 / 256;
        q_out[i] = buf_prob[i];
        if (buf_prob[i] > buf_prob[top]) {
            top = i;
        }
    }
    return top;
}

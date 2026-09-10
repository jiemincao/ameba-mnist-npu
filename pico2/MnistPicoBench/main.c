/*
  Raspberry Pi Pico 2(RP2350 / Cortex-M33)MNIST 推論跑分。

  這支程式是 AMB82-MINI 那支 MnistNpuBench.ino 的對照組。
  兩邊刻意做成同一個形狀,好讓數字可以直接並排比:

      同一個 .h5 訓練出來的模型
      同一張圖(MNIST test 集第一張標籤 7 的)
      同樣的 warmup + 多次取平均 / min / max
      同樣印成 CSV,不用人工讀秒

  差別只有「誰來算」:AMB82 交給 VeriSilicon VIP NPU,這裡交給 CPU。

  一支程式跑三種模式,燒一次就拿到全部數字:

      FLOAT     float32 naive              —— 沒量化、沒 SIMD 的原始狀態
      INT8_REF  int8 naive                 —— 只加了量化
      CMSISNN   int8 + CMSIS-NN            —— 量化 + SIMD kernel

  這樣排是為了讓每一步只變一件事。直接拿 float naive 比 CMSIS-NN 會得到一個
  很大的加速倍數,但那個數字沒有解釋力 —— 分不出是量化的功勞還是 kernel 的功勞。

  兩個 int8 模式的輸出都會跟 g_expect_q 做逐 byte 比對。g_expect_q 是 PC 上
  TFLite 直譯器對同一張圖跑出來的 int8 輸出,所以這個比對不是「看起來對」,
  而是「跟參考實作一模一樣」。手寫版本和 CMSIS-NN 都必須 byte-exact 才算 PASS。

  計時用兩把尺:
      time_us_32()  微秒牆鐘,跟人的體感一致
      DWT CYCCNT    CPU cycle 數,可以跟 NPU 報的 total_cycle 同單位比較
  牆鐘會受 clock 設定影響,cycle 數不會,所以兩個都留著。
*/
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "kernels.h"
#include "model/mnist_bench_input.h"

// 三個模式的次數不一樣,因為速度差了兩三個數量級。
// float naive 一次就要百毫秒級,跑 100 次只是白等;CMSIS-NN 很快,
// 次數少了平均值會被單次抖動汙染。取樣數不同不影響平均值的可比性。
#define WARMUP_FLOAT   1
#define ITER_FLOAT     5
#define WARMUP_INT8    2
#define ITER_INT8      20
#define WARMUP_CMSIS   5
#define ITER_CMSIS     100

#define ROUNDS         3   // 整組重跑幾輪,看看數字穩不穩

/* ---------------- DWT cycle counter ---------------------------------- */
/*
  Cortex-M33 的 DWT(Data Watchpoint and Trace)裡有一個 32-bit 的
  CYCCNT,每個 CPU cycle +1。要用它得先做兩件事:
    1. CoreDebug->DEMCR 的 TRCENA 打開(不打開整個 DWT 都是死的)
    2. DWT->CTRL 的 CYCCNTENA 打開
  另外 M33 的 DWT 有 lock access register,某些實作要先寫魔術字解鎖。
  RP2350 上不需要,但寫了也無害,所以照寫,省得換板子時再踩一次。
*/
#define DEMCR       (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004u)
#define DWT_LAR     (*(volatile uint32_t *)0xE0001FB0u)
#define DEMCR_TRCENA      (1u << 24)
#define DWT_CTRL_CYCCNTENA (1u << 0)

static bool cyc_ok;

static void cyccnt_init(void)
{
    DEMCR |= DEMCR_TRCENA;
    DWT_LAR = 0xC5ACCE55u;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;

    // 實測它到底有沒有在動。有些晶片會把 DWT 拿掉,寄存器變成常數 0,
    // 那時候印出來的 cycle 數會是垃圾,不如老實說量不到。
    const uint32_t a = DWT_CYCCNT;
    for (volatile int i = 0; i < 50; i++) {
    }
    cyc_ok = (DWT_CYCCNT != a);
}

static inline uint32_t cyc_now(void)
{
    return cyc_ok ? DWT_CYCCNT : 0;
}

/* ---------------- 一個模式的量測 -------------------------------------- */

typedef enum { M_FLOAT, M_INT8REF, M_CMSISNN } bench_mode_t;

static const char *mode_name(bench_mode_t m)
{
    switch (m) {
    case M_FLOAT:    return "FLOAT";
    case M_INT8REF:  return "INT8_REF";
    default:         return "CMSISNN";
    }
}

static int run_once(bench_mode_t m, int *prob, int8_t *q)
{
    switch (m) {
    case M_FLOAT:    return infer_float(prob);
    case M_INT8REF:  return infer_int8_ref(prob, q);
    default:         return infer_cmsisnn(prob, q);
    }
}

static void bench(bench_mode_t m, int warmup, int iter, int round)
{
    int prob[BENCH_NUM_CLASS];
    int8_t q[BENCH_NUM_CLASS];
    int top = -1;

    for (int i = 0; i < warmup; i++) {
        top = run_once(m, prob, q);
    }

    uint32_t sum = 0, mn = 0xFFFFFFFFu, mx = 0, csum = 0;
    for (int i = 0; i < iter; i++) {
        const uint32_t c0 = cyc_now();
        const uint32_t t0 = time_us_32();
        top = run_once(m, prob, q);
        const uint32_t dt = time_us_32() - t0;
        const uint32_t dc = cyc_now() - c0;   // 32-bit 自然環繞,相減仍正確
        sum += dt;
        csum += dc;
        if (dt < mn) {
            mn = dt;
        }
        if (dt > mx) {
            mx = dt;
        }
    }

    const uint32_t avg = sum / iter;
    const uint32_t cavg = csum / iter;

    // 正確性:top-1 要對,int8 兩個模式還要跟 TFLite 逐 byte 相同
    int pass = (top == BENCH_LABEL);
    int qmismatch = -1;
    if (m != M_FLOAT) {
        for (int i = 0; i < BENCH_NUM_CLASS; i++) {
            if (q[i] != g_expect_q[i]) {
                pass = 0;
                if (qmismatch < 0) {
                    qmismatch = i;
                }
            }
        }
    }

    printf("%d,%s,%u,%u,%u,%u,%u,%.1f,%d,%d,%s\n",
           round, mode_name(m), iter, avg, mn, mx, cavg,
           avg ? 1000000.0 / (double)avg : 0.0,
           top, prob[top], pass ? "PASS" : "FAIL");

    if (qmismatch >= 0) {
        printf("  ! int8 輸出與 TFLite 不符,第一個不同的是 class %d: "
               "got %d, expect %d\n",
               qmismatch, (int)q[qmismatch], (int)g_expect_q[qmismatch]);
    }

    // 只在第一輪印完整的每類機率,後面幾輪不重複洗畫面
    if (round == 1) {
        printf("  class :");
        for (int i = 0; i < BENCH_NUM_CLASS; i++) {
            printf(" %4d", i);
        }
        printf("\n  this  :");
        for (int i = 0; i < BENCH_NUM_CLASS; i++) {
            printf(" %4d", prob[i]);
        }
        printf("\n  pc-ref:");
        for (int i = 0; i < BENCH_NUM_CLASS; i++) {
            printf(" %4d", g_ref_permille[i]);
        }
        printf("   (單位:千分比)\n");
    }
}

/* ---------------- main ------------------------------------------------ */

extern int cmsisnn_scratch_needed(void);

int main(void)
{
    stdio_init_all();

    // USB CDC 要等主機把 port 打開,不然開頭幾行會不見。
    // UART 沒這問題,但兩個都開著時等一下比較保險。
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
        sleep_ms(100);
    }
    sleep_ms(300);

    cyccnt_init();

    printf("\n==== MNIST inference bench on Pico 2 (RP2350 Cortex-M33) ====\n");
    printf("sys clock      : %u Hz\n", (unsigned)clock_get_hz(clk_sys));
    printf("DWT CYCCNT     : %s\n", cyc_ok ? "可用" : "不可用(cycle 欄位無意義)");
    printf("image          : MNIST test, label = %d, %dx%dx%d\n",
           BENCH_LABEL, BENCH_IN_W, BENCH_IN_H, BENCH_IN_C);
    printf("CMSIS-NN 暫存需求: %d bytes\n", cmsisnn_scratch_needed());
    printf("iterations     : float %d / int8_ref %d / cmsisnn %d, %d rounds\n",
           ITER_FLOAT, ITER_INT8, ITER_CMSIS, ROUNDS);
    printf("\nround,mode,iter,avg_us,min_us,max_us,avg_cycles,fps,top1,prob_permille,result\n");

    for (int r = 1; r <= ROUNDS; r++) {
        bench(M_FLOAT,   WARMUP_FLOAT, ITER_FLOAT, r);
        bench(M_INT8REF, WARMUP_INT8,  ITER_INT8,  r);
        bench(M_CMSISNN, WARMUP_CMSIS, ITER_CMSIS, r);
    }

    printf("\n==== 跑完了 ====\n");
    while (true) {
        // 燈慢閃,表示程式沒掛掉,只是結果已經印完了
        sleep_ms(500);
    }
    return 0;
}

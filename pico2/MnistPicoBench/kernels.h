/*
  三種推論實作的共同介面。

  三個模式跑的是同一個模型、同一張圖,差別只在「用什麼算」:

    infer_float()    float32,手寫 naive 迴圈,沒有任何優化
    infer_int8_ref() int8,手寫 naive 迴圈,量化數學跟 TFLite 完全一致
    infer_cmsisnn()  int8,呼叫 CMSIS-NN(Cortex-M33 的 DSP/SIMD 指令)

  這樣設計是為了讓每一步只變一件事:
    float -> int8_ref  量的是「量化」帶來的加速
    int8_ref -> cmsisnn 量的是「SIMD kernel」帶來的加速
  如果直接拿 float naive 跟 CMSIS-NN 比,兩個因素混在一起就分不清了。
*/
#ifndef KERNELS_H_
#define KERNELS_H_

#include <stdint.h>

// 每個 infer 函式都把 10 類的機率寫進 prob_permille(千分比),
// 並回傳 top-1 的類別。int8 的兩個版本另外把原始 int8 輸出寫進 q_out,
// 用來跟 TFLite 直譯器的 g_expect_q 逐 byte 比對。
int infer_float(int *prob_permille);
int infer_int8_ref(int *prob_permille, int8_t *q_out);
int infer_cmsisnn(int *prob_permille, int8_t *q_out);

#endif

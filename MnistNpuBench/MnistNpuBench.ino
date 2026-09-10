/*
  MnistNpuBench —— AMB82-MINI (RTL8735B / VeriSilicon VIP NPU) 上量 MNIST 0~9 的純推論時間。

  不用相機、不用 mmf/StreamIO,直接呼叫 VIPLite 驅動:

      vip_init → vip_create_network → vip_prepare_network
              → [ vip_run_network ] xN  ← 只有這一段被計時
              → vip_finish_network

  為什麼不用相機:
    1. 相機會把速率壓在 fps 天花板下,量到的是相機而不是 NPU
    2. Pico 2 那邊沒有相機,是餵固定陣列 —— 兩邊餵同一張圖,比較才公平
    3. 輸入固定,結果可重現,而且能跟 PC 端的機率逐項對照

  兩種計時同時做:
    micros()  —— CPU 從外面量 vip_run_network() 的牆鐘時間(含驅動 overhead)
    profiling —— 硬體自己回報的 inference_time 與 total_cycle
                 (vip_query_network(VIP_NETWORK_PROP_PROFILING),單位 us)
    兩者的差就是驅動層的 overhead。

  模型與測試圖都編進程式(mnist_bench_data.h),不依賴 flash 的 NN_MDL 分割區。
*/

#include "vip_lite.h"
#include "mnist_bench_data.h"

/*
  NPU 的電源與時鐘 —— vip_init() 之前一定要開。

  沒開的話驅動去讀晶片 ID 會讀成 0x0,查不到對應的 feature database:
      npu[...] did not find feature database for 0x0
      npu[...] fail to context init status=-11
  正常應該讀到 0xad(VIP8000NANONI_PID0XAD)。

  透過 NNImageClassification 跑時,是 mmf 的 vipnn 模組替我們做掉的。
  那個模組只有 libarduino.a 的二進位、沒有原始碼,把 vipnn_hardware_init()
  反組譯出來就只有兩行:
      hal_sys_peripheral_en(38, 1)   // 38 = NN_SYS,rtl8735b_sys_ctrl.h:221
      hal_sys_set_clk(38, 0)         // sel 0 = 500 MHz
                                     // (SYS_NN_SRC_SEL: 0=500 1=400 2=250 MHz)
  直接自己呼叫這兩個 hal,就不必把整個 vipnn 模組(含 mmf、crypto 相依)拖進來。
*/
extern "C" void hal_sys_peripheral_en(uint8_t id, uint8_t en);
extern "C" void hal_sys_set_clk(uint8_t id, uint8_t sel_val);

#define NN_SYS_ID       38  // rtl8735b_sys_ctrl.h: NN_SYS

// SYS_NN_SRC_SEL(rtl8735b_syson_s_type.h:946): 0 = 500MHz, 1 = 400MHz, 2 = 250MHz
// 改這一行掃時鐘。這個實驗已經做完了,結論記在這裡免得將來重做:
//
//   NN_CLK_SEL=0 (500MHz)  total_cycle ~= 60,900
//   NN_CLK_SEL=2 (250MHz)  total_cycle ~= 59,206  (27 輪,範圍 59,096~59,346)
//
// 時鐘砍半而 cycle 數只降 2.8%、沒有變成兩倍 -> total_cycle 數的是 NPU 核心
// 自己的時鐘域,所以 60,900 / 500MHz = 121.8us 這個換算成立。
//
// 那 2.8% 的降幅本身也是「時鐘真的切了」的證據:DDR 固定在 533MHz
// (開機 log 的 ddr_freq = 533),不跟著 NN 時鐘走。記憶體等待的絕對時間固定,
// 換算成核心 cycle 時就會隨核心時鐘變慢而縮短 -> stall cycle 變少 -> 總 cycle 略降。
// 若時鐘根本沒切成功,兩組應該完全重疊而不是穩定差 2.8%。
//
// 保留:下面那行 printf 印的 "@ %d MHz" 只是把這個 #define 回顯出來,
// 不是暫存器回讀。要決定性證明,讀 SYSON_S_REG_SYS_NN_CTRL(offset 0x11C,
// 欄位 SYSON_S_MASK_SYS_NN_SRC_SEL = 0x3 << 3)回來看。
//
// 0 = 500MHz 是正常工作條件,跑分數字要用這組。
#define NN_CLK_SEL      0
#define NN_CLK_MHZ      (NN_CLK_SEL == 0 ? 500 : (NN_CLK_SEL == 1 ? 400 : 250))

#define ITER        100     // 每一輪跑幾次推論
#define WARMUP      5       // 前幾次不計入統計(cache / 首次執行效應)

static vip_network g_net = VIP_NULL;
static vip_buffer  g_in  = VIP_NULL;
static vip_buffer  g_out = VIP_NULL;
static void       *g_nbg_ram = NULL;    // NBG 要在 DRAM 裡,不是直接指 XIP flash

static uint32_t g_round = 0;

// 出錯就停住並印出狀態碼 —— 跑分程式一旦有一步失敗,後面的數字都沒意義
#define VIPCHK(expr)                                                    \
    do {                                                                \
        vip_status_e _s = (expr);                                       \
        if (_s != VIP_SUCCESS) {                                        \
            Serial.print("FAIL ");                                      \
            Serial.print(#expr);                                        \
            Serial.print(" status=");                                   \
            Serial.println((int)_s);                                    \
            while (1) { delay(1000); }                                  \
        }                                                               \
    } while (0)

// 依網路自己宣告的參數建 buffer,不要用寫死的數字 ——
// 這樣換模型時不必改 sketch,而且能順便印出來確認跟預期一致
static void make_buffer(bool is_input, vip_buffer *buf, const char *tag)
{
    vip_buffer_create_params_t p;
    memset(&p, 0, sizeof(p));

    if (is_input) {
        VIPCHK(vip_query_input(g_net, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &p.num_of_dims));
        VIPCHK(vip_query_input(g_net, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, p.sizes));
        VIPCHK(vip_query_input(g_net, 0, VIP_BUFFER_PROP_DATA_FORMAT, &p.data_format));
        VIPCHK(vip_query_input(g_net, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &p.quant_format));
    } else {
        VIPCHK(vip_query_output(g_net, 0, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &p.num_of_dims));
        VIPCHK(vip_query_output(g_net, 0, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, p.sizes));
        VIPCHK(vip_query_output(g_net, 0, VIP_BUFFER_PROP_DATA_FORMAT, &p.data_format));
        VIPCHK(vip_query_output(g_net, 0, VIP_BUFFER_PROP_QUANT_FORMAT, &p.quant_format));
    }

    Serial.print(tag);
    Serial.print(" dims=");
    Serial.print(p.num_of_dims);
    Serial.print(" sizes=");
    for (uint32_t i = 0; i < p.num_of_dims; i++) {
        Serial.print(p.sizes[i]);
        Serial.print(i + 1 < p.num_of_dims ? "x" : "");
    }
    Serial.print(" fmt=");
    Serial.print(p.data_format);      // 2 = UINT8, 1 = FP16
    Serial.print(" quant=");
    Serial.println(p.quant_format);   // 0 = none

    VIPCHK(vip_create_buffer(&p, sizeof(p), buf));
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== MnistNpuBench (direct VIPLite, no camera) ===");
    Serial.print("NBG size       : ");
    Serial.println(sizeof(g_nbg));
    Serial.print("test digit     : MNIST test[0], label = ");
    Serial.println(BENCH_LABEL);

    // 先開 NPU 電源與時鐘,再 vip_init()。順序反了就是 status=-11。
    hal_sys_peripheral_en(NN_SYS_ID, 1);
    hal_sys_set_clk(NN_SYS_ID, NN_CLK_SEL);
    Serial.print("NPU power+clk  : NN_SYS enabled @ ");
    Serial.print(NN_CLK_MHZ);
    Serial.println(" MHz");

    VIPCHK(vip_init());

    // NBG 複製到 DRAM。驅動在 prepare 階段會大量讀它,放在 XIP flash 上不保險。
    g_nbg_ram = malloc(sizeof(g_nbg));
    if (g_nbg_ram == NULL) {
        Serial.println("FAIL malloc NBG");
        while (1) { delay(1000); }
    }
    memcpy(g_nbg_ram, g_nbg, sizeof(g_nbg));

    VIPCHK(vip_create_network(g_nbg_ram, sizeof(g_nbg),
                              VIP_CREATE_NETWORK_FROM_MEMORY, &g_net));

    uint32_t in_cnt = 0, out_cnt = 0;
    VIPCHK(vip_query_network(g_net, VIP_NETWORK_PROP_INPUT_COUNT, &in_cnt));
    VIPCHK(vip_query_network(g_net, VIP_NETWORK_PROP_OUTPUT_COUNT, &out_cnt));
    Serial.print("input count    : ");
    Serial.println(in_cnt);
    Serial.print("output count   : ");
    Serial.println(out_cnt);

    make_buffer(true,  &g_in,  "input  0:");
    make_buffer(false, &g_out, "output 0:");

    // prepare 很慢(要配記憶體、產生並修補 command buffer),只做一次,不計時。
    //
    // 順序很重要:prepare 一定要在 set_input/set_output 之前。
    // 反過來寫的話驅動會直接擋掉:
    //     nbglk_set_input[3932], pls prepare network firstly
    //     fail to set network input 0, status=-9
    // 因為配置 tensor 記憶體是 prepare 這一步做的,還沒配好就沒有東西能綁 buffer。
    uint32_t t0 = micros();
    VIPCHK(vip_prepare_network(g_net));
    Serial.print("prepare took   : ");
    Serial.print(micros() - t0);
    Serial.println(" us (只做一次,不計入推論時間)");

    VIPCHK(vip_set_input(g_net, 0, g_in));
    VIPCHK(vip_set_output(g_net, 0, g_out));

    // 把測試圖寫進輸入 buffer。之後每次推論都用同一張,所以只需要 flush 一次。
    void *in_ptr = vip_map_buffer(g_in);
    if (in_ptr == NULL) {
        Serial.println("FAIL vip_map_buffer(in)");
        while (1) { delay(1000); }
    }
    memcpy(in_ptr, g_digit, sizeof(g_digit));
    VIPCHK(vip_flush_buffer(g_in, VIP_BUFFER_OPER_TYPE_FLUSH));

    Serial.println();
    Serial.println("round,iter,wall_avg_us,wall_min_us,wall_max_us,hw_avg_us,hw_cycles,fps,top1,top1_permille,ok");
}

void loop()
{
    uint32_t wall_sum = 0, wall_min = 0xFFFFFFFF, wall_max = 0;
    uint32_t hw_sum = 0, hw_cycles = 0, hw_n = 0;
    int counted = 0;

    for (int i = 0; i < ITER + WARMUP; i++) {
        uint32_t t0 = micros();
        VIPCHK(vip_run_network(g_net));
        uint32_t d = micros() - t0;

        if (i < WARMUP) {
            continue;
        }

        wall_sum += d;
        if (d < wall_min) {
            wall_min = d;
        }
        if (d > wall_max) {
            wall_max = d;
        }
        counted++;

        // 硬體自己回報的時間。驅動若沒編進 profiling 就會回非 VIP_SUCCESS,
        // 那種情況只是少一組數字,不該讓跑分停下來,所以這裡不用 VIPCHK。
        vip_inference_profile_t prof;
        memset(&prof, 0, sizeof(prof));
        if (vip_query_network(g_net, VIP_NETWORK_PROP_PROFILING, &prof) == VIP_SUCCESS) {
            hw_sum += prof.inference_time;
            hw_cycles = prof.total_cycle;
            hw_n++;
        }
    }

    // 讀輸出:FP16 的一維機率向量。CPU 讀之前要 invalidate cache。
    VIPCHK(vip_flush_buffer(g_out, VIP_BUFFER_OPER_TYPE_INVALIDATE));
    __fp16 *prob = (__fp16 *)vip_map_buffer(g_out);

    int top = 0;
    for (int i = 1; i < BENCH_NUM_CLASS; i++) {
        if ((float)prob[i] > (float)prob[top]) {
            top = i;
        }
    }
    int top_permille = (int)((float)prob[top] * 1000.0f + 0.5f);

    uint32_t wall_avg = wall_sum / counted;

    g_round++;
    Serial.print(g_round);
    Serial.print(",");
    Serial.print(counted);
    Serial.print(",");
    Serial.print(wall_avg);
    Serial.print(",");
    Serial.print(wall_min);
    Serial.print(",");
    Serial.print(wall_max);
    Serial.print(",");
    Serial.print(hw_n ? (hw_sum / hw_n) : 0);
    Serial.print(",");
    Serial.print(hw_cycles);
    Serial.print(",");
    Serial.print(1000000.0f / (float)wall_avg, 1);
    Serial.print(",");
    Serial.print(top);
    Serial.print(",");
    Serial.print(top_permille);
    Serial.print(",");
    Serial.println(top == BENCH_LABEL ? "PASS" : "FAIL");

    // 第一輪順便把 10 類機率全印出來,跟 PC 端逐項對照
    if (g_round == 1) {
        Serial.println("--- 裝置端 vs PC 端(千分比) ---");
        for (int i = 0; i < BENCH_NUM_CLASS; i++) {
            Serial.print("  class ");
            Serial.print(i);
            Serial.print(": npu=");
            Serial.print((int)((float)prob[i] * 1000.0f + 0.5f));
            Serial.print("  pc=");
            Serial.println(g_ref_permille[i]);
        }
        Serial.println("------------------------------");
    }

    delay(1000);
}

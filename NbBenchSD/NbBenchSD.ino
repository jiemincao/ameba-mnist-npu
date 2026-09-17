/*
  NbBenchSD -- 直接呼叫 VIPLite 跑分，量 NPU 利用率。

  跟 MnistNpuBench 的差別有兩個：
    1. 模型不編進程式，從 flash 的 nn 分割區拿
    2. 支援多輸入/多輸出 -- YOLO 有 3 個輸出頭才跑得動

  為什麼不用 SD、也不編進程式：
    * SD 卡被公司的可移除儲存裝置政策擋掉了
    * flash 分割表裡 fw1 (韌體) 只有 0x400000 = 4 MB，
      4.6 MB 的 YOLO 模型編進去根本放不下
    * 但 nn 分割區 (0x530000, 0xA90000 = 10.56 MB) 就是專門放模型的

  nor_pfw_get_address() 回傳模型在 flash 上的記憶體映射位址，
  可以直接餵給 vip_create_network，不用拷貝。

  目的是量 NPU 利用率，不是做物件偵測，所以：
    * 輸入餵固定圖樣(不是真圖)。NPU 的 cycle 數跟輸入「內容」無關，
      只跟網路「形狀」有關，所以拿垃圾當輸入量到的時間是對的。
    * 完全不做前處理/後處理/NMS -- 那些是 CPU 的事，不在量測範圍內。

  量出來的 total_cycle 可以直接跟 MnistNpuBench 的 60,900 比。
*/
#include "vip_lite.h"
#include "NNObjectDetection.h"

extern "C" {
#include "fwfs.h"
#include "nn_file_op.h"
}

// ---------------------------------------------------------------- 要跑的模型
// 路徑形式要跟 libarduino.a 裡用的一致：前面有 "NN_MDL/"。
// (用 strings 挑出來的，頭文件沒寫)
// 我們自己的 MNIST 模型在 flash 裡是 "NN_MDL/img_class.nb"。
#define NB_NAME     "NN_MDL/yolov7_tiny.nb"

// 1 = 一律拷貝到 RAM；0 = 拿得到 flash 位址就直接用。
// 目前 nor_pfw_get_address 回 0xFFFFFFFF(找不到)，所以只能拷貝。
#define FORCE_COPY_TO_RAM 1

#define ITER        10      // 每輪跑幾次推論(大模型很慢，不要設太大)
#define WARMUP      2

// NPU 電源與時鐘 -- vip_init() 之前一定要開，順序反了就是 status=-11。
// (SYS_NN_SRC_SEL: 0 = 500MHz, 1 = 400MHz, 2 = 250MHz)
extern "C" void hal_sys_peripheral_en(uint8_t id, uint8_t en);
extern "C" void hal_sys_set_clk(uint8_t id, uint8_t sel_val);
#define NN_SYS_ID       38
#define NN_CLK_SEL      0
#define NN_CLK_MHZ      (NN_CLK_SEL == 0 ? 500 : (NN_CLK_SEL == 1 ? 400 : 250))

#define MAX_IO          8   // YOLOv7-tiny 有 3 個輸出頭,8 個夠用

static vip_network g_net = VIP_NULL;
static vip_buffer  g_inbuf[MAX_IO];
static vip_buffer  g_outbuf[MAX_IO];
static uint32_t    g_in_cnt = 0, g_out_cnt = 0;
static void       *g_nbg = NULL;
static uint32_t    g_nbg_size = 0;
static uint32_t    g_round = 0;

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

static void die(const char *msg)
{
    Serial.print("FAIL ");
    Serial.println(msg);
    while (1) { delay(1000); }
}

// ---------------------------------------------------------------- NBG
// 只是為了讓建置流程把 yolov7_tiny.nb 打進 nn 分割區。
// prebuild 的 ino_validation 會掃 sketch，找到 modelSelect 才知道要打包哪個模型；
// 所以這兩行必須跟官方範例写法一致(全域物件、四個參數、在 setup 裡呼叫)。
// 呼叫它只是設幾個成員變數，不會碰到 NPU；真正的推論全走下面的 VIPLite。
NNObjectDetection ObjDet;

static void load_nbg(void)
{
    pfw_init();

    Serial.println("flash 分割表:");
    pfw_list(0);

    // 先拿檔案大小：開起來 -> seek 到尾 -> tell。
    void *fr = nn_f_open((char *)NB_NAME, M_NORMAL);
    if (fr == NULL) {
        Serial.print("nn_f_open(\"" NB_NAME "\") 失敗 -- ");
        Serial.println("模型沒被打進 nn 分割區?");
        die("nn_f_open");
    }
    nn_f_seek(fr, 0, SEEK_END);
    g_nbg_size = (uint32_t)nn_f_tell(fr);
    Serial.print("NBG            : " NB_NAME "  ");
    Serial.print(g_nbg_size);
    Serial.println(" bytes");

    // 探一下能不能直接拿 flash 位址（那樣就不用花 4.6 MB RAM）。
    // 0 跟 0xFFFFFFFF 都是失敗，實測 NOR 的映射位址長得像 0x085xxxxx。
    unsigned int a1 = nor_pfw_get_address(NB_NAME);
    unsigned int a2 = nor_pfw_get_address("yolov7_tiny.nb");
    Serial.print("nor addr probe : \"" NB_NAME "\" -> 0x");
    Serial.print(a1, HEX);
    Serial.print("   \"yolov7_tiny.nb\" -> 0x");
    Serial.println(a2, HEX);

    unsigned int addr = 0;
    if (a1 != 0 && a1 != 0xFFFFFFFFU) {
        addr = a1;
    } else if (a2 != 0 && a2 != 0xFFFFFFFFU) {
        addr = a2;
    }

    if (addr != 0 && !FORCE_COPY_TO_RAM) {
        Serial.println("-> 直接用 flash 位址，不拷貝");
        g_nbg = (void *)addr;
    } else {
        Serial.println("-> 拿不到可用的 flash 位址，改成拷貝到 RAM");
        g_nbg = malloc(g_nbg_size);
        if (g_nbg == NULL) {
            Serial.print("malloc ");
            Serial.print(g_nbg_size);
            Serial.println(" bytes 失敗 -- heap 不夠大");
            die("malloc NBG");
        }
        nn_f_seek(fr, 0, SEEK_SET);
        uint32_t t0 = millis();
        int got = nn_f_read(fr, g_nbg, (int)g_nbg_size);
        Serial.print("copy to RAM    : ");
        Serial.print(got);
        Serial.print(" bytes, ");
        Serial.print(millis() - t0);
        Serial.println(" ms");
        if (got < 0 || (uint32_t)got != g_nbg_size) {
            die("nn_f_read 讀不完");
        }
    }
    nn_f_close(fr);
}

// ---------------------------------------------------------------- buffer
// 依網路自己宣告的參數建 buffer,順便把形狀印出來 ——
// 這是我們唯一能事先知道「這個 .nb 期待什麼輸入」的方法。
static void make_buffer(bool is_input, uint32_t idx, vip_buffer *buf)
{
    vip_buffer_create_params_t p;
    memset(&p, 0, sizeof(p));

    if (is_input) {
        VIPCHK(vip_query_input(g_net, idx, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &p.num_of_dims));
        VIPCHK(vip_query_input(g_net, idx, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, p.sizes));
        VIPCHK(vip_query_input(g_net, idx, VIP_BUFFER_PROP_DATA_FORMAT, &p.data_format));
        VIPCHK(vip_query_input(g_net, idx, VIP_BUFFER_PROP_QUANT_FORMAT, &p.quant_format));
    } else {
        VIPCHK(vip_query_output(g_net, idx, VIP_BUFFER_PROP_NUM_OF_DIMENSION, &p.num_of_dims));
        VIPCHK(vip_query_output(g_net, idx, VIP_BUFFER_PROP_SIZES_OF_DIMENSION, p.sizes));
        VIPCHK(vip_query_output(g_net, idx, VIP_BUFFER_PROP_DATA_FORMAT, &p.data_format));
        VIPCHK(vip_query_output(g_net, idx, VIP_BUFFER_PROP_QUANT_FORMAT, &p.quant_format));
    }
    p.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;

    Serial.print(is_input ? "  input  " : "  output ");
    Serial.print(idx);
    Serial.print(" : dims=");
    Serial.print(p.num_of_dims);
    Serial.print(" [");
    uint32_t elems = 1;
    for (uint32_t i = 0; i < p.num_of_dims; i++) {
        Serial.print(p.sizes[i]);
        if (i + 1 < p.num_of_dims) {
            Serial.print(",");
        }
        elems *= p.sizes[i];
    }
    Serial.print("] fmt=");
    Serial.print(p.data_format);
    Serial.print(" quant=");
    Serial.print(p.quant_format);
    Serial.print(" elems=");
    Serial.println(elems);

    VIPCHK(vip_create_buffer(&p, sizeof(p), buf));
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== NbBenchSD (direct VIPLite, model from flash nn partition, no camera) ===");

    ObjDet.modelSelect(OBJECT_DETECTION, DEFAULT_YOLOV7TINY, NA_MODEL, NA_MODEL);
    load_nbg();

    hal_sys_peripheral_en(NN_SYS_ID, 1);
    hal_sys_set_clk(NN_SYS_ID, NN_CLK_SEL);
    Serial.print("NPU power+clk  : NN_SYS enabled @ ");
    Serial.print(NN_CLK_MHZ);
    Serial.println(" MHz");

    VIPCHK(vip_init());
    VIPCHK(vip_create_network(g_nbg, g_nbg_size,
                              VIP_CREATE_NETWORK_FROM_MEMORY, &g_net));

    VIPCHK(vip_query_network(g_net, VIP_NETWORK_PROP_INPUT_COUNT, &g_in_cnt));
    VIPCHK(vip_query_network(g_net, VIP_NETWORK_PROP_OUTPUT_COUNT, &g_out_cnt));
    Serial.print("input count    : ");
    Serial.println(g_in_cnt);
    Serial.print("output count   : ");
    Serial.println(g_out_cnt);
    if (g_in_cnt > MAX_IO || g_out_cnt > MAX_IO) {
        die("輸入或輸出數量超過 MAX_IO,把它調大");
    }

    for (uint32_t i = 0; i < g_in_cnt; i++) {
        make_buffer(true, i, &g_inbuf[i]);
    }
    for (uint32_t i = 0; i < g_out_cnt; i++) {
        make_buffer(false, i, &g_outbuf[i]);
    }

    // prepare 很慢(配記憶體、產生並修補 command buffer),只做一次,不計時。
    // 順序:一定要在 set_input/set_output 之前,否則 status=-9。
    uint32_t t0 = micros();
    VIPCHK(vip_prepare_network(g_net));
    Serial.print("prepare took   : ");
    Serial.print(micros() - t0);
    Serial.println(" us (只做一次,不計入推論時間)");

    for (uint32_t i = 0; i < g_in_cnt; i++) {
        VIPCHK(vip_set_input(g_net, i, g_inbuf[i]));
    }
    for (uint32_t i = 0; i < g_out_cnt; i++) {
        VIPCHK(vip_set_output(g_net, i, g_outbuf[i]));
    }

    // 餵固定圖樣。cycle 數跟輸入內容無關,所以這裡不需要真的圖。
    for (uint32_t i = 0; i < g_in_cnt; i++) {
        void *p = vip_map_buffer(g_inbuf[i]);
        if (p == NULL) {
            die("vip_map_buffer(in)");
        }
        uint32_t sz = vip_get_buffer_size(g_inbuf[i]);
        memset(p, 0x80, sz);
        Serial.print("  filled input ");
        Serial.print(i);
        Serial.print(" : ");
        Serial.print(sz);
        Serial.println(" bytes");
        VIPCHK(vip_flush_buffer(g_inbuf[i], VIP_BUFFER_OPER_TYPE_FLUSH));
    }

    Serial.println();
    Serial.println("round,iter,wall_avg_us,wall_min_us,wall_max_us,hw_cycles,hw_us,fps");
}

void loop()
{
    uint32_t wall_sum = 0, wall_min = 0xFFFFFFFF, wall_max = 0;
    uint32_t hw_cycles = 0;
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

        vip_inference_profile_t prof;
        memset(&prof, 0, sizeof(prof));
        if (vip_query_network(g_net, VIP_NETWORK_PROP_PROFILING, &prof) == VIP_SUCCESS) {
            hw_cycles = prof.total_cycle;
        }
    }

    uint32_t wall_avg = wall_sum / counted;
    // total_cycle 數的是 NPU 核心時鐘域(MnistNpuBench 用 500/250MHz 兩點驗過),
    // 所以除以核心時鐘就是硬體自己花的時間。
    uint32_t hw_us = hw_cycles / NN_CLK_MHZ;

    g_round++;
    Serial.print(g_round);          Serial.print(",");
    Serial.print(counted);          Serial.print(",");
    Serial.print(wall_avg);         Serial.print(",");
    Serial.print(wall_min);         Serial.print(",");
    Serial.print(wall_max);         Serial.print(",");
    Serial.print(hw_cycles);        Serial.print(",");
    Serial.print(hw_us);            Serial.print(",");
    Serial.println(wall_avg ? (1000000UL / wall_avg) : 0);

    delay(1000);
}

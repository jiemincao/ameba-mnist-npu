/*
 * I2sRingProbe — 看見 DMA 環形緩衝
 *
 *   1. 每頁到達的時間間隔  -> 驗證 20 ms 的算式
 *   2. 開頭每頁的振幅 / 直流 -> 量出 pop 要丟幾頁
 *   3. 之後的即時音量條      -> 確認麥克風真的在收
 *
 * 對照 RT584 的 i2s-mic/main.c:
 *   hosal_i2s_callback_register  ->  audio_rx_irq_handler
 *   seg_size / blk_size          ->  page_num / page_size
 *   自己算 offset                ->  callback 直接給 pbuf
 *   (無)                         ->  audio_set_rx_page  用完必須歸還
 *
 * 這塊板的三個坑:
 *   - 不能叫 PAGE_SIZE,會撞到 rtl8735b_flash_sec.h 的同名巨集
 *   - Serial 沒有 printf(),要用裸的 printf()
 *   - 不要用 %f,rtl printf 對浮點支援不保證,一律整數算
 *   - printf("#") 逐字元印不出來,要先組好字串再一次印
 */

#include "AudioStream.h"

extern "C" {
#include "hal_cache.h"
}

// ---- 環的三個數字 ---------------------------------------------------
#define AUD_PAGE_SIZE   640                // bytes,必須是 64 的倍數
#define AUD_PAGE_NUM    AUDIO_PNUM_4       // 硬體最多只能 4
#define N_PAGES         4
#define SAMPLE_RATE_KHZ 16
// 640 bytes / 2 = 320 samples / 16 kHz = 20 ms 一頁
// 環總長 = 4 x 20 ms = 80 ms  <- 這就是你的截止期限
// ---------------------------------------------------------------------

// ---- 增益。一次只改一個,才知道是誰造成的 ---------------------------
#define MIC_GAIN  MIC_20DB                 // MIC_0DB / 20DB / 30DB / 40DB
#define ADC_VOL   DVOL_ADC_0DB             // 0x2F = 0 dB
#define HPF_ON    1                        // 高通濾波:切掉直流
// ---------------------------------------------------------------------

// ---- 實測結果:第 10 頁 (200 ms) 才進底噪,所以錄音要丟掉這麼多 ------
#define DROP_PAGES 10
// ---------------------------------------------------------------------

#define SAMPLES_PER_PAGE (AUD_PAGE_SIZE / 2)
#define WARMUP_LOG       50                // 記錄前 50 頁 = 1000 ms

static audio_t g_obj;
static uint8_t g_tx[AUD_PAGE_SIZE * N_PAGES] __attribute__((aligned(0x20)));
static uint8_t g_rx[AUD_PAGE_SIZE * N_PAGES] __attribute__((aligned(0x20)));

volatile uint32_t g_pages = 0;
volatile uint16_t g_peak_now = 0;
volatile int16_t  g_dc_now = 0;
volatile uint16_t g_peak_log[WARMUP_LOG];
volatile int16_t  g_dc_log[WARMUP_LOG];

// 印「百分之一毫秒」,例如 2000 -> "20.00 ms"
static void print_ms(uint32_t hundredths)
{
    printf("%lu.%02lu ms", (unsigned long)(hundredths / 100),
           (unsigned long)(hundredths % 100));
}

// 先組好整條字串再一次印
static void print_bar(uint16_t peak)
{
    char buf[70];
    int bars = (int)((uint32_t)peak * 60 / 32768);
    if (bars > 60) { bars = 60; }
    if (bars < 1 && peak > 40) { bars = 1; }
    for (int b = 0; b < bars; b++) { buf[b] = '#'; }
    buf[bars] = '\0';
    printf("%s\r\n", buf);
}

// 跑在中斷裡。不要在這裡 printf。
static void rx_page_ready(uint32_t arg, uint8_t* pbuf)
{
    (void)arg;

    // ★ DMA 繞過 CPU 寫記憶體,快取裡可能是舊資料
    dcache_invalidate_by_addr((uint32_t*)pbuf, AUD_PAGE_SIZE);

    int16_t* s = (int16_t*)pbuf;
    uint16_t peak = 0;
    int32_t  sum = 0;
    for (int i = 0; i < SAMPLES_PER_PAGE; i++) {
        int32_t v = s[i];
        sum += v;
        if (v < 0) { v = -v; }
        if (v > peak) { peak = (uint16_t)v; }
    }

    g_peak_now = peak;
    g_dc_now = (int16_t)(sum / SAMPLES_PER_PAGE);
    if (g_pages < WARMUP_LOG) {
        g_peak_log[g_pages] = peak;
        g_dc_log[g_pages] = g_dc_now;
    }
    g_pages++;

    // ★ 用完一定要還,不還 DMA 就沒有空頁可寫,音訊會停
    audio_set_rx_page(&g_obj);
}

void setup()
{
    Serial.begin(115200);
    delay(2000);
    printf("\r\n=== I2sRingProbe ===\r\n");
    printf("page_size = %d bytes   page_num = %d\r\n", AUD_PAGE_SIZE, N_PAGES);

    printf("預期: 一頁 %d samples = ", SAMPLES_PER_PAGE);
    print_ms((uint32_t)SAMPLES_PER_PAGE * 100 / SAMPLE_RATE_KHZ);
    printf(",  整條環 = ");
    print_ms((uint32_t)SAMPLES_PER_PAGE * N_PAGES * 100 / SAMPLE_RATE_KHZ);
    printf("\r\n");

    // 順序抄自原廠 module_audio.c:1043~1096,不要調換
    audio_init(&g_obj, OUTPUT_SINGLE_EDNED, MIC_SINGLE_EDNED, AUDIO_CODEC_2p8V);
    audio_set_dma_buffer(&g_obj, g_tx, g_rx, AUD_PAGE_SIZE, AUD_PAGE_NUM);
    audio_adc_digital_vol(&g_obj, ADC_VOL);
    audio_mic_analog_gain(&g_obj, 1, MIC_GAIN);
    audio_adc_l_hpf(&g_obj, HPF_ON, HPF_FS_3);
    audio_rx_irq_handler(&g_obj, rx_page_ready, NULL);
    audio_set_param_adv(&g_obj, ASR_16KHZ, WL_16BIT, A_MONO, A_MONO);

    for (int i = 0; i < N_PAGES - 1; i++) {
        audio_set_rx_page(&g_obj);
    }

    printf("開始收音... (請保持安靜,這段在量開機雜訊)\r\n");
    uint32_t t0 = millis();
    audio_rx_start(&g_obj);

    while (g_pages < WARMUP_LOG) { delay(1); }
    uint32_t dt = millis() - t0;

    printf("\r\n實測: %d 頁花了 %lu ms  ->  一頁 ", WARMUP_LOG, (unsigned long)dt);
    print_ms(dt * 100 / WARMUP_LOG);
    printf("\r\n");

    printf("\r\npage   t(ms)    peak      DC   (滿刻度 32767)\r\n");
    for (int i = 0; i < WARMUP_LOG; i++) {
        printf("%4d  %5d  %7u  %6d  ", i,
               i * SAMPLES_PER_PAGE / SAMPLE_RATE_KHZ,
               g_peak_log[i], g_dc_log[i]);
        print_bar(g_peak_log[i]);
        if (i == DROP_PAGES - 1) {
            printf("---- 以上 %d 頁 (%d ms) 是開機 pop,錄音要丟掉 ----\r\n",
                   DROP_PAGES, DROP_PAGES * SAMPLES_PER_PAGE / SAMPLE_RATE_KHZ);
        }
    }
    printf("\r\n  peak 一直是 32767/32768 -> 削波,MIC_GAIN 調小\r\n");
    printf("  DC 離 0 很遠            -> 直流偏移,HPF 沒生效\r\n");
    printf("\r\n現在開始即時音量,對著板子講話:\r\n");
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last < 100) { return; }
    last = millis();

    uint16_t p = g_peak_now;
    printf("pages=%-8lu peak=%5u  DC=%5d  ",
           (unsigned long)g_pages, p, g_dc_now);
    print_bar(p);
}

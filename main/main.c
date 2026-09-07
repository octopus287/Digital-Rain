/* ============================================================
 * Digital Rain Ring —— 黑客帝国"代码雨"戒指
 * Matrix-style "digital rain" on a tiny OLED ring display
 *
 * 硬件 Hardware : ESP32-C3 开发板 + 0.42 寸 SSD1306 OLED (I2C 接口, 单色)
 *
 * 效果 Effect  :
 *   1. 代码雨: 10 列片假名字符不断从屏幕顶部落下(电影里的经典画面)
 *   2. 彩蛋  : 开场像打字机一样逐字打出 "Wake up, Neo...",
 *              之后每隔约 20 秒暂停下雨、重演一次
 *
 * 怎么跑起来 How to run:
 *   1. 安装 ESP-IDF v5.5, 进入本目录
 *   2. idf.py set-target esp32c3      (首次编译前指定芯片)
 *   3. idf.py build flash monitor     (编译、烧录、看日志一条龙)
 *
 * 屏幕会自动探测: 程序会挨个尝试常见的 I2C 引脚组合和地址,
 * 不用改代码就能适配大多数接线方式。
 * (这块板子实测为 SDA=GPIO5, SCL=GPIO6, 地址=0x3C)
 *
 * ★ 这块 0.42 寸小屏的两个特殊性(换其他屏时务必重新确认) ★
 *   1. 显存和屏幕方向是"横竖互换"的: 驱动芯片显存的"页"方向
 *      对应屏幕的竖直方向。可视区域只有 40 像素高(显存页 0~4),
 *      水平方向用显存第 32~95 列(SEG_BASE=32)。
 *   2. 玻璃边缘有一排"隐形像素"连着显存页 5~7: 往这几页写东西
 *      会在屏幕底部显示出来。所以程序始终把页 5~7 保持全黑。
 * ============================================================ */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"

static const char *TAG = "matrix_rain";

/* ---------------- 自动探测 OLED: 挨个尝试这些 引脚组合 和 I2C 地址 ---------------- */
static const int pin_pairs[][2] = {
    {5, 6}, {4, 5}, {6, 7}, {2, 3}, {3, 2},
    {8, 9}, {9, 10}, {0, 1}, {1, 0}, {7, 6},
};
#define PIN_PAIR_NUM  (sizeof(pin_pairs) / sizeof(pin_pairs[0]))
static const uint8_t probe_addrs[] = {0x3C, 0x3D};
#define PROBE_ADDR_NUM  (sizeof(probe_addrs) / sizeof(probe_addrs[0]))

static i2c_master_bus_handle_t s_bus = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

/* ---------------- 显存缓冲区 ----------------
 * SSD1306 驱动芯片把显存分成 8 个"页", 每页 8 像素高、128 像素宽。
 * vfb[页][列] 是整块显存的镜像: 先在这里画好, 再用 vfb_flush()
 * 一次性刷到屏幕。这块 0.42 寸屏只显示其中一小块, 偏移见 SEG_BASE */
#define SEG_BASE    32          /* 屏幕最左边一列对应显存的第 32 列 */
#define SCREEN_W    64
#define SCREEN_H    64
static uint8_t vfb[8][128];     /* [页][列] */

/* 把整块显存一次性刷到屏幕 */
static esp_err_t vfb_flush(void)
{
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 128, 64, vfb);
}

/* ---------------- ASCII 5x7 点阵字库 (0x20~0x7E) ----------------
 * 每个字符 5 个字节, 每个字节是一列像素: bit0 是最顶上一行 */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x41,0x51,0x32}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x41,0x41,0x7F,0x00,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x08,0x2A,0x1C,0x08},
};

/* ---------------- 片假名 5x7 点阵字库 (37 个字形, 格式同上面的 ASCII 字库) ----------------
 * 手工简化的片假名字形, 专门给代码雨用 —— 电影原片里落的就是这类字符 */
static const uint8_t kata[][5] = {
    /*ア*/ {0x61,0x19,0x07,0x09,0x09}, /*イ*/ {0x00,0x60,0x1E,0x01,0x00},
    /*ウ*/ {0x01,0x61,0x1D,0x03,0x01}, /*エ*/ {0x41,0x41,0x5D,0x41,0x41},
    /*オ*/ {0x41,0x31,0x0F,0x01,0x01}, /*カ*/ {0x41,0x31,0x0F,0x09,0x09},
    /*キ*/ {0x12,0x12,0x7F,0x12,0x12}, /*ク*/ {0x00,0x61,0x19,0x07,0x03},
    /*コ*/ {0x41,0x41,0x41,0x41,0x7F}, /*サ*/ {0x08,0x7F,0x08,0x7F,0x08},
    /*シ*/ {0x3C,0x60,0x40,0x42,0x41}, /*ス*/ {0x61,0x1F,0x07,0x19,0x21},
    /*セ*/ {0x09,0x69,0x1F,0x09,0x09}, /*ソ*/ {0x1C,0x32,0x22,0x21,0x20},
    /*タ*/ {0x70,0x09,0x09,0x0D,0x03}, /*チ*/ {0x08,0x78,0x0F,0x0A,0x0A},
    /*ツ*/ {0x30,0x40,0x40,0x46,0x41}, /*テ*/ {0x08,0x39,0x0F,0x09,0x09},
    /*ト*/ {0x40,0x30,0x10,0x7F,0x00}, /*ニ*/ {0x24,0x24,0x24,0x24,0x24},
    /*ノ*/ {0x40,0x30,0x0C,0x03,0x01}, /*ハ*/ {0x07,0x18,0x60,0x1C,0x03},
    /*ヒ*/ {0x1F,0x10,0x1E,0x3E,0x10}, /*フ*/ {0x01,0x01,0x61,0x19,0x07},
    /*ヘ*/ {0x10,0x08,0x04,0x0A,0x02}, /*ホ*/ {0x41,0x31,0x7F,0x31,0x41},
    /*ミ*/ {0x24,0x12,0x12,0x12,0x09}, /*ム*/ {0x41,0x36,0x09,0x06,0x01},
    /*メ*/ {0x21,0x12,0x0C,0x12,0x21}, /*モ*/ {0x68,0x68,0x1F,0x05,0x05},
    /*ヨ*/ {0x49,0x49,0x49,0x49,0x7F}, /*ラ*/ {0x60,0x41,0x41,0x41,0x7F},
    /*リ*/ {0x3E,0x00,0x01,0x7E,0x00}, /*ル*/ {0x3F,0x40,0x30,0x0C,0x03},
    /*ロ*/ {0x7F,0x41,0x41,0x41,0x7F}, /*ワ*/ {0x7F,0x41,0x01,0x3F,0x3F},
    /*ン*/ {0x40,0x30,0x09,0x06,0x01},
};
#define KATA_NUM  (sizeof(kata) / sizeof(kata[0]))

/* ---------------- 画点 / 画字 API ----------------
 * 这块屏"存数据是横的、看屏幕是竖的": 显存的页方向对应屏幕的
 * 竖直方向 y(0~39), 显存的列方向对应屏幕的水平方向 x(0~63)。
 * vfb_px() 负责把 (x,y) 换算成显存里的一个比特。
 * 越界的点一律不画 —— 这样显存页 5~7 永远全黑,
 * 压住玻璃边缘那排会漏出来的"隐形像素" */
#define DRAW_H  40              /* 屏幕实际可见高度: 40 像素 */
static inline void vfb_px(int x, int y, int on)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= DRAW_H) return;
    uint8_t *b = &vfb[y / 8][SEG_BASE + x];
    if (on) *b |= (1 << (y % 8));
    else    *b &= ~(1 << (y % 8));
}

/* 画一个 5x7 字形(传入字库里的 5 个列字节), 返回步进宽度 6 */
static int vfb_glyph(int x, int y, const uint8_t *f)
{
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            vfb_px(x + c, y + r, (f[c] >> r) & 1);
    return 6;
}

/* 画一个 5x7 ASCII 字符 */
static int vfb_char(int x, int y, char ch)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    return vfb_glyph(x, y, font5x7[ch - 0x20]);
}

__attribute__((unused)) static int vfb_str(int x, int y, const char *s)
{
    while (*s) x += vfb_char(x, y, *s++);
    return x;   /* 返回结束坐标，便于计算宽度 */
}

/* ---------------- 代码雨参数 ---------------- */
#define RAIN_FPS    15        /* 动画帧率: 越大越流畅, 也越耗电 */
#define NCOLS       10        /* 雨的列数: 10 列 x 6 像素 = 60, 屏宽 64 刚好放下 */
#define COL_PITCH   6         /* 列间距(像素) = 字宽 5 + 空隙 1 */
#define GLYPH_H     8         /* 行高(像素) = 字高 7 + 空隙 1 */
#define NROWS       5         /* 竖直方向能放 40/8 = 5 行字符 */

typedef struct {
    int8_t  head;   /* 雨滴"头"所在的字符行, 小于 0 表示还没落进屏幕 */
    int8_t  len;    /* 尾迹长度(几个字符行) */
    uint8_t spd;    /* 每隔几帧往下走一行(越小落得越快) */
    uint8_t cnt;    /* 帧计数器(配合 spd 用) */
    uint8_t wait;   /* 重生前等待的帧数 */
} drop_t;
static drop_t drops[NCOLS];
static uint8_t gchars[NCOLS][NROWS];  /* 每列每一行当前显示的片假名下标 */

/* 随机取一个片假名字形下标 */
static uint8_t rain_glyph(void)
{
    return (uint8_t)(esp_random() % KATA_NUM);
}

/* 生成一颗新雨滴: 随机起点、尾迹长度、速度; first=开机时的初始铺排 */
static void rain_respawn(int c, bool first)
{
    drops[c].head = -(int8_t)(esp_random() % (first ? 12 : 6)); /* 顶部随机错开 */
    drops[c].len  = 2 + (esp_random() % 3);                     /* 尾迹 2~4 行 */
    drops[c].spd  = 1 + (esp_random() % 3);                     /* 1~3 帧/行 */
    drops[c].cnt  = 0;
    drops[c].wait = first ? 0 : (esp_random() % 24);            /* 重生等 0~1.5s */
}

/* ---------------- 彩蛋: 打字机式打出 "Wake up, Neo..." ---------------- */
#define EGG_PERIOD_FRAMES  225    /* 每隔多少帧弹一次彩蛋(15FPS 下约 20 秒) */
#define EGG_TYPE_FRAMES    2      /* 出一个字后停几帧再出下一个(越小打字越快) */
#define EGG_HOLD_FRAMES    30     /* 全部打完后停留几帧(30 帧 = 2 秒) */
#define EGG_CHARS          14     /* 两行文字的总字符数 */
#define EGG_TOTAL_FRAMES   (EGG_CHARS * EGG_TYPE_FRAMES + EGG_HOLD_FRAMES)

/* 按打字机进度绘制: reveal = 目前已显示的字符数, 第一行打完接着打第二行 */
static void egg_draw(int reveal)
{
    static const char l1[] = "Wake up,";
    static const char l2[] = "Neo...";
    const int len1  = (int)(sizeof(l1) - 1);
    const int total = len1 + (int)(sizeof(l2) - 1);
    if (reveal > total) reveal = total;
    int n1 = reveal < len1 ? reveal : len1;
    int n2 = reveal - n1;

    memset(vfb, 0, sizeof(vfb));
    for (int i = 0; i < n1; i++) vfb_char(8 + i * 6, 12, l1[i]);
    for (int i = 0; i < n2; i++) vfb_char(14 + i * 6, 24, l2[i]);
    ESP_ERROR_CHECK(vfb_flush());
}

/* ---------------- 自动探测 OLED: 试出屏幕接在哪组引脚、用什么地址 ---------------- */
static bool probe_oled(uint8_t *ret_addr, int *ret_sda, int *ret_scl)
{
    for (int p = 0; p < PIN_PAIR_NUM; p++) {
        int sda = pin_pairs[p][0], scl = pin_pairs[p][1];
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = -1,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t bus;
        if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) continue;

        for (int a = 0; a < PROBE_ADDR_NUM; a++) {
            if (i2c_master_probe(bus, probe_addrs[a], 50) == ESP_OK) {
                *ret_addr = probe_addrs[a];
                *ret_sda = sda;
                *ret_scl = scl;
                s_bus = bus;    /* 保留总线句柄，避免反复创建 */
                return true;
            }
        }
        i2c_del_master_bus(bus);
    }
    return false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Digital Rain Ring · Matrix code rain ===");

    /* 1. 自动探测 OLED 接在哪 */
    uint8_t addr;
    int sda, scl;
    ESP_ERROR_CHECK(probe_oled(&addr, &sda, &scl) ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "找到 OLED: SDA=%d SCL=%d 地址=0x%02X", sda, scl, addr);

    /* 2. 初始化 SSD1306 屏幕驱动(用 ESP-IDF 自带的驱动) */
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .scl_speed_hz = 400 * 1000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &s_io));

    const esp_lcd_panel_ssd1306_config_t vendor_cfg = { .height = 64 };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
        .vendor_config = (void *)&vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* 3. 补发几条屏幕校准命令(通用驱动没照顾到这块屏的差异):
     *    0xDA 指定引脚排布; 0xA4 显示显存内容(而不是整屏全亮);
     *    0xA6 正常黑底白字(不反色) */
    esp_lcd_panel_io_tx_param(s_io, 0xDA, (uint8_t[]){0x12}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xA4, NULL, 0);
    esp_lcd_panel_io_tx_param(s_io, 0xA6, NULL, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    memset(vfb, 0, sizeof(vfb));
    ESP_ERROR_CHECK(vfb_flush());
    ESP_LOGI(TAG, "OLED 就绪");

    /* 4. 主循环: 代码雨 + 彩蛋循环
     *    开机先打字机式打出 "Wake up, Neo...", 然后开始下雨;
     *    之后每隔一段时间暂停下雨、再打一遍彩蛋, 如此往复。
     *    彩蛋期间雨滴的位置被冻结, 彩蛋结束后从原处继续下 */
    for (int c = 0; c < NCOLS; c++) rain_respawn(c, true);
    int egg_frames = EGG_TOTAL_FRAMES;
    bool egg_on = true;
    int egg_timer = EGG_PERIOD_FRAMES;
    ESP_LOGI(TAG, "Wake up, Neo... -> 代码雨启动 @%dFPS", RAIN_FPS);

    while (1) {
        if (egg_on) {
            /* 打字机进行中: 根据已过帧数算出该显示几个字, 每帧重绘 */
            int shown = (EGG_TOTAL_FRAMES - egg_frames) / EGG_TYPE_FRAMES + 1;
            egg_draw(shown);
            if (--egg_frames <= 0) {         /* 打完并停留完毕, 恢复下雨 */
                egg_on = false;
                egg_timer = EGG_PERIOD_FRAMES;
            }
            vTaskDelay(pdMS_TO_TICKS(1000 / RAIN_FPS));
            continue;
        }

        memset(vfb, 0, sizeof(vfb));

        for (int c = 0; c < NCOLS; c++) {
            drop_t *d = &drops[c];
            if (d->wait) { d->wait--; continue; }

            /* 往下走: 每 spd 帧走一行, 头部进入新行时换一个随机片假名 */
            if (++d->cnt >= d->spd) {
                d->cnt = 0;
                d->head++;
                if (d->head >= 0 && d->head < NROWS)
                    gchars[c][d->head] = rain_glyph();
                if (d->head - d->len >= NROWS) {  /* 整条雨丝落出屏幕, 重新生成 */
                    rain_respawn(c, false);
                    continue;
                }
            }

            /* 画出这一列: 头部 + 尾迹(尾迹字形保持不变, 才像连贯的雨丝) */
            int r0 = d->head - d->len + 1; if (r0 < 0)     r0 = 0;
            int r1 = d->head;              if (r1 >= NROWS) r1 = NROWS - 1;
            for (int r = r0; r <= r1; r++)
                vfb_glyph(c * COL_PITCH, r * GLYPH_H, kata[gchars[c][r]]);

            /* 偶尔随机改写一个字形, 制造雨中"字符闪烁"的效果 */
            if ((esp_random() & 7) == 0)
                gchars[c][esp_random() % NROWS] = rain_glyph();
        }
        ESP_ERROR_CHECK(vfb_flush());

        if (--egg_timer <= 0) {              /* 到点了: 定格下雨, 开始打彩蛋 */
            egg_on = true;
            egg_frames = EGG_TOTAL_FRAMES;
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / RAIN_FPS));
    }
}


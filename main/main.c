/* ============================================================
 * Digital Rain Ring —— 黑客帝国代码雨戒指
 * 硬件：ESP32-C3 + 0.42寸 SSD1306 OLED (I2C 单色)
 *
 * ★ 屏幕实测参数（阶段2/3标定结论，勿动）★
 *   - 探测：SDA=GPIO5, SCL=GPIO6, 地址=0x3C
 *   - 玻璃：72x40 可视窗口，横装
 *     物理横向 x(0~63) = SEG 方向, 基准 SEG_BASE=32
 *     物理纵向 y(0~39) = 页(COM)方向：页0在最顶, 页内bit0朝顶, 向下递增
 *     （页标定照片：顶=页0条纹 -> 黑 -> 亮(页2) -> 黑 -> 亮(页4)，页5~7不可见；
 *       但玻璃底部边窗像素会显示页5~7内容，故页5~7必须恒黑压住）
 *   - POR 默认 entire-on，init 后必须补发 0xA4/0xA6
 *
 * 阶段3：5x7 点阵字体 + 文字渲染，显示 "Wake up, Neo..."
 * 阶段4：代码雨动画（10列 ASCII 数字雨，15FPS 全帧重绘）
 * 阶段5：彩蛋循环（开场文字 + 每约15s 定格弹出 "Wake up, Neo..." 2s）
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

/* ---------------- I2C 探测参数 ---------------- */
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

/* ---------------- 竖屏帧缓冲（按 GDDRAM 原始布局） ---------------- */
#define SEG_BASE    32
#define SCREEN_W    64
#define SCREEN_H    64
static uint8_t vfb[8][128];     /* [页][SEG列] */

static esp_err_t vfb_flush(void)
{
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 128, 64, vfb);
}

/* ---------------- 5x7 字体（0x20~0x7E，bit0=字符顶行） ---------------- */
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

/* ---------------- 竖屏绘制 API ----------------
 * 实测（文字方向校准）：屏幕为横装
 *   物理横向 x(0~63) = SEG 方向 -> 列地址 SEG_BASE+x
 *   物理纵向 y(0~63) = 页(COM)方向 -> 页 y/8, 页内bit y%8
 * ★ 玻璃底部有一条"边窗像素"连到页5~7区域，写入即显示，
 *   因此内容限制在 DRAW_H=40（页0~4），页5~7恒黑压住边窗 */
#define DRAW_H  40
static inline void vfb_px(int x, int y, int on)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= DRAW_H) return;
    uint8_t *b = &vfb[y / 8][SEG_BASE + x];
    if (on) *b |= (1 << (y % 8));
    else    *b &= ~(1 << (y % 8));
}

/* 画一个 5x7 字符，返回步进宽度 6 */
static int vfb_char(int x, int y, char ch)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *f = font5x7[ch - 0x20];
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            vfb_px(x + c, y + r, (f[c] >> r) & 1);
    return 6;
}

__attribute__((unused)) static int vfb_str(int x, int y, const char *s)
{
    while (*s) x += vfb_char(x, y, *s++);
    return x;   /* 返回结束坐标，便于计算宽度 */
}

/* ---------------- 阶段4：代码雨参数 ---------------- */
#define RAIN_FPS    15        /* 目标帧率（约定 15~25FPS） */
#define NCOLS       10        /* 10列 x 6px = 60px，左右各留 2px */
#define COL_PITCH   6
#define GLYPH_H     8         /* 7px 字形 + 1px 行距 */
#define NROWS       5         /* 40px / 8px = 5 行字形 */

typedef struct {
    int8_t  head;   /* 头部字形行号，可为负(尚未入屏) */
    int8_t  len;    /* 尾迹长度(行) */
    uint8_t spd;    /* 每几帧前进一行 */
    uint8_t cnt;    /* 帧计数 */
    uint8_t wait;   /* 重生等待帧数 */
} drop_t;
static drop_t drops[NCOLS];
static char   gchars[NCOLS][NROWS];   /* 每列各屏幕行当前字符 */

/* 片假名暂无字库，用 数字/大写字母/符号 池代替 */
static char rain_char(void)
{
    static const char pool[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ<>+=*!?:";
    return pool[esp_random() % (sizeof(pool) - 1)];
}

static void rain_respawn(int c, bool first)
{
    drops[c].head = -(int8_t)(esp_random() % (first ? 12 : 6)); /* 顶部随机错开 */
    drops[c].len  = 2 + (esp_random() % 3);                     /* 尾迹 2~4 行 */
    drops[c].spd  = 1 + (esp_random() % 3);                     /* 1~3 帧/行 */
    drops[c].cnt  = 0;
    drops[c].wait = first ? 0 : (esp_random() % 24);            /* 重生等 0~1.5s */
}

/* ---------------- 阶段5：彩蛋循环 ---------------- */
#define EGG_PERIOD_FRAMES  225    /* 约15s @15FPS（含刷屏耗时实际约20s） */
#define EGG_SHOW_FRAMES    30     /* 显示 2s @15FPS */

static void egg_show(void)
{
    memset(vfb, 0, sizeof(vfb));
    vfb_str(8,  12, "Wake up,");            /* 宽 48px -> x 8~55 */
    vfb_str(14, 24, "Neo...");              /* 宽 36px -> x 14~49 */
    ESP_ERROR_CHECK(vfb_flush());
}

/* ---------------- I2C 探测 ---------------- */
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
    ESP_LOGI(TAG, "=== Digital Rain Ring · 阶段3：文字渲染 ===");

    /* 1. 探测板载 OLED */
    uint8_t addr;
    int sda, scl;
    ESP_ERROR_CHECK(probe_oled(&addr, &sda, &scl) ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "探测成功: SDA=%d SCL=%d 地址=0x%02X", sda, scl, addr);

    /* 2. 挂载 SSD1306 面板设备 */
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

    /* 3. 校准命令（内置驱动不覆盖这块屏的差异） */
    esp_lcd_panel_io_tx_param(s_io, 0xDA, (uint8_t[]){0x12}, 1);  /* alt COM 引脚 */
    esp_lcd_panel_io_tx_param(s_io, 0xA4, NULL, 0);               /* 关键：恢复显存显示 */
    esp_lcd_panel_io_tx_param(s_io, 0xA6, NULL, 0);               /* 正常（非反色） */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    memset(vfb, 0, sizeof(vfb));
    ESP_ERROR_CHECK(vfb_flush());
    ESP_LOGI(TAG, "OLED 就绪");

    /* 4. 阶段5：代码雨 + 彩蛋循环
     *    开场先弹 "Wake up, Neo..." 2s（电影式开场），随后开始下雨；
     *    之后每隔 EGG_PERIOD_FRAMES 帧定格弹出彩蛋 EGG_SHOW_FRAMES 帧。
     *    彩蛋期间雨滴状态冻结，恢复后从原位继续。 */
    for (int c = 0; c < NCOLS; c++) rain_respawn(c, true);
    int egg_timer = EGG_SHOW_FRAMES;
    bool egg_on = true;
    egg_show();
    ESP_LOGI(TAG, "Wake up, Neo... -> 代码雨启动 @%dFPS", RAIN_FPS);

    while (1) {
        if (egg_on) {
            if (--egg_timer <= 0) {          /* 彩蛋结束，恢复下雨 */
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

            /* 前进：每 spd 帧走一个字形行，进入新行时取一个随机字符 */
            if (++d->cnt >= d->spd) {
                d->cnt = 0;
                d->head++;
                if (d->head >= 0 && d->head < NROWS)
                    gchars[c][d->head] = rain_char();
                if (d->head - d->len >= NROWS) {  /* 尾迹完全离屏 -> 重生 */
                    rain_respawn(c, false);
                    continue;
                }
            }

            /* 绘制该列：尾迹内字符保持稳定，形成连贯雨丝 */
            int r0 = d->head - d->len + 1; if (r0 < 0)     r0 = 0;
            int r1 = d->head;              if (r1 >= NROWS) r1 = NROWS - 1;
            for (int r = r0; r <= r1; r++)
                vfb_char(c * COL_PITCH, r * GLYPH_H, gchars[c][r]);

            /* 闪烁：小概率随机改写一个字符 */
            if ((esp_random() & 7) == 0)
                gchars[c][esp_random() % NROWS] = rain_char();
        }
        ESP_ERROR_CHECK(vfb_flush());

        if (--egg_timer <= 0) {              /* 该弹彩蛋了 */
            egg_on = true;
            egg_timer = EGG_SHOW_FRAMES;
            egg_show();
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / RAIN_FPS));
    }
}


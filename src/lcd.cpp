// lcd.cpp — ST7789P3 (240x320) panel driver for the AI Passport build.
//
// Uses the ESP-IDF esp_lcd SPI panel stack that ships with the Arduino core.
// Only compiled for the `aipassport` PlatformIO environment.
//
// Bench notes for this exact glass (AI Passport, Arduino core 2.0.17 / IDF 4.4):
//   * SPI mode 3 is required - mode 0 accepts every transaction but the panel
//     stays grey and blank.
//   * The IDF 5.x st7789 driver also sends RAMCTRL (0xBD) {0x00, 0xE0}; the 4.4
//     driver does not, so it is sent here explicitly.
//   * The panel ships colour-inverted (INVON).

#include "lcd.h"

#ifdef ADBLOCK_AIPASSPORT

#include <Arduino.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "font8x8_basic.h"   // public-domain 8x8 bitmap font

#define AI_LCD_MOSI     9
#define AI_LCD_SCLK     8
#define AI_LCD_CS       1
#define AI_LCD_DC       20
#define AI_LCD_RST      (-1)      // tied to 3.3 V -> esp_lcd uses SWRESET
#define AI_LCD_BL       21
#define AI_LCD_PCLK_HZ  (80 * 1000 * 1000)
#define AI_LCD_SPI_MODE 3

#define AI_BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define AI_BL_LEDC_TIMER    LEDC_TIMER_0
#define AI_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define AI_BL_LEDC_RES      LEDC_TIMER_10_BIT
#define AI_BL_LEDC_FREQ_HZ  5000

// Largest single draw: a full-width line at text scale 3 (240 x 24 px).
#define STRIP_LINES 24

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t                 *s_buf;      // DMA-capable scratch strip
static bool                      s_ready;
static bool                      s_bl_ready;
static uint32_t                  s_wait_timeouts;   // diagnostics for the wake log
static SemaphoreHandle_t         s_lcd_idle; // set when a colour transfer finishes

// Colour data is queued asynchronously (DMA + interrupt), so the scratch strip
// must not be rewritten until the previous transfer is done.
static bool lcd_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
    BaseType_t hp = pdFALSE;
    if (s_lcd_idle) xSemaphoreGiveFromISR(s_lcd_idle, &hp);
    return hp == pdTRUE;
}

static void wait_idle(void) {
    if (!s_lcd_idle) return;
    // The timeout only guards against an error path that never fires the
    // callback; a healthy transfer takes well under a millisecond.
    if (xSemaphoreTake(s_lcd_idle, pdMS_TO_TICKS(200)) != pdTRUE) s_wait_timeouts++;
}

uint32_t lcd_wait_timeouts(void) { return s_wait_timeouts; }

static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

// Vendor init sequence for this panel, copied from the AI Passport BSP (which
// took it from the panel vendor's reference TFT_init()). Generic ST7789
// defaults give wrong porch / power / gamma on this glass.
struct StInitCmd { uint8_t cmd; uint8_t data[16]; uint8_t len; };

static const StInitCmd ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5},  // PORCTRL
    {0xB7, {0x35}, 1},                          // GCTRL
    {0xBB, {0x21}, 1},                          // VCOMS
    {0xC0, {0x2C}, 1},                          // LCMCTRL
    {0xC2, {0x01}, 1},                          // VDVVRHEN
    {0xC3, {0x0B}, 1},                          // VRHS
    {0xC4, {0x20}, 1},                          // VDVSET
    {0xC6, {0x0F}, 1},                          // FRCTRL2
    {0xD0, {0xA7, 0xA1}, 2},                    // PWCTRL1
    {0xD0, {0xA4, 0xA1}, 2},                    // PWCTRL1 (vendor re-send wins)
    {0xD6, {0xA1}, 1},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14},   // PVGAMCTRL
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14},   // NVGAMCTRL
};

// The stock AI Passport firmware holds the LCD pins across deep sleep; those
// holds survive a reset, so the pins get safe levels before the hold is lifted.
static void release_deep_sleep_holds(void) {
    gpio_deep_sleep_hold_dis();
    const int pins[]   = {AI_LCD_CS, AI_LCD_SCLK, AI_LCD_MOSI, AI_LCD_DC, AI_LCD_BL};
    const int levels[] = {1, 0, 0, 0, 0};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)pins[i], levels[i]);
        gpio_hold_dis((gpio_num_t)pins[i]);
    }
}

static void backlight_init(void) {
    ledc_timer_config_t t = {};
    t.speed_mode      = AI_BL_LEDC_MODE;
    t.duty_resolution = AI_BL_LEDC_RES;
    t.timer_num       = AI_BL_LEDC_TIMER;
    t.freq_hz         = AI_BL_LEDC_FREQ_HZ;
    t.clk_cfg         = LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) != ESP_OK) return;

    ledc_channel_config_t ch = {};
    ch.gpio_num   = AI_LCD_BL;
    ch.speed_mode = AI_BL_LEDC_MODE;
    ch.channel    = AI_BL_LEDC_CHANNEL;
    ch.timer_sel  = AI_BL_LEDC_TIMER;
    ch.duty       = 0;
    ch.hpoint     = 0;
    if (ledc_channel_config(&ch) != ESP_OK) return;

    s_bl_ready = true;
    lcd_backlight(70);   // plenty indoors, keeps the panel/regulator cooler
}

void lcd_init(void) {
    if (s_ready) return;
    release_deep_sleep_holds();

    s_buf = (uint16_t *)heap_caps_malloc((size_t)LCD_W * STRIP_LINES * 2, MALLOC_CAP_DMA);
    if (!s_buf) return;
    s_lcd_idle = xSemaphoreCreateBinary();
    if (s_lcd_idle) xSemaphoreGive(s_lcd_idle);

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = AI_LCD_MOSI;
    bus.miso_io_num     = -1;
    bus.sclk_io_num     = AI_LCD_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = LCD_W * STRIP_LINES * 2;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return;

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num       = AI_LCD_CS;
    io.dc_gpio_num       = AI_LCD_DC;
    io.pclk_hz           = AI_LCD_PCLK_HZ;
    io.spi_mode          = AI_LCD_SPI_MODE;
    io.lcd_cmd_bits      = 8;
    io.lcd_param_bits    = 8;
    io.trans_queue_depth = 10;
    io.on_color_trans_done = lcd_trans_done;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io) != ESP_OK) return;

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = AI_LCD_RST;
    dev.color_space    = ESP_LCD_COLOR_SPACE_RGB;
    dev.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(s_io, &dev, &s_panel) != ESP_OK) return;

    if (esp_lcd_panel_reset(s_panel) != ESP_OK) return;
    if (esp_lcd_panel_init(s_panel) != ESP_OK) return;

    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        esp_lcd_panel_io_tx_param(s_io, ST7789P3_CMDS[i].cmd, ST7789P3_CMDS[i].data,
                                  ST7789P3_CMDS[i].len);
    }
    const uint8_t ramctrl[2] = {0x00, 0xE0};   // 16bpp expansion, as the IDF 5.x driver does
    esp_lcd_panel_io_tx_param(s_io, 0xBD, ramctrl, 2);

    esp_lcd_panel_invert_color(s_panel, true);      // this glass ships inverted
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);

    backlight_init();
    s_ready = true;
    Serial.println("[lcd] ST7789P3 240x320 ready");
}

bool lcd_ready(void) { return s_ready; }

void lcd_backlight(uint8_t percent) {
    if (!s_bl_ready) return;
    if (percent > 100) percent = 100;
    uint32_t max_duty = (1u << AI_BL_LEDC_RES) - 1u;
    uint32_t duty = max_duty * percent / 100u;
    ledc_set_duty(AI_BL_LEDC_MODE, AI_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(AI_BL_LEDC_MODE, AI_BL_LEDC_CHANNEL);
}

// Blank the panel for idle. Only the backlight is cut: the panel keeps its
// picture, so waking is instant and needs no full repaint.
void lcd_power(bool on) {
    if (!s_ready) return;
    lcd_backlight(on ? 70 : 0);
}

static void push(int x, int y, int w, int h) {
    // The panel takes RGB565 high byte first; the IDF SPI panel IO sends the
    // buffer byte for byte, so the scratch strip holds byte-swapped pixels.
    if (esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, s_buf) != ESP_OK && s_lcd_idle) {
        xSemaphoreGive(s_lcd_idle);   // never leave the buffer marked busy
    }
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_ready || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    wait_idle();
    const uint16_t c = be16(color);
    int yy = y, rows = h;
    while (rows > 0) {
        int chunk = rows > STRIP_LINES ? STRIP_LINES : rows;
        int px = w * chunk;
        for (int i = 0; i < px; i++) s_buf[i] = c;
        push(x, yy, w, chunk);
        yy += chunk;
        rows -= chunk;
    }
}

void lcd_fill(uint16_t color) { lcd_fill_rect(0, 0, LCD_W, LCD_H, color); }

void lcd_text(int x, int y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg) {
    if (!s_ready || !s || !*s) return;
    if (scale < 1) scale = 1;

    size_t len = strlen(s);
    size_t max_len = (x >= 0 && x < LCD_W) ? (size_t)((LCD_W - x) / (8 * scale)) : 0;
    if (len > max_len) len = max_len;
    if (len == 0) return;

    int w = (int)len * 8 * scale;
    int h = 8 * scale;
    if (x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) return;
    if ((size_t)w * h > (size_t)LCD_W * STRIP_LINES) return;

    wait_idle();
    const uint16_t f = be16(fg), b = be16(bg);
    for (int r = 0; r < h; r++) {
        uint16_t *row = s_buf + (size_t)r * w;
        for (int i = 0; i < w; i++) row[i] = b;
        for (size_t gi = 0; gi < len; gi++) {
            uint8_t ch = (uint8_t)s[gi];
            if (ch < 32 || ch > 127) ch = '?';
            uint8_t bits = (uint8_t)font8x8_basic[ch][r / scale];
            if (!bits) continue;
            int base = (int)gi * 8 * scale;
            for (int px = 0; px < 8; px++) {
                if (!(bits & (1u << px))) continue;
                uint16_t *dst = row + base + px * scale;
                for (int k = 0; k < scale; k++) dst[k] = f;
            }
        }
    }
    push(x, y, w, h);
}

#endif  // ADBLOCK_AIPASSPORT

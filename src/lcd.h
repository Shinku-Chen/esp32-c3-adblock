// lcd.h — ST7789P3 (240x320, 4-line SPI) panel driver for the AI Passport build.
//
// Wiring follows the AI Passport hardware (SPI2: SCLK=8, MOSI=9, CS=1, DC=20,
// no reset line, backlight on GPIO21); see the upstream BSP's bsp_pins.h.
#pragma once

#include <stdint.h>

#define LCD_W 240
#define LCD_H 320

// RGB565 from 8-bit components
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void lcd_init(void);
bool lcd_ready(void);
void lcd_backlight(uint8_t percent);           // 0..100
void lcd_power(bool on);                       // backlight only; the panel keeps its picture
uint32_t lcd_wait_timeouts(void);              // colour transfers that needed the 200 ms guard
void lcd_fill(uint16_t color);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_text(int x, int y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg);

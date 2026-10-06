// screen.cpp — the AI Passport panel content.
//
// One static page per state, plus a small live stats block on the online page
// (blocked queries / allowed queries / devices seen), fed from loop() once a
// second. Nothing here touches the rest of the firmware.

#include "screen.h"

#ifdef ADBLOCK_AIPASSPORT

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "lcd.h"

#define C_BG     RGB565(0x0d, 0x11, 0x17)
#define C_HEAD   RGB565(0x17, 0x4a, 0x2b)
#define C_TEXT   RGB565(0xc9, 0xd1, 0xd9)
#define C_DIM    RGB565(0x8b, 0x94, 0x9e)
#define C_GREEN  RGB565(0x3f, 0xb9, 0x50)
#define C_WHITE  0xFFFF

#define STATS_PERIOD_MS 1000
// Rows of the live stats block: y, erase height, text scale.
#define ROW_BLOCKED_Y   142
#define ROW_BLOCKED_H   24
#define ROW_ALLOWED_Y   188
#define ROW_ALLOWED_H   16
#define ROW_DEVICES_Y   224
#define ROW_DEVICES_H   16
#define ROW_X           12

enum ScreenState : uint8_t { S_NONE = 0, S_BOOT, S_CONNECTING, S_SETUP, S_ONLINE };

static ScreenState s_state = S_NONE;
static char s_ssid[40];
static char s_ip[24];

static uint32_t s_blocked, s_allowed;
static int s_devices;
static bool s_have_stats;
static uint32_t s_stats_ms;
static char s_blocked_s[16], s_allowed_s[16], s_devices_s[12];

static int centerX(const char *s, uint8_t scale) {
    int x = (LCD_W - (int)strlen(s) * 8 * scale) / 2;
    return x < 0 ? 0 : x;
}

static void clip(char *s, size_t max_chars) {
    if (strlen(s) > max_chars) s[max_chars] = 0;
}

// 1234567 -> "1,234,567"
static void fmtNum(uint32_t v, char *out, size_t n) {
    char tmp[12];
    int d = 0;
    do { tmp[d++] = (char)('0' + v % 10); v /= 10; } while (v);
    int o = 0;
    for (int i = d - 1; i >= 0 && o + 1 < (int)n; i--) {
        if (i != d - 1 && ((d - 1 - i) % 3) == 0) out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = 0;
}

static void header(void) {
    lcd_fill(C_BG);
    lcd_fill_rect(0, 0, LCD_W, 34, C_HEAD);
    lcd_text(8, 9, "C3 AdBlock", 2, C_WHITE, C_HEAD);
}

static void footer(void) {
    lcd_text(centerX("c3adblock.local", 1), 296, "c3adblock.local", 1, C_DIM, C_BG);
}

// Paint the three live values; only changed fields are rewritten, so the 1 Hz
// refresh never flickers.
static void paint_stats(bool force) {
    char b[16], a[12], d[8];
    fmtNum(s_blocked, b, sizeof(b));
    fmtNum((uint32_t)s_allowed, a, sizeof(a));
    snprintf(d, sizeof(d), "%d", s_devices);

    if (force || strcmp(b, s_blocked_s) != 0) {
        lcd_fill_rect(ROW_X, ROW_BLOCKED_Y, LCD_W - ROW_X, ROW_BLOCKED_H, C_BG);
        lcd_text(ROW_X, ROW_BLOCKED_Y, b, 3, C_GREEN, C_BG);
        snprintf(s_blocked_s, sizeof(s_blocked_s), "%s", b);
    }
    if (force || strcmp(a, s_allowed_s) != 0) {
        lcd_fill_rect(ROW_X, ROW_ALLOWED_Y, LCD_W - ROW_X, ROW_ALLOWED_H, C_BG);
        lcd_text(ROW_X, ROW_ALLOWED_Y, a, 2, C_TEXT, C_BG);
        snprintf(s_allowed_s, sizeof(s_allowed_s), "%s", a);
    }
    if (force || strcmp(d, s_devices_s) != 0) {
        lcd_fill_rect(ROW_X, ROW_DEVICES_Y, LCD_W - ROW_X, ROW_DEVICES_H, C_BG);
        lcd_text(ROW_X, ROW_DEVICES_Y, d, 2, C_TEXT, C_BG);
        snprintf(s_devices_s, sizeof(s_devices_s), "%s", d);
    }
}

static void draw(void) {
    char ssid[40];
    char ip[24];
    snprintf(ssid, sizeof(ssid), "%s", s_ssid);
    snprintf(ip, sizeof(ip), "%s", s_ip);

    switch (s_state) {
        case S_BOOT:
            header();
            lcd_text(centerX("C3 AdBlock", 3), 110, "C3 AdBlock", 3, C_TEXT, C_BG);
            lcd_text(centerX("DNS ad blocker", 1), 152, "DNS ad blocker", 1, C_DIM, C_BG);
            lcd_text(centerX("starting ...", 1), 190, "starting ...", 1, C_DIM, C_BG);
            footer();
            break;

        case S_CONNECTING:
            header();
            lcd_text(centerX("CONNECTING", 2), 70, "CONNECTING", 2, C_GREEN, C_BG);
            clip(ssid, 14);
            lcd_text(centerX(ssid, 2), 110, ssid, 2, C_WHITE, C_BG);
            lcd_text(centerX("waiting for the network", 1), 150,
                     "waiting for the network", 1, C_DIM, C_BG);
            lcd_text(10, 196, "If it cannot connect, the", 1, C_DIM, C_BG);
            lcd_text(10, 208, "setup hotspot starts by", 1, C_DIM, C_BG);
            lcd_text(10, 220, "itself after 30 seconds.", 1, C_DIM, C_BG);
            footer();
            break;

        case S_SETUP:
            header();
            lcd_text(centerX("WI-FI SETUP", 2), 56, "WI-FI SETUP", 2, C_GREEN, C_BG);
            lcd_text(10, 96, "1. JOIN THIS NETWORK", 1, C_DIM, C_BG);
            clip(ssid, 14);
            lcd_text(10, 108, ssid, 2, C_WHITE, C_BG);
            lcd_text(10, 150, "2. OPEN THE SETUP PAGE", 1, C_DIM, C_BG);
            clip(ip, 14);
            lcd_text(10, 162, ip, 2, C_WHITE, C_BG);
            lcd_text(10, 206, "Pick your Wi-Fi, type the", 1, C_DIM, C_BG);
            lcd_text(10, 218, "password - the device joins", 1, C_DIM, C_BG);
            lcd_text(10, 230, "it and reboots.", 1, C_DIM, C_BG);
            break;

        case S_ONLINE: {
            char line[40];
            header();
            lcd_text(centerX("ONLINE", 2), 48, "ONLINE", 2, C_GREEN, C_BG);
            snprintf(line, sizeof(line), "IP %s", ip);
            lcd_text(centerX(line, 1), 82, line, 1, C_TEXT, C_BG);
            lcd_text(centerX("c3adblock.local", 1), 96, "c3adblock.local", 1, C_DIM, C_BG);
            lcd_fill_rect(16, 118, LCD_W - 32, 1, C_DIM);

            lcd_text(ROW_X, 130, "BLOCKED", 1, C_DIM, C_BG);
            lcd_text(ROW_X, 176, "ALLOWED", 1, C_DIM, C_BG);
            lcd_text(ROW_X, 212, "DEVICES", 1, C_DIM, C_BG);
            // fall through to the values below
            s_blocked_s[0] = s_allowed_s[0] = s_devices_s[0] = 0;
            if (s_have_stats) paint_stats(true);
            break;
        }

        default:
            break;
    }
}

static void show(ScreenState state, const char *a, const char *b) {
    if (!lcd_ready()) return;
    const char *na = a ? a : "";
    const char *nb = b ? b : "";
    if (state == s_state && strcmp(na, s_ssid) == 0 && strcmp(nb, s_ip) == 0) return;
    s_state = state;
    snprintf(s_ssid, sizeof(s_ssid), "%s", na);
    snprintf(s_ip, sizeof(s_ip), "%s", nb);
    draw();
    Serial.printf("[screen] state=%d ssid=\"%s\" ip=\"%s\"\n", (int)state, s_ssid, s_ip);
}

void screen_init(void) {
    lcd_init();
    show(S_BOOT, "", "");
}

void screen_connecting(const char *ssid) { show(S_CONNECTING, ssid, ""); }
void screen_setup(const char *ap, const char *ip) { show(S_SETUP, ap, ip); }
void screen_online(const char *ip) { show(S_ONLINE, "", ip); }

void screen_stats(uint32_t blocked, uint32_t allowed, int devices) {
    if (!lcd_ready()) return;
    s_blocked = blocked;
    s_allowed = allowed;
    s_devices = devices;
    s_have_stats = true;
    if (s_state != S_ONLINE) return;
    uint32_t now = millis();
    if (now - s_stats_ms < STATS_PERIOD_MS) return;
    s_stats_ms = now;
    paint_stats(false);
}

#else   // !ADBLOCK_AIPASSPORT — no panel on the other boards

void screen_init(void) {}
void screen_connecting(const char *) {}
void screen_setup(const char *, const char *) {}
void screen_online(const char *) {}
void screen_stats(uint32_t, uint32_t, int) {}

#endif  // ADBLOCK_AIPASSPORT

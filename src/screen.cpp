// screen.cpp — the AI Passport panel content.
//
// One static page per state; nothing here touches the rest of the firmware.
// The only job is to point a human at the web page that does the real work:
// join the setup hotspot and open 192.168.4.1, or open c3adblock.local later.

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

enum ScreenState : uint8_t { S_NONE = 0, S_BOOT, S_CONNECTING, S_SETUP, S_ONLINE };

static ScreenState s_state = S_NONE;
static char s_ssid[40];
static char s_ip[24];

static int centerX(const char *s, uint8_t scale) {
    int x = (LCD_W - (int)strlen(s) * 8 * scale) / 2;
    return x < 0 ? 0 : x;
}

static void clip(char *s, size_t max_chars) {
    if (strlen(s) > max_chars) s[max_chars] = 0;
}

static void header(void) {
    lcd_fill(C_BG);
    lcd_fill_rect(0, 0, LCD_W, 34, C_HEAD);
    lcd_text(8, 9, "C3 AdBlock", 2, C_WHITE, C_HEAD);
}

static void footer(void) {
    lcd_text(centerX("c3adblock.local", 1), 296, "c3adblock.local", 1, C_DIM, C_BG);
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

        case S_ONLINE:
            header();
            lcd_text(centerX("ONLINE", 2), 56, "ONLINE", 2, C_GREEN, C_BG);
            lcd_text(centerX("open the dashboard", 1), 104, "open the dashboard", 1, C_DIM, C_BG);
            lcd_text(centerX("c3adblock.local", 2), 116, "c3adblock.local", 2, C_TEXT, C_BG);
            lcd_text(centerX("or", 1), 156, "or", 1, C_DIM, C_BG);
            clip(ip, 14);
            lcd_text(centerX(ip, 2), 168, ip, 2, C_WHITE, C_BG);
            lcd_text(10, 212, "Stats, pause and blocklist", 1, C_DIM, C_BG);
            lcd_text(10, 224, "updates all live in the web", 1, C_DIM, C_BG);
            lcd_text(10, 236, "dashboard.", 1, C_DIM, C_BG);
            break;

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
}

void screen_init(void) {
    lcd_init();
    show(S_BOOT, "", "");
}

void screen_connecting(const char *ssid) { show(S_CONNECTING, ssid, ""); }
void screen_setup(const char *ap, const char *ip) { show(S_SETUP, ap, ip); }
void screen_online(const char *ip) { show(S_ONLINE, "", ip); }

#else   // !ADBLOCK_AIPASSPORT — no panel on the other boards

void screen_init(void) {}
void screen_connecting(const char *) {}
void screen_setup(const char *, const char *) {}
void screen_online(const char *) {}

#endif  // ADBLOCK_AIPASSPORT

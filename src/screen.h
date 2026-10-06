// screen.h — the on-device text shown by the AI Passport variant.
//
// Kept deliberately small: the screen only tells you which hotspot to join and
// which web page to open. Everything else lives in the existing web dashboard.
// On every other board these functions are no-ops.
#pragma once

#include <stdint.h>

void screen_init(void);                             // "starting" splash, drawn at boot
void screen_connecting(const char *ssid);           // trying the saved network
void screen_setup(const char *ap, const char *ip);  // captive-portal hotspot is up
void screen_online(const char *ip);                 // joined the network
void screen_stats(uint32_t blocked, uint32_t allowed, int devices, int rssi, int temp_c);
                                                                        // live counters, 1 Hz

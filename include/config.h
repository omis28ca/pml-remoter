#pragma once
#include <Arduino.h>

// ---- Hardware (classic ESP32 DevKit) ---------------------------------------
#define NUM_CHANNELS 4
static const uint8_t TX_PINS[NUM_CHANNELS] = {25, 26, 27, 32};  // -> driver transistors
#define RX_PIN 34        // IR receiver OUT (GPIO34 is input-only, which is fine here)
#define STATUS_LED 2     // on-board LED on most DevKits

// ---- Network ---------------------------------------------------------------
#define HOSTNAME "irhub"              // also reachable as irhub.local
#define AP_SSID "IRHub-Setup"         // fallback access point (open) for first-time Wi-Fi setup
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define TCP_PORT 4999
#define MAX_TCP_CLIENTS 3

// ---- IR --------------------------------------------------------------------
#define CAPTURE_BUF 1024              // raw capture buffer (entries)
#define CAPTURE_TIMEOUT_MS 25         // gap that ends a frame while learning
#define LEARN_TIMEOUT_MS 15000        // how long learn mode waits for a button press
#define RAW_REPEAT_GAP_MS 40          // gap between repeats of raw codes

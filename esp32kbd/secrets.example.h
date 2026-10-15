#pragma once

// Copy this file to secrets.h and fill it in. secrets.h is git-ignored.

// Your Wi-Fi network (2.4 GHz only - the ESP32 can't see 5 GHz networks).
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// Bearer token required by the MCP endpoint. Anyone with it can type on the
// attached computer, so make it long and random, e.g.:
//   python -c "import secrets; print(secrets.token_hex(24))"
#define MCP_TOKEN "CHANGE_ME"

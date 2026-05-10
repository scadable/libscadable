// AP-mode captive portal for the SCADABLE provisioning firmware.
//
// Brings up an open-auth WiFi access point named `scadable-XXXX`
// (last 4 hex of the device MAC), runs a UDP DNS catch-all that
// answers every query with the AP's own IP (192.168.4.1), and
// serves a one-page HTML form on port 80 that writes the
// submitted SSID + password into NVS namespace `wifi` and
// reboots into STA mode.
//
// Never returns — caller is expected to block forever, since the
// device only exits this mode via `esp_restart()` after the form
// POST handler persists the new credentials.

#pragma once

void ap_mode_start(void);

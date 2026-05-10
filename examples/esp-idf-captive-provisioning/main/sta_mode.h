// STA-mode bringup for the SCADABLE provisioning firmware.
//
// Connects to the WiFi network whose creds were written into NVS
// namespace `wifi` by the AP-mode captive portal handler. After
// `IP_EVENT_STA_GOT_IP`, calls `scadable_init` + `scadable_connect`
// using the device cert already provisioned into NVS namespace
// `scadable_certs` by the dashboard's WebSerial flasher.
//
// On 5 consecutive `WIFI_EVENT_STA_DISCONNECTED` events without ever
// having reached `IP_EVENT_STA_GOT_IP`, erases the NVS creds and
// reboots — that drops the device back into AP-mode for re-entry of
// (presumably bad) credentials. This is the recovery path when an
// end-user typos their WiFi password.

#pragma once

void sta_mode_start(const char *ssid, const char *password);

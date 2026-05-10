# esp-idf-captive-provisioning

SCADABLE provisioning firmware. Boots → if no WiFi creds in NVS, broadcasts an open `scadable-XXXX` access point with a captive-portal form. End-user picks their network, enters the password, taps Save. Device writes creds to NVS, reboots into STA mode, joins WiFi, calls `scadable_init`/`scadable_connect` with the device cert that was already provisioned into NVS, and idles waiting for the customer firmware to OTA in.

## What gets flashed when

This example is what the SCADABLE web dashboard's "Provisioning mode" path writes onto a blank ESP32 (see `web-dashboard/src/features/namespaces/AddDeviceDialog.tsx`). After the chip is online, the customer's compiled firmware (built by the BYOR pipeline from their GitHub repo) lands via OTA into `ota_0` / `ota_1` and replaces this provisioning bootstrap.

## Build

```bash
idf.py set-target esp32-s3
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

First boot: open WiFi settings, join `scadable-XXXX`. The captive portal banner appears automatically (DNS catch-all hijacks every probe URL the OS pings). Pick your WiFi, enter password, hit Save. Reboots into STA. Watch the monitor for `scadable connected — waiting for customer firmware OTA`.

## NVS contract

| Namespace        | Key             | Written by                  | Read by                    |
|------------------|-----------------|-----------------------------|----------------------------|
| `wifi`           | `ssid`          | captive-portal `/provision` | `app_main`                 |
| `wifi`           | `password`      | captive-portal `/provision` | `app_main`                 |
| `scadable_certs` | `device_cert`   | dashboard WebSerial flasher | `scadable_init`            |
| `scadable_certs` | `device_key`    | dashboard WebSerial flasher | `scadable_init`            |

If WiFi creds are missing or empty, AP mode runs. If 5 consecutive STA disconnects happen without ever reaching `IP_EVENT_STA_GOT_IP` (typical of a typo'd password), the firmware erases the creds and reboots back into AP mode.

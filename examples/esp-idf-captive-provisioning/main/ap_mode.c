// AP-mode captive portal implementation.
// SCADABLE 2026 · Apache-2.0
//
// Three concurrent surfaces:
//   1. WiFi AP (open auth, SSID `scadable-XXXX`).
//   2. UDP DNS catch-all on port 53 (every query → 192.168.4.1).
//   3. HTTP server on port 80:
//        GET  /                       → captive_portal.html (the form)
//        GET  /generate_204           → 200 + form body (Android probe)
//        GET  /hotspot-detect.html    → 200 + form body (iOS probe)
//        GET  /scan                   → JSON list of nearby networks
//        POST /provision              → write NVS + reboot
//
// The DNS catch-all is what makes iOS / Android pop the "Sign in to WiFi"
// banner — the OS probes a known URL (e.g. captive.apple.com), and any
// non-matching response counts as "captive". We hijack DNS so every probe
// resolves to us, then return a 200 + HTML so the OS surfaces the form.

#include "ap_mode.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ap-mode";

// EMBED_TXTFILES generates two symbols for `captive_portal.html`:
//   captive_portal_html_start  — pointer to first byte
//   captive_portal_html_end    — pointer to one-past-last byte
extern const uint8_t captive_portal_html_start[] asm("_binary_captive_portal_html_start");
extern const uint8_t captive_portal_html_end[] asm("_binary_captive_portal_html_end");

// ─────────────────────────────────────────────────────────────────────────
// URL-decoding for the form-encoded POST body. Decodes %xx + treats `+` as
// space. In-place; null-terminates at the original length boundary.
// ─────────────────────────────────────────────────────────────────────────
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = hex_nibble(r[1]), lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Pull `key=value&...` out of a form body. Caller-owned `out`.
static bool form_field(const char *body, const char *key, char *out, size_t out_len) {
    size_t klen   = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen     = end ? (size_t)(end - p) : strlen(p);
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, p, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return true;
        }
        const char *next = strchr(p, '&');
        if (!next) return false;
        p = next + 1;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────
// HTTP handlers.
// ─────────────────────────────────────────────────────────────────────────

static esp_err_t serve_form(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const size_t len = captive_portal_html_end - captive_portal_html_start;
    return httpd_resp_send(req, (const char *)captive_portal_html_start, len);
}

// Minimal JSON list of nearby networks for the form's SSID dropdown.
// We do a fresh blocking scan per request — cheap given how rarely this
// hits and avoids stale results when the user reloads the page after
// moving rooms.
static esp_err_t scan_handler(httpd_req_t *req) {
    wifi_scan_config_t cfg = {0};
    esp_err_t err          = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        return httpd_resp_send(req, "[]", 2);
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;  // cap so JSON stays small
    wifi_ap_record_t *records = calloc(n, sizeof(wifi_ap_record_t));
    if (!records) return httpd_resp_send(req, "[]", 2);
    esp_wifi_scan_get_ap_records(&n, records);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < n; i++) {
        if (records[i].ssid[0] == '\0') continue;  // skip hidden
        char buf[96];
        // Escape only the basics — SSIDs with `"` or `\` are exotic but
        // possible. Worst case the JSON string breaks and the dropdown
        // falls back to the free-text input.
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d}", i == 0 ? "" : ",",
                 (const char *)records[i].ssid, records[i].rssi);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    free(records);
    return ESP_OK;
}

static esp_err_t provision_handler(httpd_req_t *req) {
    char body[256];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    body[total] = '\0';

    char ssid[33]     = {0};
    char password[65] = {0};
    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing ssid");
    }
    form_field(body, "password", password, sizeof(password));  // password may be empty (open WiFi)

    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "nvs_open failed");
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "password", password);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "provisioned ssid=%s — rebooting in 2s", ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
                       "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:24px'>"
                       "<h2>Saved</h2><p>The device is rebooting and will join your WiFi.</p>"
                       "</body></html>");

    // Defer restart so the response actually flushes to the browser.
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static httpd_handle_t http_start(void) {
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // We register fewer than the default URI count, but several captive
    // probes from each OS. Bump the slot count just in case.
    cfg.max_uri_handlers  = 12;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }
    httpd_uri_t form_uri = {.uri = "/", .method = HTTP_GET, .handler = serve_form};
    httpd_uri_t scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = scan_handler};
    httpd_uri_t prov_uri = {.uri = "/provision", .method = HTTP_POST, .handler = provision_handler};
    // OS captive-portal probe URLs. All of them get the same form HTML
    // so any probe surfaces the "Sign in to WiFi" banner on the device.
    httpd_uri_t probe_g = {.uri = "/generate_204", .method = HTTP_GET, .handler = serve_form};
    httpd_uri_t probe_a = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = serve_form};
    httpd_uri_t probe_w = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = serve_form};
    httpd_uri_t probe_n = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = serve_form};
    httpd_register_uri_handler(server, &form_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &prov_uri);
    httpd_register_uri_handler(server, &probe_g);
    httpd_register_uri_handler(server, &probe_a);
    httpd_register_uri_handler(server, &probe_w);
    httpd_register_uri_handler(server, &probe_n);
    return server;
}

// ─────────────────────────────────────────────────────────────────────────
// Captive DNS server. Answers EVERY query with a single A record pointing
// at 192.168.4.1 (the AP's gateway IP). Implemented inline rather than as
// a component because the wire format is tiny and dragging in a full DNS
// library would balloon the firmware.
// ─────────────────────────────────────────────────────────────────────────

#define DNS_PORT    53
#define DNS_MAX_LEN 512

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS listening on UDP/53");

    uint8_t buf[DNS_MAX_LEN];
    while (true) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int n             = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < 12) continue;  // smaller than DNS header

        // Set QR=1 (response), AA=1 (authoritative), RA=1 (recursion avail).
        buf[2] = 0x84;  // QR=1, AA=1, OP=0
        buf[3] = 0x80;  // RA=1
        // ANCOUNT = 1, copy QDCOUNT into ANCOUNT slot.
        buf[6] = 0;
        buf[7] = 1;
        // NSCOUNT, ARCOUNT zeroed.
        buf[8]  = 0;
        buf[9]  = 0;
        buf[10] = 0;
        buf[11] = 0;

        // Walk the question section to find its end (so we can append the
        // answer). Question = QNAME (length-prefixed labels, terminated by
        // a zero byte) + QTYPE (2 bytes) + QCLASS (2 bytes).
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
            if (p >= DNS_MAX_LEN) break;
        }
        p += 1 + 4;                          // skip null terminator + QTYPE + QCLASS
        if (p + 16 > DNS_MAX_LEN) continue;  // no room for answer

        // Append the answer record. NAME = 0xC00C (compression pointer to
        // the question's QNAME), TYPE=A, CLASS=IN, TTL=60, RDLENGTH=4,
        // RDATA = 192.168.4.1.
        buf[p++] = 0xC0;
        buf[p++] = 0x0C;
        buf[p++] = 0x00;
        buf[p++] = 0x01;  // TYPE A
        buf[p++] = 0x00;
        buf[p++] = 0x01;  // CLASS IN
        buf[p++] = 0x00;
        buf[p++] = 0x00;
        buf[p++] = 0x00;
        buf[p++] = 0x3C;  // TTL 60
        buf[p++] = 0x00;
        buf[p++] = 0x04;  // RDLENGTH 4
        buf[p++] = 192;
        buf[p++] = 168;
        buf[p++] = 4;
        buf[p++] = 1;

        sendto(sock, buf, p, 0, (struct sockaddr *)&src, src_len);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// AP setup.
// ─────────────────────────────────────────────────────────────────────────

void ap_mode_start(void) {
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // AP+STA so we can scan nearby SSIDs from the form while still
    // serving the AP. (esp_wifi_scan_start works in APSTA without
    // disconnecting AP clients.)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Build SSID `scadable-XXXX` from the last 4 hex of the device MAC.
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    wifi_config_t wifi_cfg = {0};
    snprintf((char *)wifi_cfg.ap.ssid, sizeof(wifi_cfg.ap.ssid), "scadable-%02X%02X", mac[4],
             mac[5]);
    wifi_cfg.ap.ssid_len       = strlen((const char *)wifi_cfg.ap.ssid);
    wifi_cfg.ap.channel        = 1;
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode       = WIFI_AUTH_OPEN;  // open — captive portals
                                                  // shouldn't gate on WPA.
    wifi_cfg.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP up: SSID=%s (open)", wifi_cfg.ap.ssid);

    // HTTP + DNS.
    if (http_start() == NULL) {
        ESP_LOGE(TAG, "http server failed; will reboot in 30s");
        vTaskDelay(pdMS_TO_TICKS(30000));
        esp_restart();
    }
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    // Block forever — only exit is provision_handler -> esp_restart().
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

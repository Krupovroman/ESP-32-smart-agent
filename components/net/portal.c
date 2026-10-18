// Phone setup portal: SoftAP "RoundAI-XXXX" + captive DNS + a small web form for Wi-Fi and the AI key.
#include "portal.h"
#include "net.h"
#include "settings.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_netif.h"   // dns_server.h needs esp_ip4_addr_t
#include "dns_server.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "portal";

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

static esp_netif_t *s_ap;
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static bool s_active;
static char s_ssid[33];
static char s_pass[12];
static uint32_t s_ap_ip;

// Only clients of the setup network may use the portal (not the home LAN).
static bool from_ap(httpd_req_t *req)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    uint32_t ip = addr.sin6_family == AF_INET6 ? addr.sin6_addr.un.u32_addr[3]
                                               : ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    if (ip == s_ap_ip || (settings_get_int("dev_flags") & 4)) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "setup is only available on the device network");
    return false;
}

static void json_str(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    out[o++] = '"';
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = 0;
}

static esp_err_t h_root(httpd_req_t *req)
{
    if (!from_ap(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    if (!from_ap(req)) {
        return ESP_OK;
    }
    net_ap_t aps[16];
    int n = 0;
    net_scan(5000, aps, 16, &n);
    char *buf = malloc(3072);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    size_t o = snprintf(buf, 3072, "{\"aps\":[");
    for (int i = 0; i < n && o < 2900; i++) {
        char ssid[80];
        json_str(ssid, sizeof(ssid), aps[i].ssid);
        o += snprintf(buf + o, 3072 - o, "%s{\"ssid\":%s,\"rssi\":%d,\"auth\":%d}", i ? "," : "", ssid, aps[i].rssi,
                      aps[i].auth != WIFI_AUTH_OPEN);
    }
    o += snprintf(buf + o, 3072 - o, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, o);
    free(buf);
    return err;
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!from_ap(req)) {
        return ESP_OK;
    }
    net_status_t st;
    net_status(&st);
    char model[100], url[140], ssid[80], tmp[130];
    settings_get_str("ai_model", tmp, sizeof(tmp));
    json_str(model, sizeof(model), tmp);
    settings_get_str("ai_url", tmp, sizeof(tmp));
    json_str(url, sizeof(url), tmp);
    json_str(ssid, sizeof(ssid), st.ssid);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"net\":\"%s\",\"ssid\":%s,\"ip\":\"%s\",\"ai_model\":%s,\"ai_url\":%s,\"ai_key_set\":%s}",
                     net_state_name(st.state), ssid, st.ip, model, url, settings_is_set("ai_key") ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

// Finds key=value in an urlencoded body and decodes it into out.
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl >= cap) {
                vl = cap - 1;
            }
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            url_decode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return false;
}

typedef struct {
    char ssid[33];
    char pass[65];
} conn_job_t;

static void connect_task(void *arg)
{
    conn_job_t *job = arg;
    vTaskDelay(pdMS_TO_TICKS(500));    // let the HTTP answer reach the phone first
    net_connect(job->ssid, job->pass, true);
    memset(job, 0, sizeof(*job));
    free(job);
    vTaskDelete(NULL);
}

static esp_err_t h_save(httpd_req_t *req)
{
    if (!from_ap(req)) {
        return ESP_OK;
    }
    if (req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
        }
        got += r;
    }
    char val[256];
    const char *problem = NULL;
    static const char *const keys[] = { "ai_key", "ai_model", "ai_url" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (form_get(body, keys[i], val, sizeof(val)) && val[0]) {
            if (settings_set_str(keys[i], val) != ESP_OK) {
                problem = keys[i];
            }
        }
    }
    conn_job_t *job = calloc(1, sizeof(conn_job_t));
    bool wifi = job && form_get(body, "ssid", job->ssid, sizeof(job->ssid)) && job->ssid[0];
    if (wifi) {
        form_get(body, "pass", job->pass, sizeof(job->pass));
    }
    memset(val, 0, sizeof(val));
    memset(body, 0, got);
    free(body);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (problem) {
        free(job);
        char msg[96];
        snprintf(msg, sizeof(msg), "РћС€РёР±РєР° РІ РїРѕР»Рµ %s", problem);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, msg);
    }
    if (wifi) {
        char msg[192];
        snprintf(msg, sizeof(msg), "РЎРѕС…СЂР°РЅРµРЅРѕ. РџРѕРґРєР»СЋС‡Р°СЋСЃСЊ Рє В«%sВ»вЂ¦", job->ssid);
        httpd_resp_sendstr(req, msg);
        xTaskCreate(connect_task, "portal_conn", 4096, job, 5, NULL);
        return ESP_OK;
    }
    free(job);
    return httpd_resp_sendstr(req, "РЎРѕС…СЂР°РЅРµРЅРѕ.");
}

static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    return h_redirect(req);
}

static esp_err_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/scan.json", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/status.json", .method = HTTP_GET, .handler = h_status },
        { .uri = "/save", .method = HTTP_POST, .handler = h_save },
        { .uri = "/generate_204", .method = HTTP_GET, .handler = h_redirect },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = h_redirect },
        { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = h_redirect },
        { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = h_redirect },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_404);
    return ESP_OK;
}

esp_err_t portal_start(void)
{
    if (s_active) {
        return ESP_OK;
    }
    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "RoundAI-%02X%02X", mac[4], mac[5]);
    snprintf(s_pass, sizeof(s_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)ap.ap.password, s_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    s_ap_ip = ip.ip.addr;
    // static: the DHCP server keeps this pointer for option 114 (dhcps_set_option_info stores it, no copy)
    static char uri[32];
    snprintf(uri, sizeof(uri), "http://" IPSTR "/", IP2STR(&ip.ip));
    esp_netif_dhcps_stop(s_ap);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(s_ap);

    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns);
    ESP_RETURN_ON_ERROR(start_http(), TAG, "http");
    s_active = true;
    ESP_LOGI(TAG, "setup network \"%s\" is up at %s", s_ssid, uri);
    return ESP_OK;
}

esp_err_t portal_stop(void)
{
    if (!s_active) {
        return ESP_OK;
    }
    httpd_stop(s_httpd);
    s_httpd = NULL;
    stop_dns_server(s_dns);
    s_dns = NULL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_active = false;
    memset(s_pass, 0, sizeof(s_pass));
    ESP_LOGI(TAG, "setup network stopped");
    return ESP_OK;
}

bool portal_active(void) { return s_active; }

void portal_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    strlcpy(ssid, s_ssid, ssid_len);
    strlcpy(pass, s_active ? s_pass : "", pass_len);
}


#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#define WIFI_SSID "DICE"
#define WIFI_PASS "12345678"

#define REPORT_INTERVAL_MS 5000

static const char *TAG = "CSI_DASHBOARD";

/* CSI statistics */
static uint32_t csi_count = 0;
static uint32_t csi_interval_count = 0;

static uint16_t latest_csi_len = 0;
static int8_t latest_rssi = 0;
static bool latest_first_word_invalid = false;

static bool csi_enabled = false;
static bool server_started = false;

static portMUX_TYPE csi_mux = portMUX_INITIALIZER_UNLOCKED;

/* Dashboard webpage */
static const char INDEX_HTML[] =
"<!DOCTYPE html>"
"<html lang='en'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 CSI Dashboard</title>"
"<style>"
"*{box-sizing:border-box}"
"body{margin:0;padding:20px;background:#101827;color:#e5e7eb;"
"font-family:Arial,sans-serif}"
".container{max-width:900px;margin:auto}"
"h1{margin-bottom:5px;color:#60a5fa}"
".subtitle{color:#9ca3af;margin-bottom:25px}"
".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:15px}"
".card{background:#1e293b;padding:20px;border-radius:12px}"
".label{color:#9ca3af;font-size:14px}"
".value{font-size:32px;font-weight:bold;margin-top:10px}"
".unit{font-size:14px;color:#9ca3af}"
".chart{margin-top:18px;background:#1e293b;padding:18px;"
"border-radius:12px}"
"canvas{width:100%;height:220px}"
".status{display:inline-block;padding:6px 12px;border-radius:20px;"
"background:#14532d;color:#86efac;font-size:13px}"
".footer{color:#9ca3af;font-size:12px;margin-top:20px}"
"@media(max-width:500px){.grid{grid-template-columns:1fr}"
".value{font-size:28px}}"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>ESP32 CSI Dashboard</h1>"
"<p class='subtitle'>Wi-Fi signal-quality diagnostics</p>"
"<p><span class='status' id='status'>Connecting...</span></p>"

"<div class='grid'>"

"<div class='card'>"
"<div class='label'>Latest RSSI</div>"
"<div class='value' id='rssi'>--</div>"
"<div class='unit'>dBm</div>"
"</div>"

"<div class='card'>"
"<div class='label'>Total CSI Samples</div>"
"<div class='value' id='total'>0</div>"
"<div class='unit'>Records received</div>"
"</div>"

"<div class='card'>"
"<div class='label'>New Samples</div>"
"<div class='value' id='new'>0</div>"
"<div class='unit'>Last reporting interval</div>"
"</div>"

"<div class='card'>"
"<div class='label'>CSI Length</div>"
"<div class='value' id='length'>0</div>"
"<div class='unit'>Bytes</div>"
"</div>"

"</div>"

"<div class='chart'>"
"<h3>RSSI History</h3>"
"<canvas id='graph'></canvas>"
"<p class='unit'>Latest signal-strength readings</p>"
"</div>"

"<div class='footer'>"
"Updates every 2 seconds. CSI statistics are diagnostic measurements."
"</div>"
"</div>"

"<script>"
"const history=[];"
"const canvas=document.getElementById('graph');"
"const ctx=canvas.getContext('2d');"

"function drawGraph(){"
"const w=canvas.clientWidth;"
"const h=220;"
"const dpr=window.devicePixelRatio||1;"
"canvas.width=w*dpr;"
"canvas.height=h*dpr;"
"ctx.setTransform(dpr,0,0,dpr,0,0);"
"ctx.clearRect(0,0,w,h);"
"ctx.strokeStyle='#475569';"
"ctx.lineWidth=1;"
"ctx.beginPath();"
"ctx.moveTo(35,10);"
"ctx.lineTo(35,h-25);"
"ctx.lineTo(w-5,h-25);"
"ctx.stroke();"

"ctx.fillStyle='#9ca3af';"
"ctx.font='11px Arial';"
"ctx.fillText('-20 dBm',2,18);"
"ctx.fillText('-50 dBm',2,95);"
"ctx.fillText('-80 dBm',2,175);"

"if(history.length<2)return;"

"ctx.strokeStyle='#60a5fa';"
"ctx.lineWidth=2;"
"ctx.beginPath();"

"history.forEach((v,i)=>{"
"const x=35+(i/(history.length-1))*(w-45);"
"const y=10+((-20-v)/60)*(h-35);"
"if(i===0)ctx.moveTo(x,y);"
"else ctx.lineTo(x,y);"
"});"

"ctx.stroke();"
"}"

"async function update(){"
"try{"
"const response=await fetch('/api/stats');"
"if(!response.ok)throw new Error('HTTP error');"
"const data=await response.json();"

"document.getElementById('rssi').textContent=data.rssi;"
"document.getElementById('total').textContent=data.total;"
"document.getElementById('new').textContent=data.new_samples;"
"document.getElementById('length').textContent=data.length;"
"document.getElementById('status').textContent='ESP32 Connected';"

"if(data.total>0){"
"history.push(data.rssi);"
"if(history.length>30)history.shift();"
"}"

"drawGraph();"
"}catch(error){"
"document.getElementById('status').textContent='Connection lost';"
"}"
"}"

"window.addEventListener('resize',drawGraph);"
"update();"
"setInterval(update,2000);"
"</script>"
"</body>"
"</html>";

/* CSI receive callback */
static void csi_rx_callback(void *ctx, wifi_csi_info_t *data)
{
    if (data == NULL || data->buf == NULL || data->len == 0) {
        return;
    }

    portENTER_CRITICAL(&csi_mux);

    csi_count++;
    csi_interval_count++;

    latest_csi_len = data->len;
    latest_rssi = data->rx_ctrl.rssi;
    latest_first_word_invalid = data->first_word_invalid;

    portEXIT_CRITICAL(&csi_mux);
}

/* Webpage route */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    return httpd_resp_send(
        req,
        INDEX_HTML,
        HTTPD_RESP_USE_STRLEN
    );
}

/* Statistics API route */
static esp_err_t stats_handler(httpd_req_t *req)
{
    uint32_t total;
    uint32_t interval;
    uint16_t length;
    int8_t rssi;
    bool invalid;

    portENTER_CRITICAL(&csi_mux);

    total = csi_count;
    interval = csi_interval_count;
    length = latest_csi_len;
    rssi = latest_rssi;
    invalid = latest_first_word_invalid;

    portEXIT_CRITICAL(&csi_mux);

    char json[256];

    snprintf(
        json,
        sizeof(json),
        "{\"total\":%lu,\"new_samples\":%lu,"
        "\"rssi\":%d,\"length\":%u,"
        "\"first_word_invalid\":%s}",
        (unsigned long)total,
        (unsigned long)interval,
        (int)rssi,
        (unsigned int)length,
        invalid ? "true" : "false"
    );

    httpd_resp_set_type(req, "application/json");

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* Start local dashboard server */
static void start_webserver(void)
{
    if (server_started) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {

        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };

        httpd_uri_t stats_uri = {
            .uri = "/api/stats",
            .method = HTTP_GET,
            .handler = stats_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &stats_uri);

        server_started = true;

        ESP_LOGI(TAG, "Web server started");
        ESP_LOGI(TAG, "Open http://10.189.242.92");
    }
    else {
        ESP_LOGE(TAG, "Failed to start web server");
    }
}

/* Enable CSI */
static void enable_csi(void)
{
    if (csi_enabled) {
        return;
    }

    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    csi_enabled = true;

    ESP_LOGI(TAG, "CSI reception enabled");
}

/* Wi-Fi event handler */
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(TAG, "Wi-Fi disconnected, reason: %d",
                 event->reason);

        csi_enabled = false;

        ESP_LOGI(TAG, "Attempting to reconnect...");
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Wi-Fi connected!");
        ESP_LOGI(TAG, "IP Address: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        enable_csi();
        start_webserver();
    }
}

/* Main application */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 CSI Dashboard");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    /* Initialize network */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    /* Initialize Wi-Fi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register Wi-Fi events */
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    /* Register CSI callback */
    ESP_ERROR_CHECK(
        esp_wifi_set_csi_rx_cb(csi_rx_callback, NULL)
    );

    /* Wi-Fi credentials */
    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        WIFI_SSID,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        WIFI_PASS,
        sizeof(wifi_config.sta.password) - 1
    );

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config)
    );

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi station started");

    /* CSV header */
    printf("\n");
    printf(
        "timestamp_ms,total_samples,new_samples,"
        "rssi_dbm,csi_length,first_word_invalid\n"
    );

    fflush(stdout);

    /* CSV reporting loop */
    while (1) {

        vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));

        uint32_t total;
        uint32_t interval;
        uint16_t length;
        int8_t rssi;
        bool invalid;

        portENTER_CRITICAL(&csi_mux);

        total = csi_count;
        interval = csi_interval_count;

        csi_interval_count = 0;

        length = latest_csi_len;
        rssi = latest_rssi;
        invalid = latest_first_word_invalid;

        portEXIT_CRITICAL(&csi_mux);

        int64_t timestamp_ms = esp_timer_get_time() / 1000;

        printf(
            "%lld,%lu,%lu,%d,%u,%s\n",
            (long long)timestamp_ms,
            (unsigned long)total,
            (unsigned long)interval,
            (int)rssi,
            (unsigned int)length,
            invalid ? "true" : "false"
        );

        fflush(stdout);
    }
}

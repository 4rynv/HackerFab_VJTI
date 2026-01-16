#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include <string.h>

/* ================= USER CONFIG ================= */
#define WIFI_SSID      "delta_virus"
#define WIFI_PASS      "66380115"

#define ESC_PIN        21
#define PWM_CHANNEL    LEDC_CHANNEL_0
#define PWM_TIMER      LEDC_TIMER_0
#define PWM_MODE       LEDC_LOW_SPEED_MODE
#define PWM_FREQ       50
#define PWM_RES        LEDC_TIMER_16_BIT
#define ARM_DELAY_MS   3000

/* RPM to Pulse Width Mapping - Calibrated */
#define MIN_RPM        3700      // Actual RPM at 1100us
#define MAX_RPM        10956     // Actual RPM at 2000us
#define MIN_PULSE_US   1100      // Pulse for minimum speed
#define MAX_PULSE_US   2000      // Pulse for maximum speed
#define ARM_PULSE_US   1000      // Pulse for arming (below minimum)
/* =============================================== */

#define PWM_PERIOD_US  20000
#define MAX_DUTY       65535

static const char *TAG = "ESC_CONTROL";
static uint32_t current_rpm = 0;

static uint32_t us_to_duty(uint32_t us)
{
    return (us * MAX_DUTY) / PWM_PERIOD_US;
}

static uint32_t rpm_to_pulse(uint32_t rpm)
{
    if (rpm < MIN_RPM) rpm = MIN_RPM;
    if (rpm > MAX_RPM) rpm = MAX_RPM;
    
    // Linear mapping: RPM -> pulse width
    uint32_t pulse = MIN_PULSE_US + ((rpm - MIN_RPM) * (MAX_PULSE_US - MIN_PULSE_US)) / (MAX_RPM - MIN_RPM);
    return pulse;
}

static void set_esc_rpm(uint32_t rpm)
{
    uint32_t pulse;
    
    if (rpm == 0) {
        // Stop/Arm position
        pulse = ARM_PULSE_US;
        current_rpm = 0;
    } else {
        if (rpm < MIN_RPM) rpm = MIN_RPM;
        if (rpm > MAX_RPM) rpm = MAX_RPM;
        
        current_rpm = rpm;
        pulse = rpm_to_pulse(rpm);
    }
    
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, us_to_duty(pulse));
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    ESP_LOGI(TAG, "ESC set to %lu RPM (%lu us)", current_rpm, pulse);
}

/* HTML page */
static const char html_page[] = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESC Control</title>"
"<style>"
"body{font-family:Arial;text-align:center;margin:50px;background:#1a1a1a;color:#fff}"
"h1{color:#4CAF50}"
".container{max-width:500px;margin:auto;padding:30px;background:#2d2d2d;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.3)}"
".slider-container{margin:30px 0}"
"input[type=range]{width:100%;height:8px;background:#555;outline:none;border-radius:5px}"
"input[type=range]::-webkit-slider-thumb{width:25px;height:25px;background:#4CAF50;cursor:pointer;border-radius:50%}"
".value-display{font-size:48px;color:#4CAF50;margin:20px 0;font-weight:bold}"
".rpm-label{font-size:24px;color:#888}"
".btn{padding:15px 30px;margin:10px;font-size:18px;cursor:pointer;border:none;border-radius:5px;background:#4CAF50;color:#fff}"
".btn:hover{background:#45a049}"
".btn-stop{background:#f44336}"
".btn-stop:hover{background:#da190b}"
".info{font-size:14px;color:#888;margin-top:20px}"
".pulse-info{font-size:12px;color:#666;margin-top:10px}"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>⚡ ESC Motor Control</h1>"
"<div class='value-display' id='rpmValue'>0</div>"
"<div class='rpm-label'>RPM</div>"
"<div class='pulse-info' id='pulseInfo'>Pulse: 1000 µs</div>"
"<div class='slider-container'>"
"<input type='range' min='0' max='10956' value='0' step='100' id='rpmSlider'>"
"</div>"
"<button class='btn btn-stop' onclick='stop()'>STOP</button>"
"<button class='btn' onclick='arm()'>ARM</button>"
"<div class='info'>Range: 0 (stopped) | 3700-10956 RPM</div>"
"</div>"
"<script>"
"const slider=document.getElementById('rpmSlider');"
"const display=document.getElementById('rpmValue');"
"const pulseDisplay=document.getElementById('pulseInfo');"
"function updatePulse(rpm){"
"let pulse;"
"if(rpm==0){pulse=1000;}"
"else if(rpm<3700){pulse=1100;}"
"else{pulse=1100+Math.round(((rpm-3700)/(10956-3700))*(2000-1100));}"
"pulseDisplay.textContent='Pulse: '+pulse+' µs';"
"}"
"slider.oninput=function(){"
"display.textContent=this.value;"
"updatePulse(this.value);"
"fetch('/set?rpm='+this.value);"
"};"
"function stop(){"
"slider.value=0;"
"display.textContent='0';"
"updatePulse(0);"
"fetch('/set?rpm=0');"
"}"
"function arm(){"
"slider.value=0;"
"display.textContent='0';"
"updatePulse(0);"
"display.textContent='0 (Arming...)';"
"fetch('/arm');"
"setTimeout(()=>{display.textContent='0';},3000);"
"}"
"</script>"
"</body>"
"</html>";

/* HTTP GET handler for root */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

/* HTTP GET handler for /set */
static esp_err_t set_get_handler(httpd_req_t *req)
{
    char buf[100];
    size_t buf_len;
    
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[32];
            if (httpd_query_key_value(buf, "rpm", param, sizeof(param)) == ESP_OK) {
                uint32_t rpm = atoi(param);
                set_esc_rpm(rpm);
            }
        }
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* HTTP GET handler for /arm */
static esp_err_t arm_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Arming ESC...");
    set_esc_rpm(0);
    httpd_resp_send(req, "Arming...", 9);
    return ESP_OK;
}

/* Start web server */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t set = {
            .uri = "/set",
            .method = HTTP_GET,
            .handler = set_get_handler
        };
        httpd_register_uri_handler(server, &set);

        httpd_uri_t arm = {
            .uri = "/arm",
            .method = HTTP_GET,
            .handler = arm_get_handler
        };
        httpd_register_uri_handler(server, &arm);
    }
    return server;
}

/* WiFi event handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

/* Initialize WiFi */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init complete");
}

void app_main(void)
{
    /* Initialize WiFi */
    wifi_init();

    /* Configure LEDC timer */
    ledc_timer_config_t timer_conf = {
        .speed_mode       = PWM_MODE,
        .timer_num        = PWM_TIMER,
        .duty_resolution  = PWM_RES,
        .freq_hz          = PWM_FREQ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    /* Configure LEDC channel */
    ledc_channel_config_t channel_conf = {
        .gpio_num   = ESC_PIN,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&channel_conf);

    /* Arm ESC */
    ESP_LOGI(TAG, "Arming ESC...");
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, us_to_duty(ARM_PULSE_US));
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(ARM_DELAY_MS));
    ESP_LOGI(TAG, "ESC armed");

    /* Keep running */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}







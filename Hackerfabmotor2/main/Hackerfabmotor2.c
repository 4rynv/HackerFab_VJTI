//w/o timer 
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

#define ESC_PIN        8
#define PWM_CHANNEL    LEDC_CHANNEL_0
#define PWM_TIMER      LEDC_TIMER_0
#define PWM_MODE       LEDC_LOW_SPEED_MODE
#define PWM_FREQ       50
#define PWM_RES        LEDC_TIMER_14_BIT
#define ARM_DELAY_MS   3000

#define MIN_RPM        0
#define MAX_RPM        15540
#define MIN_PULSE_US   1100
#define MAX_PULSE_US   2000
#define ARM_PULSE_US   1000

// Smooth transition settings
#define RPM_STEP       50        // RPM increment per step
#define STEP_DELAY_MS  50      // Delay between steps (ms)
/* =============================================== */

#define PWM_PERIOD_US  20000
#define MAX_DUTY       16383

static const char *TAG = "ESC_CONTROL";
static uint32_t current_rpm = 0;
static uint32_t target_rpm = 0;
static bool is_transitioning = false;
static TaskHandle_t transition_task_handle = NULL;

static uint32_t us_to_duty(uint32_t us)
{
    return (us * MAX_DUTY) / PWM_PERIOD_US;
}

static uint32_t rpm_to_pulse(uint32_t rpm)
{
    if (rpm < MIN_RPM) rpm = MIN_RPM;
    if (rpm > MAX_RPM) rpm = MAX_RPM;
    
    uint32_t pulse = MIN_PULSE_US + ((rpm - MIN_RPM) * (MAX_PULSE_US - MIN_PULSE_US)) / (MAX_RPM - MIN_RPM);
    return pulse;
}

static void set_esc_rpm_immediate(uint32_t rpm)
{
    uint32_t pulse;
    
    if (rpm == 0) {
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
}

/* Task for smooth RPM transition */
static void rpm_transition_task(void *pvParameters)
{
    is_transitioning = true;
    
    ESP_LOGI(TAG, "Starting transition from %lu to %lu RPM", current_rpm, target_rpm);
    
    while (current_rpm != target_rpm) {
        if (current_rpm < target_rpm) {
            // Accelerating
            if (target_rpm - current_rpm > RPM_STEP) {
                current_rpm += RPM_STEP;
            } else {
                current_rpm = target_rpm;
            }
        } else {
            // Decelerating
            if (current_rpm - target_rpm > RPM_STEP) {
                current_rpm -= RPM_STEP;
            } else {
                current_rpm = target_rpm;
            }
        }
        
        set_esc_rpm_immediate(current_rpm);
        ESP_LOGI(TAG, "Transitioning... Current RPM: %lu", current_rpm);
        vTaskDelay(pdMS_TO_TICKS(STEP_DELAY_MS));
    }
    
    ESP_LOGI(TAG, "Transition complete at %lu RPM", current_rpm);
    is_transitioning = false;
    transition_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_rpm_transition(uint32_t new_target_rpm)
{
    // Stop any existing transition
    if (transition_task_handle != NULL) {
        vTaskDelete(transition_task_handle);
        transition_task_handle = NULL;
        is_transitioning = false;
    }
    
    target_rpm = new_target_rpm;
    
    // If already at target, don't create task
    if (current_rpm == target_rpm) {
        ESP_LOGI(TAG, "Already at target RPM: %lu", target_rpm);
        return;
    }
    
    // Create transition task
    xTaskCreate(rpm_transition_task, "rpm_transition", 2048, NULL, 5, &transition_task_handle);
}

/* HTML page */
static const char html_page[] = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESC Control</title>"
"<style>"
"body{font-family:Arial;text-align:center;margin:50px;background:#1a1a1a;color:#fff}"
"h1{color:#4CAF50}"
".container{max-width:600px;margin:auto;padding:30px;background:#2d2d2d;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.3)}"
".control-section{margin:30px 0;padding:20px;background:#333;border-radius:8px}"
".value-display{font-size:56px;color:#4CAF50;margin:10px 0;font-weight:bold}"
".label{font-size:20px;color:#888;margin-bottom:15px}"
".control-row{display:flex;align-items:center;justify-content:center;gap:15px;margin:15px 0}"
".btn-control{width:60px;height:60px;font-size:32px;cursor:pointer;border:none;border-radius:50%;background:#4CAF50;color:#fff;font-weight:bold}"
".btn-control:hover{background:#45a049}"
".btn-control:active{transform:scale(0.95)}"
".btn{padding:15px 40px;margin:10px;font-size:18px;cursor:pointer;border:none;border-radius:8px;background:#4CAF50;color:#fff;font-weight:bold}"
".btn:hover{background:#45a049}"
".btn-stop{background:#f44336}"
".btn-stop:hover{background:#da190b}"
".btn-start{background:#2196F3}"
".btn-start:hover{background:#0b7dda}"
".btn-arm{background:#FF9800}"
".btn-arm:hover{background:#e68900}"
".info{font-size:14px;color:#666;margin-top:20px}"
".step-size{font-size:12px;color:#888;margin:5px 0}"
".status{font-size:16px;color:#FFA500;margin:10px 0;min-height:24px}"
"input[type='range']{width:100%;height:8px;background:#555;border-radius:5px;outline:none}"
"input[type='range']::-webkit-slider-thumb{width:24px;height:24px;background:#4CAF50;cursor:pointer;border-radius:50%}"
"input[type='range']::-moz-range-thumb{width:24px;height:24px;background:#4CAF50;cursor:pointer;border-radius:50%}"
".slider-container{margin:20px 0}"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>ESC Motor Control</h1>"
"<div class='control-section'>"
"<div class='label'>Target RPM</div>"
"<div class='value-display' id='targetValue'>0</div>"
"<div class='control-row'>"
"<button class='btn-control' onclick='changeTarget(-1000)'>--</button>"
"<button class='btn-control' onclick='changeTarget(-100)'>-</button>"
"<button class='btn-control' onclick='changeTarget(100)'>+</button>"
"<button class='btn-control' onclick='changeTarget(1000)'>++</button>"
"</div>"
"<div class='slider-container'>"
"<input type='range' min='0' max='15540' step='100' value='0' id='rpmSlider' oninput='updateTargetFromSlider(this.value)'>"
"</div>"
"<div class='step-size'>- / + : 100 RPM | -- / ++ : 1000 RPM</div>"
"</div>"
"<div class='control-section'>"
"<div class='label'>Current RPM</div>"
"<div class='value-display' id='currentValue' style='color:#FF9800'>0</div>"
"<div class='status' id='status'></div>"
"</div>"
"<div style='margin-top:30px'>"
"<button class='btn btn-start' onclick='startMotor()'>START</button>"
"<button class='btn btn-stop' onclick='stopMotor()'>STOP</button>"
"<button class='btn btn-arm' onclick='arm()'>ARM</button>"
"</div>"
"<div class='info'>Range: 0-15540 RPM (3S LiPo) | Smooth acceleration/deceleration</div>"
"</div>"
"<script>"
"let targetRPM=0;"
"let currentRPM=0;"
"const MIN_RPM=0;"
"const MAX_RPM=15540;"
"let statusInterval=null;"
"function updateDisplay(){"
"document.getElementById('targetValue').textContent=targetRPM;"
"document.getElementById('currentValue').textContent=currentRPM;"
"document.getElementById('rpmSlider').value=targetRPM;"
"}"
"function changeTarget(delta){"
"targetRPM=Math.max(MIN_RPM,Math.min(MAX_RPM,targetRPM+delta));"
"updateDisplay();"
"}"
"function updateTargetFromSlider(value){"
"targetRPM=parseInt(value);"
"updateDisplay();"
"}"
"function startMotor(){"
"if(targetRPM===0){"
"document.getElementById('status').textContent='⚠ Set target RPM > 0 first!';"
"return;"
"}"
"document.getElementById('status').textContent='🚀 Starting motor...';"
"fetch('/start?rpm='+targetRPM);"
"startStatusPolling();"
"}"
"function stopMotor(){"
"document.getElementById('status').textContent='🛑 Stopping motor...';"
"fetch('/stop');"
"targetRPM=0;"
"updateDisplay();"
"startStatusPolling();"
"}"
"function arm(){"
"targetRPM=0;"
"currentRPM=0;"
"updateDisplay();"
"document.getElementById('status').textContent='🔧 Arming ESC...';"
"fetch('/arm');"
"setTimeout(()=>{"
"document.getElementById('status').textContent='✓ ESC Armed';"
"setTimeout(()=>{document.getElementById('status').textContent='';},2000);"
"},3000);"
"}"
"function startStatusPolling(){"
"if(statusInterval)clearInterval(statusInterval);"
"statusInterval=setInterval(updateStatus,200);"
"}"
"function updateStatus(){"
"fetch('/status').then(r=>r.json()).then(data=>{"
"currentRPM=data.current_rpm;"
"document.getElementById('currentValue').textContent=currentRPM;"
"if(data.transitioning){"
"document.getElementById('status').textContent='⚙ Transitioning...';"
"}else{"
"if(currentRPM>0){"
"document.getElementById('status').textContent='✓ Running at '+currentRPM+' RPM';"
"}else{"
"document.getElementById('status').textContent='✓ Motor stopped';"
"}"
"if(statusInterval){"
"clearInterval(statusInterval);"
"statusInterval=null;"
"}"
"}"
"}).catch(err=>console.error('Status error:',err));"
"}"
"updateDisplay();"
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

/* HTTP GET handler for /start */
static esp_err_t start_get_handler(httpd_req_t *req)
{
    char buf[100];
    size_t buf_len;
    
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[32];
            if (httpd_query_key_value(buf, "rpm", param, sizeof(param)) == ESP_OK) {
                uint32_t rpm = atoi(param);
                if (rpm > MAX_RPM) rpm = MAX_RPM;
                start_rpm_transition(rpm);
            }
        }
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* HTTP GET handler for /stop */
static esp_err_t stop_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Stopping motor smoothly...");
    start_rpm_transition(0);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* HTTP GET handler for /status */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json_response[100];
    snprintf(json_response, sizeof(json_response), 
             "{\"current_rpm\":%lu,\"target_rpm\":%lu,\"transitioning\":%s}",
             current_rpm, target_rpm, is_transitioning ? "true" : "false");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));
    return ESP_OK;
}

/* HTTP GET handler for /arm */
static esp_err_t arm_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Arming ESC...");
    
    // Stop any transition
    if (transition_task_handle != NULL) {
        vTaskDelete(transition_task_handle);
        transition_task_handle = NULL;
        is_transitioning = false;
    }
    
    current_rpm = 0;
    target_rpm = 0;
    set_esc_rpm_immediate(0);
    
    httpd_resp_send(req, "Arming...", 9);
    return ESP_OK;
}

/* Start web server */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &root);

        httpd_uri_t start = {.uri = "/start", .method = HTTP_GET, .handler = start_get_handler};
        httpd_register_uri_handler(server, &start);

        httpd_uri_t stop = {.uri = "/stop", .method = HTTP_GET, .handler = stop_get_handler};
        httpd_register_uri_handler(server, &stop);

        httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler};
        httpd_register_uri_handler(server, &status);

        httpd_uri_t arm = {.uri = "/arm", .method = HTTP_GET, .handler = arm_get_handler};
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
    wifi_init();

    ledc_timer_config_t timer_conf = {
        .speed_mode       = PWM_MODE,
        .timer_num        = PWM_TIMER,
        .duty_resolution  = PWM_RES,
        .freq_hz          = PWM_FREQ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num   = ESC_PIN,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&channel_conf);

    ESP_LOGI(TAG, "Arming ESC...");
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, us_to_duty(ARM_PULSE_US));
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(ARM_DELAY_MS));
    ESP_LOGI(TAG, "ESC armed");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

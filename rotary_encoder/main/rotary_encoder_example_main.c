/*
 * ESP32 Quadrature Encoder RPM Measurement
 * Encoder: LPD3806-600
 * ESP-IDF v5+
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"

static const char *TAG = "ENCODER_RPM";

/* ---------------- CONFIG ---------------- */

#define ENCODER_GPIO_A 18
#define ENCODER_GPIO_B 19

#define ENCODER_PPR        600
#define QUADRATURE_FACTOR  4
#define COUNTS_PER_REV     (ENCODER_PPR * QUADRATURE_FACTOR)

#define SAMPLE_TIME_MS     1000   // 1 second

#define PCNT_HIGH_LIMIT    32767
#define PCNT_LOW_LIMIT    -32768

/* ---------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing PCNT unit");

    /* 1. Create PCNT unit */
    pcnt_unit_config_t unit_config = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit  = PCNT_LOW_LIMIT,
    };

    pcnt_unit_handle_t pcnt_unit = NULL;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &pcnt_unit));

    /* 2. Glitch filter (important for encoder noise) */
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 5000, // 5 µs
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config));

    /* 3. Channel A */
    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num  = ENCODER_GPIO_A,
        .level_gpio_num = ENCODER_GPIO_B,
    };

    pcnt_channel_handle_t chan_a = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_a_config, &chan_a));

    /* 4. Channel B */
    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num  = ENCODER_GPIO_B,
        .level_gpio_num = ENCODER_GPIO_A,
    };

    pcnt_channel_handle_t chan_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_b_config, &chan_b));

    /* 5. Quadrature decoding logic */
    ESP_ERROR_CHECK(
        pcnt_channel_set_edge_action(
            chan_a,
            PCNT_CHANNEL_EDGE_ACTION_DECREASE,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE));

    ESP_ERROR_CHECK(
        pcnt_channel_set_level_action(
            chan_a,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(
        pcnt_channel_set_edge_action(
            chan_b,
            PCNT_CHANNEL_EDGE_ACTION_INCREASE,
            PCNT_CHANNEL_EDGE_ACTION_DECREASE));

    ESP_ERROR_CHECK(
        pcnt_channel_set_level_action(
            chan_b,
            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    /* 6. Start PCNT */
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));

    ESP_LOGI(TAG, "PCNT started. Measuring RPM...");

    /* 7. RPM calculation variables */
    int count = 0;
    int last_count = 0;
    int delta_count = 0;
    float rpm = 0.0f;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_TIME_MS));

        ESP_ERROR_CHECK(pcnt_unit_get_count(pcnt_unit, &count));

        delta_count = count - last_count;
        last_count = count;

        rpm = ((float)delta_count / COUNTS_PER_REV) * 60.0f;

        ESP_LOGI(TAG,
                 "Total Count: %d | Delta: %d | RPM: %.2f",
                 count,
                 delta_count,
                 rpm);
    }
}


idf_component_register(
    SRCS "lvgllcd.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd driver esp_timer esp_lcd_ili9341
)

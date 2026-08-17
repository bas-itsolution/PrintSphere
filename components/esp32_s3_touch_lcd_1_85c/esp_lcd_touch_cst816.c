#include "esp_lcd_touch.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cst816";

#define CST816_DATA_REG (0x02)
#define CST816_GESTURE_REG (0x01)
#define CST816_CHIP_ID_REG (0xA7)
#define CST816_AUTOSLEEP_REG (0xFE)
#define CST816_MAX_POINTS (1)

static esp_err_t cst816_read_data(esp_lcd_touch_handle_t tp);
static bool cst816_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num,
                          uint8_t max_point_num);
static esp_err_t cst816_del(esp_lcd_touch_handle_t tp);

static esp_err_t cst816_i2c_read(esp_lcd_touch_handle_t tp, uint16_t reg,
                                 uint8_t *data, uint8_t len) {
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t cst816_i2c_write(esp_lcd_touch_handle_t tp, uint16_t reg,
                                  uint8_t *data, uint8_t len) {
    return esp_lcd_panel_io_tx_param(tp->io, reg, data, len);
}

static esp_err_t cst816_reset(esp_lcd_touch_handle_t tp) {
    if (tp->config.rst_gpio_num == GPIO_NUM_NC) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return ESP_OK;
    }

    gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t cst816_read_id(esp_lcd_touch_handle_t tp) {
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(cst816_i2c_read(tp, CST816_CHIP_ID_REG, &id, 1), TAG,
                        "chip id read failed");
    ESP_LOGI(TAG, "IC id: %u", id);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_cst816(const esp_lcd_panel_io_handle_t io,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *out_touch) {
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t touch = NULL;

    ESP_RETURN_ON_FALSE(io != NULL && config != NULL && out_touch != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "invalid args");

    touch = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE(touch != NULL, ESP_ERR_NO_MEM, err, TAG, "alloc failed");

    touch->io = io;
    touch->read_data = cst816_read_data;
    touch->get_xy = cst816_get_xy;
    touch->del = cst816_del;
    touch->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&touch->config, config, sizeof(*config));

    if (touch->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << touch->config.int_gpio_num,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = touch->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), err, TAG, "int gpio config failed");
    }

    if (touch->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << touch->config.rst_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), err, TAG, "rst gpio config failed");
    }

    ESP_GOTO_ON_ERROR(cst816_reset(touch), err, TAG, "reset failed");
    if (cst816_read_id(touch) != ESP_OK) {
        ESP_LOGW(TAG, "touch chip id unavailable; continuing with CST816-compatible reads");
    }

    uint8_t autosleep_off = 1;
    (void)cst816_i2c_write(touch, CST816_AUTOSLEEP_REG, &autosleep_off, 1);

    *out_touch = touch;
    return ESP_OK;

err:
    if (touch != NULL) {
        cst816_del(touch);
    }
    return ret;
}

static esp_err_t cst816_read_data(esp_lcd_touch_handle_t tp) {
    uint8_t point[5] = {};
    uint8_t point_num = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    static int log_reads_remaining = 12;
    static int log_failures_remaining = 8;

    if (tp->config.int_gpio_num != GPIO_NUM_NC &&
        gpio_get_level(tp->config.int_gpio_num) != tp->config.levels.interrupt) {
        taskENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 0;
        taskEXIT_CRITICAL(&tp->data.lock);
        return ESP_OK;
    }

    esp_err_t read_err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; ++attempt) {
        read_err = cst816_i2c_read(tp, CST816_DATA_REG, point, sizeof(point));
        if (read_err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (read_err != ESP_OK) {
        if (log_failures_remaining > 0) {
            ESP_LOGW(TAG, "point read failed while INT active: %s", esp_err_to_name(read_err));
            --log_failures_remaining;
        }
        taskENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 0;
        taskEXIT_CRITICAL(&tp->data.lock);
        return ESP_OK;
    }
    point_num = point[0] & 0x0F;
    if (point_num > 0 && point_num <= CST816_MAX_POINTS) {
        x = (((uint16_t)point[1] & 0x0F) << 8) | point[2];
        y = (((uint16_t)point[3] & 0x0F) << 8) | point[4];
    }

    if (point_num == 0 || point_num > CST816_MAX_POINTS ||
        x >= tp->config.x_max || y >= tp->config.y_max) {
        uint8_t alt[6] = {};
        if (cst816_i2c_read(tp, CST816_GESTURE_REG, alt, sizeof(alt)) == ESP_OK) {
            const uint8_t alt_num = alt[1] & 0x0F;
            const uint16_t alt_x = (((uint16_t)alt[2] & 0x0F) << 8) | alt[3];
            const uint16_t alt_y = (((uint16_t)alt[4] & 0x0F) << 8) | alt[5];
            if (alt_num > 0 && alt_num <= CST816_MAX_POINTS &&
                alt_x < tp->config.x_max && alt_y < tp->config.y_max) {
                point_num = alt_num;
                x = alt_x;
                y = alt_y;
            }
            if (log_reads_remaining > 0 && (point_num > 0 || alt_num > 0)) {
                ESP_LOGI(TAG,
                         "touch raw 02=[%02x %02x %02x %02x %02x] 01=[%02x %02x %02x %02x %02x %02x] -> n=%u x=%u y=%u",
                         point[0], point[1], point[2], point[3], point[4],
                         alt[0], alt[1], alt[2], alt[3], alt[4], alt[5],
                         point_num, x, y);
                --log_reads_remaining;
            }
        }
    } else if (log_reads_remaining > 0) {
        ESP_LOGI(TAG, "touch raw 02=[%02x %02x %02x %02x %02x] -> n=%u x=%u y=%u",
                 point[0], point[1], point[2], point[3], point[4], point_num, x, y);
        --log_reads_remaining;
    }

    if (point_num > CST816_MAX_POINTS || x >= tp->config.x_max || y >= tp->config.y_max) {
        point_num = 0;
    }

    taskENTER_CRITICAL(&tp->data.lock);
    tp->data.points = point_num;
    if (tp->data.points > 0) {
        tp->data.coords[0].x = x;
        tp->data.coords[0].y = y;
        tp->data.coords[0].strength = 1;
    }
    taskEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool cst816_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num,
                          uint8_t max_point_num) {
    taskENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points > max_point_num ? max_point_num : tp->data.points;
    for (size_t i = 0; i < *point_num; ++i) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength != NULL) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    taskEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

static esp_err_t cst816_del(esp_lcd_touch_handle_t tp) {
    if (tp == NULL) {
        return ESP_OK;
    }
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    free(tp);
    return ESP_OK;
}

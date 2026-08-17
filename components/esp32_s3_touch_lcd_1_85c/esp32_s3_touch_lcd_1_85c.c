#include "bsp/esp32_s3_touch_lcd_1_85c.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_st77916.h"

static const char *TAG = "ESP32-S3-Touch-LCD-1.85C";

#define BSP_LCD_QSPI_PCLK_HZ (40 * 1000 * 1000)
#define BSP_LCD_DRAW_BUFFER_HEIGHT (16)
#define BSP_LCD_BACKLIGHT_TIMER LEDC_TIMER_0
#define BSP_LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BSP_LCD_BACKLIGHT_RESOLUTION LEDC_TIMER_13_BIT
#define BSP_LCD_BACKLIGHT_MAX_DUTY ((1 << BSP_LCD_BACKLIGHT_RESOLUTION) - 1)
#ifndef CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH
#define CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH 1
#endif

#define BSP_EXIO_TOUCH_RESET IO_EXPANDER_PIN_NUM_0
#define BSP_EXIO_LCD_RESET IO_EXPANDER_PIN_NUM_1
#define BSP_EXIO_SD_CS IO_EXPANDER_PIN_NUM_2

#define LCD_OPCODE_WRITE_CMD (0x02UL)

static i2c_master_bus_handle_t s_i2c_handle = NULL;
static bool s_i2c_initialized = false;
static bool s_backlight_initialized = false;
static esp_io_expander_handle_t s_io_expander = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_panel_io_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev = NULL;
static uint8_t s_brightness = 0;

sdmmc_card_t *bsp_sdcard = NULL;

extern esp_err_t esp_lcd_touch_new_i2c_cst816(const esp_lcd_panel_io_handle_t io,
                                              const esp_lcd_touch_config_t *config,
                                              esp_lcd_touch_handle_t *out_touch);

static uint32_t qspi_lcd_cmd(uint8_t cmd) {
    return ((uint32_t)LCD_OPCODE_WRITE_CMD << 24) | ((uint32_t)cmd << 8);
}

static esp_err_t expander_set_level(uint32_t pin_mask, uint8_t level) {
    esp_io_expander_handle_t io = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(io != NULL, ESP_FAIL, TAG, "io expander unavailable");
    return esp_io_expander_set_level(io, pin_mask, level);
}

static esp_err_t io_expander_prepare_outputs(void) {
    esp_io_expander_handle_t io = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(io != NULL, ESP_FAIL, TAG, "io expander init failed");
    const uint32_t mask = BSP_EXIO_TOUCH_RESET | BSP_EXIO_LCD_RESET | BSP_EXIO_SD_CS;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io, mask, IO_EXPANDER_OUTPUT), TAG,
                        "set expander dir failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, mask, 1), TAG,
                        "set expander defaults failed");
    return ESP_OK;
}

static esp_err_t lcd_reset_sequence(void) {
    ESP_RETURN_ON_ERROR(io_expander_prepare_outputs(), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(expander_set_level(BSP_EXIO_LCD_RESET, 0), TAG, "lcd reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(expander_set_level(BSP_EXIO_LCD_RESET, 1), TAG, "lcd reset high failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t touch_reset_sequence(void) {
    ESP_RETURN_ON_ERROR(io_expander_prepare_outputs(), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(expander_set_level(BSP_EXIO_TOUCH_RESET, 0), TAG, "touch reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(expander_set_level(BSP_EXIO_TOUCH_RESET, 1), TAG, "touch reset high failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static lv_display_rotation_t lv_rotation_for(bsp_display_rotation_t rotation) {
    switch (rotation) {
        case BSP_DISPLAY_ROTATE_90:
            return LV_DISPLAY_ROTATION_90;
        case BSP_DISPLAY_ROTATE_180:
            return LV_DISPLAY_ROTATION_180;
        case BSP_DISPLAY_ROTATE_270:
            return LV_DISPLAY_ROTATION_270;
        case BSP_DISPLAY_ROTATE_0:
        default:
            return LV_DISPLAY_ROTATION_0;
    }
}

static esp_err_t apply_touch_flags(const bsp_display_cfg_t *cfg) {
    if (s_touch_handle == NULL || cfg == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_swap_xy(s_touch_handle, cfg->touch_flags.swap_xy != 0), TAG,
                        "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_x(s_touch_handle, cfg->touch_flags.mirror_x != 0), TAG,
                        "mirror_x failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_y(s_touch_handle, cfg->touch_flags.mirror_y != 0), TAG,
                        "mirror_y failed");
    return ESP_OK;
}

esp_err_t bsp_i2c_init(void) {
    if (s_i2c_initialized) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_NUM,
        .scl_io_num = BSP_I2C_SCL,
        .sda_io_num = BSP_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags =
            {
                .enable_internal_pullup = true,
            },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_handle), TAG, "i2c init failed");
    s_i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void) {
    if (!s_i2c_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(i2c_del_master_bus(s_i2c_handle), TAG, "i2c deinit failed");
    s_i2c_handle = NULL;
    s_i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
    if (bsp_i2c_init() != ESP_OK) {
        return NULL;
    }
    return s_i2c_handle;
}

esp_io_expander_handle_t bsp_io_expander_init(void) {
    if (s_io_expander != NULL) {
        return s_io_expander;
    }
    if (bsp_i2c_init() != ESP_OK) {
        return NULL;
    }
    if (esp_io_expander_new_i2c_tca9554(s_i2c_handle, BSP_IO_EXPANDER_I2C_ADDRESS,
                                        &s_io_expander) != ESP_OK) {
        return NULL;
    }
    return s_io_expander;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io) {
    if (s_panel_handle != NULL) {
        if (ret_panel != NULL) {
            *ret_panel = s_panel_handle;
        }
        if (ret_io != NULL) {
            *ret_io = s_panel_io_handle;
        }
        return ESP_OK;
    }

    const int max_transfer_sz = config != NULL && config->max_transfer_sz > 0
                                    ? config->max_transfer_sz
                                    : BSP_LCD_H_RES * BSP_LCD_DRAW_BUFFER_HEIGHT *
                                          BSP_LCD_BITS_PER_PIXEL / 8;

    ESP_RETURN_ON_ERROR(lcd_reset_sequence(), TAG, "lcd reset failed");

    const spi_bus_config_t bus_config =
        ST77916_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2,
                                      BSP_LCD_DATA3, max_transfer_sz);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.pclk_hz = BSP_LCD_QSPI_PCLK_HZ;
    io_config.trans_queue_depth = CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config,
                                 &s_panel_io_handle),
        TAG, "new panel io failed");

    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_panel_io_handle, &panel_config, &s_panel_handle),
                        TAG, "new st77916 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "display on failed");

    if (ret_panel != NULL) {
        *ret_panel = s_panel_handle;
    }
    if (ret_io != NULL) {
        *ret_io = s_panel_io_handle;
    }
    return ESP_OK;
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg) {
    const uint32_t buffer_height = BSP_LCD_DRAW_BUFFER_HEIGHT;
    const size_t max_transfer_sz = BSP_LCD_H_RES * buffer_height * BSP_LCD_BITS_PER_PIXEL / 8;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = (int)max_transfer_sz,
    };
    if (bsp_display_new(&disp_config, &s_panel_handle, &s_panel_io_handle) != ESP_OK) {
        ESP_LOGE(TAG, "display new failed");
        return NULL;
    }

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel_handle,
        .panel_io = s_panel_io_handle,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = cfg->rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = buffer_height,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };

    ESP_LOGI(TAG, "LVGL display buffers: %ux%u buffer_height=%" PRIu32 " max_transfer=%u qspi=%uMHz",
             BSP_LCD_H_RES, BSP_LCD_V_RES, buffer_height, (unsigned)max_transfer_sz,
             (unsigned)(BSP_LCD_QSPI_PCLK_HZ / 1000000));
    return esp_lv_adapter_register_display(&disp_cfg);
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp) {
    if (touch_reset_sequence() != ESP_OK) {
        ESP_LOGE(TAG, "touch reset failed");
        return NULL;
    }

    const esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = 0x15,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags =
            {
                .disable_control_phase = 1,
            },
        .scl_speed_hz = 100000,
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_FALSE(bsp_i2c_get_handle() != NULL, NULL, TAG, "i2c handle unavailable");
    if (esp_lcd_new_panel_io_i2c(s_i2c_handle, &tp_io_config, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io init failed");
        return NULL;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = cfg->touch_flags.swap_xy,
                .mirror_x = cfg->touch_flags.mirror_x,
                .mirror_y = cfg->touch_flags.mirror_y,
            },
    };
    if (esp_lcd_touch_new_i2c_cst816(tp_io, &tp_cfg, &s_touch_handle) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed");
        return NULL;
    }

    const esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch_handle);
    return esp_lv_adapter_register_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void) {
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg != NULL, NULL, TAG, "cfg is null");
    if (s_display != NULL) {
        return s_display;
    }

    if (esp_lv_adapter_init(&cfg->lv_adapter_cfg) != ESP_OK) {
        return NULL;
    }
    s_display = bsp_display_lcd_init(cfg);
    if (s_display == NULL) {
        return NULL;
    }
    s_indev = bsp_display_indev_init(cfg, s_display);
    if (s_indev == NULL) {
        ESP_LOGW(TAG, "touch unavailable; continuing with display-only UI");
    }
    if (bsp_display_brightness_init() != ESP_OK) {
        return NULL;
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    (void)apply_touch_flags(cfg);
    return s_display;
}

lv_indev_t *bsp_display_get_input_dev(void) {
    return s_indev;
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation) {
    if (s_display == NULL || s_panel_io_handle == NULL) {
        return ESP_OK;
    }
    if (rotation == BSP_DISPLAY_ROTATE_0) {
        return ESP_OK;
    }

    uint8_t madctl = 0x00;
    switch (rotation) {
        case BSP_DISPLAY_ROTATE_180:
            madctl = 0xC0;
            break;
        case BSP_DISPLAY_ROTATE_0:
        case BSP_DISPLAY_ROTATE_90:
        case BSP_DISPLAY_ROTATE_270:
        default:
            madctl = 0x00;
            break;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io_handle, qspi_lcd_cmd(0x36), &madctl, 1),
                        TAG, "MADCTL failed");

    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(1000), TAG, "display lock failed");
    lv_display_set_rotation(s_display, lv_rotation_for(rotation));
    esp_lv_adapter_unlock();
    return ESP_OK;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms) {
    return esp_lv_adapter_lock((int32_t)timeout_ms);
}

void bsp_display_unlock(void) {
    esp_lv_adapter_unlock();
}

static uint32_t backlight_duty_for_percent(int brightness_percent) {
    const int clamped = brightness_percent < 0 ? 0 : (brightness_percent > 100 ? 100 : brightness_percent);
    if (clamped == 0) {
        return 0;
    }
    return BSP_LCD_BACKLIGHT_MAX_DUTY - (81U * (100U - (uint32_t)clamped));
}

esp_err_t bsp_display_brightness_init(void) {
    if (s_backlight_initialized) {
        return ESP_OK;
    }
    const gpio_config_t gpio_config_backlight = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_config_backlight), TAG, "backlight gpio config failed");

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BSP_LCD_BACKLIGHT_TIMER,
        .duty_resolution = BSP_LCD_BACKLIGHT_RESOLUTION,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "backlight timer config failed");

    const ledc_channel_config_t channel_config = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_LCD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "backlight channel config failed");

    s_backlight_initialized = true;
    return bsp_display_brightness_set(70);
}

esp_err_t bsp_display_brightness_set(int brightness_percent) {
    ESP_RETURN_ON_FALSE(s_backlight_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "backlight not initialized");
    const int clamped = brightness_percent < 0 ? 0 : (brightness_percent > 100 ? 100 : brightness_percent);
    s_brightness = (uint8_t)clamped;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_BACKLIGHT_CHANNEL,
                                      backlight_duty_for_percent(clamped)),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_BACKLIGHT_CHANNEL), TAG,
                        "update duty failed");
    return ESP_OK;
}

int bsp_display_brightness_get(void) {
    return s_brightness;
}

esp_err_t bsp_display_backlight_on(void) {
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_backlight_off(void) {
    return bsp_display_brightness_set(0);
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void) {
    ESP_LOGW(TAG, "Audio codec is not ported on the 1.85C board yet");
    return NULL;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void) {
    return NULL;
}

esp_err_t bsp_audio_init(const void *i2s_config) {
    (void)i2s_config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_spiffs_mount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_spiffs_unmount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_mount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_unmount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

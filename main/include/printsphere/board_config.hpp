#pragma once

#include "driver/gpio.h"

namespace printsphere::board {

#if defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)
constexpr int kDisplayWidth = 466;
constexpr int kDisplayHeight = 466;

constexpr gpio_num_t kI2cScl = GPIO_NUM_14;
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_11;
constexpr gpio_num_t kTouchReset = GPIO_NUM_40;

constexpr gpio_num_t kQspiClk = GPIO_NUM_38;
constexpr gpio_num_t kQspiData0 = GPIO_NUM_4;
constexpr gpio_num_t kQspiData1 = GPIO_NUM_5;
constexpr gpio_num_t kQspiData2 = GPIO_NUM_6;
constexpr gpio_num_t kQspiData3 = GPIO_NUM_7;

constexpr int kAxp2101Address = 0x34;
constexpr char kBoardName[] = "Waveshare ESP32-S3 AMOLED 1.75";
#elif defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_16;
constexpr gpio_num_t kTouchReset = GPIO_NUM_NC;

constexpr gpio_num_t kQspiClk = GPIO_NUM_41;
constexpr gpio_num_t kQspiData0 = GPIO_NUM_5;
constexpr gpio_num_t kQspiData1 = GPIO_NUM_45;
constexpr gpio_num_t kQspiData2 = GPIO_NUM_48;
constexpr gpio_num_t kQspiData3 = GPIO_NUM_47;

constexpr int kAxp2101Address = 0;
constexpr char kBoardName[] = "ESP32-S3-Touch-LCD-2.8C";
#elif defined(PRINTSPHERE_HW_VARIANT_LCD_1_85C)
constexpr int kDisplayWidth = 360;
constexpr int kDisplayHeight = 360;

constexpr gpio_num_t kI2cScl = GPIO_NUM_10;
constexpr gpio_num_t kI2cSda = GPIO_NUM_11;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_4;
constexpr gpio_num_t kTouchReset = GPIO_NUM_NC;

constexpr gpio_num_t kQspiClk = GPIO_NUM_40;
constexpr gpio_num_t kQspiData0 = GPIO_NUM_46;
constexpr gpio_num_t kQspiData1 = GPIO_NUM_45;
constexpr gpio_num_t kQspiData2 = GPIO_NUM_42;
constexpr gpio_num_t kQspiData3 = GPIO_NUM_41;

constexpr int kAxp2101Address = 0;
constexpr char kBoardName[] = "ESP32-S3-Touch-LCD-1.85C Rev2.0";
#else
#error "Unknown PrintSphere hardware variant"
#endif

}  // namespace printsphere::board

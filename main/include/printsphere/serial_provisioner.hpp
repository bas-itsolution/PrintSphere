#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace printsphere {

class ConfigStore;
class WifiManager;

// Implements the open Improv Serial protocol on the USB/UART console. ESP Web
// Tools detects this protocol after a factory flash and can pass Wi-Fi
// credentials directly over the USB cable, so joining the fallback setup AP is
// no longer required for a new installation.
class SerialProvisioner {
 public:
  SerialProvisioner(ConfigStore& config_store, WifiManager& wifi_manager);

  esp_err_t start();

 private:
  enum class Transport : uint8_t { kUart, kUsbSerialJtag };
  struct Parser {
    std::array<uint8_t, 265> bytes{};
    size_t length = 0;
  };

  static void task_entry(void* context);
  void task();
  void process_byte(Parser& parser, Transport transport, uint8_t byte);
  void process_packet(Transport transport, const uint8_t* packet, size_t packet_length);
  void process_rpc(Transport transport, const uint8_t* data, size_t data_length);

  void send_state(Transport transport);
  void broadcast_state();
  void send_error(Transport transport, uint8_t error);
  void send_rpc_result(Transport transport, uint8_t command,
                       const std::vector<std::string>& values);
  void send_packet(Transport transport, uint8_t type, const uint8_t* data, size_t data_length);
  void on_wifi_connected();
  void on_wifi_connection_timeout();
  std::string device_url() const;

  ConfigStore& config_store_;
  WifiManager& wifi_manager_;
  Parser uart_parser_{};
  Parser usb_parser_{};
  bool uart_available_ = false;
  bool usb_available_ = false;
  bool provisioning_pending_ = false;
  TickType_t provisioning_started_at_ = 0;
};

}  // namespace printsphere

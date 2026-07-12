# PrintSphere v1.6.2

Patch release on top of v1.6.1 focused on reliable local MQTT connectivity in
mesh networks, WPA2/WPA3 compatibility, camera-page layout, and simpler first
time setup. OTA-compatible with v1.6.1 (no partition table change).

## Release Scope

- **Base**: v1.6.1. Devices on v1.6 or older should follow the existing v1.6
  full-flash/OTA upgrade path.
- **Toolchain target**: ESP-IDF v5.5.4, LVGL v9.5.0.
- **Hardware variants**: Waveshare ESP32-S3 Touch AMOLED 1.75 and ESP32-S3
  Touch LCD 2.8C.

## Major Fixes

- **Local MQTT connects reliably in Local Only and Hybrid mode**: Local MQTT
  is no longer gated by printer camera type or by the cloud session being
  online. X1C/RTSP printers and printers whose cloud status is temporarily
  unavailable can still establish their local MQTT connection.
- **Removed the pre-MQTT TCP probe**: PrintSphere now starts the MQTT/TLS
  handshake directly and relies on esp-mqtt's transport diagnostics and
  reconnect handling. The separate probe could fail on transient mesh paths
  or interfere with the subsequent broker connection.
- **Mesh steering and WPA2/WPA3 transition networks**: The station no longer
  pins itself to a scanned BSSID or channel. ESP-IDF performs all-channel
  RSSI-based AP selection while leaving the connection available for
  802.11k/v BSS steering. WPA3 SAE/H2E and 802.11k/v support are enabled in
  the project defaults.
- **Camera snapshot status overlap**: On the camera page, battery-powered
  devices show the battery indicator in the shared header position. USB-only
  devices continue to show the print status, without the battery overlay
  obscuring it.
- **USB Wi-Fi setup after a factory flash**: The installer now uses Improv
  Serial to scan nearby Wi-Fi networks and transfer credentials over the USB
  cable. The PrintSphere setup hotspot remains available as a fallback.
- **OTA update entry point on the install page**: Select the hardware variant,
  enter the device IP address, and the page opens the device's OTA updater with
  the matching image selected. OTA writes the inactive app slot and retains
  the existing configuration.

## Internal Changes

- Local MQTT and the JPEG camera transport now have separate network-ready
  gates; camera activation remains page- and model-dependent.
- Removed the custom Wi-Fi scan/BSSID lock and the associated retry cleanup.
- Added explicit ESP-IDF station settings for all-channel RSSI selection,
  WPA3 SAE-H2E, and 802.11k/v roaming support.

## Known Notes

- The actual roaming decision remains under control of the access point. Mesh
  steering must be enabled and configured consistently across the router and
  repeaters.
- Existing saved Wi-Fi credentials are unchanged. After updating, the station
  may initially associate with a different mesh access point because BSSID and
  channel are no longer persisted as a lock.

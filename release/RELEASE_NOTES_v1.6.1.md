# PrintSphere v1.6.1

Patch release on top of v1.6 focused on display stability during prints, power-save behavior, and local MQTT status accuracy. OTA-compatible with v1.6 (no partition table change).

## Release Scope

- **Base**: v1.6. Devices already on v1.6 can update via OTA; older devices still need the one-time v1.6 full flash first.
- **Toolchain target**: ESP-IDF v5.5.4, LVGL v9.5.0.
- **Display adapter**: `espressif/esp_lvgl_adapter` v0.6.0 with two narrow local patches (GPIO-TE TX-done timeout, LVGL worker scheduling window).

## Major Features

- **Bambu Lab X2D support**: The new X2-series flagship is now recognized (product name "Bambu Lab X2D", local and cloud detection) with the correct capability profile: RTSP camera, chamber temperature, dual nozzle (L/R display), primary + secondary chamber light, local status via Developer Mode, cloud preferred. The V2 status protocol (`extruder.info[].snow`, `vir_slot`, `nozzle.info[]`) and the embedded X2-family TLS CA were already in place, so full functionality comes with the model entry alone.
- **Bambu Lab A2L support**: The A2-series entry model is recognized and mapped to the A1-style capability profile (JPEG chamber camera, local status without Developer Mode).
- **HMS/error text database refreshed**: Synced against the current Bambu API (version 202607031835): 51 new HMS codes and 7 new print error codes, including the new extruder-clog variants and G-code verification errors introduced with the latest printer generation.
- **Hardware variant builds**: The firmware now builds per hardware variant via `-DPRINTSPHERE_HW_VARIANT=...` (e.g. `amoled_1_75`, `lcd_2_8c`) with separate build directories.
- **ESP32-S3 Touch LCD 2.8C port (experimental)**: Initial support for the Waveshare ESP32-S3 Touch LCD 2.8C (RGB panel, double full-frame buffering).

## Major Fixes

- **White screen on AMOLED fixed**: The 2.8C port had hard-coded a tear-avoid mode the AMOLED panel path rejects, silently falling back to a known-bad buffer configuration. The tear-avoid mode is now selected per hardware variant (AMOLED 1.75: TE_SYNC, LCD 2.8C: DOUBLE_FULL).
- **UI freeze during prints fixed (LVGL lock starvation)**: With continuous animations (e.g. the pulsing status ring) the LVGL worker re-acquired its mutex within microseconds of releasing it, so the application task could fail its lock for minutes and status/progress on screen froze while the printer kept printing. The worker now yields a short scheduling window after every unlock (local adapter patch). Verified during a real print with camera streaming and cloud preview: zero lock failures.
- **Screen dimming during prints works again**: A v1.6 regression kept the display permanently awake while printing. Keep-awake now only prevents display-off and selects the active-print timeouts, so the Web Config "Dim after (during print)" settings are honored again.
- **Local MQTT status no longer sticks on prep stages**: Stages like "clean nozzle" or "preparing" could latch indefinitely because some printers do not resend the stage field in partial reports. Stale prep stages are now invalidated when layer or progress advances, and a `pushall` is requested every 60 s during a live job to resynchronize full state.
- **Hybrid local/cloud handoff**: While the local MQTT connection is being established, cloud traffic is paused for the handoff window so the TLS handshake gets heap and bandwidth headroom; the cloud session stays warm for preview and account features.
- **Smoother page swipes**: Releasing a swipe now snaps with an animated ease-out instead of a hard jump, a new touch no longer aborts the snap animation, and a drag beyond ~20 % of the screen width advances to the next page instead of snapping back.

## Internal Changes

- `esp_lv_adapter.c` (managed component) carries a second local patch: a 2 ms scheduling window after each LVGL worker unlock, preventing cross-core mutex starvation of the application task. Must be re-applied after any adapter component update.
- Stale-stage invalidation, periodic `pushall` (`kPeriodicPushallMs = 60000`), and hybrid handoff ordering added in `printer_client.cpp` / `application.cpp`.
- Pager swipe logic (`ui.cpp`) reworked: animated snap via `lv_obj_scroll_to_x(LV_ANIM_ON)`, press guard, gesture-origin tracking with advance threshold.
- AMOLED BSP draw-buffer defines documented: under TE_SYNC the adapter always allocates a full-frame draw buffer; `LVGL_BUFFER_HEIGHT_PSRAM` only sizes `max_transfer_sz` and the fallback path.
- `PrinterModel` gained `kX2D` and `kA2L` (appended, capability table + `static_assert` updated); product-name detection extended in `bambu_status.cpp`.
- HMS tooling hardened: `tools/hms_merge.ps1` and `tools/check_sorted.ps1` now split the TSV sections dynamically instead of relying on hard-coded line ranges, and the refresh workflow guards against a PowerShell 5.1 UTF-8 decoding pitfall that could have corrupted non-ASCII characters (e.g. `°C`).

## Known Notes

- Between periodic `pushall` responses the raw stage field can alternate between `printing` and an empty placeholder in partial reports. This is cosmetic: lifecycle and displayed status remain `printing`.
- `esp_lvgl_adapter` v0.6.2 is available upstream but is intentionally not adopted in this release: its changes do not affect PrintSphere's code paths, and updating would drop the local worker-scheduling patch.
- X2D and A2L support is based on the published protocol behavior of these models (identical V2 payload structures as H2/P2S and A1 respectively); it has not yet been verified against physical devices. The X2D Developer-Mode requirement for local status mirrors the H2 series policy and is a one-line capability-table change if it turns out to differ.


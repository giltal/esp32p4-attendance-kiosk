# ESP32-P4 Attendance Kiosk

Student attendance system (entry / exit) for a small educational institute, running on the
**Guition JC1060P470C_I_W_Y** — an ESP32-P4 board with a 7" 1024×600 touch display, MIPI-CSI
camera, SD card, RTC and Wi-Fi (ESP32-C6 via ESP-Hosted).

Students check in and out by **face recognition** (on-device, camera always on) or a **fingerprint
sensor** (UART). The UI is in Hebrew (right-to-left).

## Features

- **Face recognition at the door** — ESP-DL face detection + face signatures, live camera view on the
  main screen, 3 consecutive matches required, per-student cooldown. Only signatures are stored
  (`faces.bin` on the SD card), never images.
- **Fingerprint** — driver for ID809-type modules (DFRobot SEN0348 protocol) with auto-detection of
  baud rate and swapped TX/RX; R503 (Grow) protocol notes and diagnostics included.
- **Attendance logic** — automatic IN/OUT toggle with a configurable minimum time between entry and
  exit (default 30 min), automatic check-out at end of day, state survives reboots (today's log is
  replayed at boot).
- **Records on the SD card** — `students.csv`, daily logs `logs/YYYY-MM-DD.csv`, monthly reports
  `reports/YYYY-MM.csv` (UTF-8 with BOM, open directly in Excel).
- **Admin screen (PIN protected)** — students (add, rename, enroll fingers / face, delete), today's
  attendance with absentees, monthly report (on screen + export, auto-export on the 1st), settings
  (Wi-Fi, NTP time, time zone, in/out gap, face recognition on/off, volume, PIN), sensor tools,
  camera preview.

## Hardware

| Part | Notes |
|---|---|
| Guition JC1060P470C_I_W_Y | ESP32-P4 (rev 1.3), 32 MB PSRAM, 16 MB flash, JD9165 LCD, GT911 touch |
| Camera | OV02C10 on MIPI-CSI (on the board), 1288×728 @ 30 fps |
| Fingerprint sensor (optional) | 3.3 V UART module; default pins TX=GPIO3, RX=GPIO4 (`main/board_config.h`) |
| microSD card | FAT32 — all data is stored here |

Fingerprint wiring for the module family used here (connector position 1→6, check your module's
datasheet — clone colour codes differ!): red VCC 3.3 V, black GND, yellow TX → GPIO4,
green RX ← GPIO3, blue WAKEUP (optional, GPIO5), white 3.3VT → 3.3 V. **Never use 5 V.**

## Build & flash

Requires **ESP-IDF v5.5.2**.

```sh
idf.py set-target esp32p4      # first time only (sdkconfig is generated from sdkconfig.defaults)
idf.py build
idf.py -p <PORT> flash monitor
```

Component versions are pinned in `main/idf_component.yml` / `dependencies.lock` — in particular
`esp_hosted` 2.12.8 and `lvgl` 9.5.0 (newer versions broke boot / config on this board).
After changing pins, delete `sdkconfig` before building.

On first boot: tap the gear icon, PIN **1234**, set Wi-Fi and change the PIN in *Settings*.

## Repository layout

| Path | Contents |
|---|---|
| `main/` | Application: BSP drivers (`bsp_*`), attendance, students, reports, fingerprint (`fp_*`), camera / face (`camera`, `face_*`), UI (`ui_*`) |
| `main/board_config.h` | All pin assignments and feature flags |
| `components/espressif__esp_cam_sensor/` | esp_cam_sensor 1.2.1 with the OV02C10 driver (vendored, from Guition's package — not in the registry) |
| `fonts_src/`, `tools/gen_fonts.sh` | Alef font sources and the script that regenerates `main/fonts/` |
| `devlog.md` | Detailed development log: decisions, hardware findings, pitfalls |

## Notes

- **Privacy:** face signatures and fingerprints of minors are biometric data — obtain consent and keep
  the SD card secure.
- **Photo spoofing:** the camera cannot tell a real face from a photo; combine with fingerprints or
  supervision where that matters.
- Board documentation (schematics, datasheets) is available from Guition and is not included here.

## Credits & licenses

- Application code: MIT (see `LICENSE`).
- esp_cam_sensor / OV02C10 driver: Apache-2.0 (Espressif Systems; OV02C10 support from Guition).
- Alef font: SIL Open Font License 1.1, © HaGilda & Mushon Zer-Aviv (`fonts_src/OFL.txt`).
- Uses ESP-IDF, LVGL, esp_video, ESP-DL (human_face_detect / human_face_recognition) and other
  Espressif components under their respective licenses (fetched by the component manager).
- ID809 protocol details from DFRobot's open-source `DFRobot_ID809` library.

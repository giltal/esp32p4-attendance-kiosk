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
- **Fingerprint** — GROW (R503-style) protocol driver, tested with the **FPM383F** (57600 baud,
  50 templates): 3-capture enrollment, 2 fingers per student, identification, duplicate check.
  Auto-detects baud rate and swapped TX/RX; a boot-time protocol scan (GROW / FPM383 / ID809)
  helps identify unknown modules.
- **Attendance logic** — automatic IN/OUT toggle, any number of visits per day, a configurable minimum
  time between entry and exit (default 30 min; re-entry after an exit needs only 1 min), automatic
  check-out at end of day, state survives reboots (today's log is replayed at boot).
- **Presence per student** — entries, exits and total time inside, today (admin screen) and per month
  (report + export; each day's visits are listed, e.g. `08:05-10:00 | 11:00-14:30 (05:25)`).
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
| Fingerprint sensor (optional) | FPM383F (or other GROW-protocol module), 3.3 V UART; board TX=GPIO4, RX=GPIO3 (`main/board_config.h`) |
| microSD card | FAT32 — all data is stored here |

Fingerprint wiring (expansion header, by **function** — check your module's own pinout, colour codes
differ between clones): VCC → 3V3, GND → GND, sensor RX ← GPIO4, sensor TX → GPIO3, touch/wake output
(optional) → GPIO5, touch supply (if present) → 3V3. TX/RX swapped is detected automatically.
**3.3 V only — never 5 V.** Unknown module? Set `PIN_DIAG 1` in `main/main.c` to run the protocol scan.

## Wall-mount enclosure

3D-printable wall mount for the display and the fingerprint sensor, in `mechanical/` (STL, mm). The
screen tilts 0–30° and is locked with two hand-turned knob screws, so no tools are needed to adjust it.
The parts have been printed and fit-checked on the cased JC1060P470C_I_W_Y with the FPM383F.

| Part | Qty | Size (mm) | Notes |
|---|---|---|---|
| `Housing.stl` | 1 | 189 × 169 × 19 | Bezel with screen window, camera hole (6 mm below the top edge), FPM383F pocket below the screen |
| `BackCover.stl` | 1 | 189 × 169 × 29 | Screws onto the display's 4 standoffs. Opening for the back case keeps the USB-C, RJ45, header and speaker reachable. Carries the threaded hinge lugs |
| `WallBracket.stl` | 1 | 190 × 90 × 32 | 2 keyholes plus 1 safety screw, cable hole to the wall, hinge cheeks |
| `TiltKnob.stl` | 2 | Ø36 × 34 | Printed M16×2 knob screw: loosen, tilt, tighten |
| `SensorClamp.stl` | 1 | 42 × 19 × 7 | Holds the FPM383F in its pocket from inside |

**Printing (PLA, 0.2 mm layers)**
- **Housing:** face down.
- **Back cover:** lugs up.
- **Wall bracket:** wall side down (the keyhole recesses face up).
- **Knobs:** head down, thread vertical, 100 % infill.
- Give the bracket and the back cover at least 4 walls and 40 % infill. They carry the load.

**Hardware**

| Item | Qty | Use |
|---|---|---|
| M2 × 6–8 screws | 4 | Back cover into the display's standoffs (M2 inserts, 166 × 99 pattern) |
| M3 × 8 self-tapping screws | 4 | Back cover into the housing |
| M2 × 8 screws | 2 | Sensor clamp |
| Wall screws (head up to Ø8, shank up to Ø4.5) | 3 | Wall bracket, with wall plugs |
| Right-angle USB-C cable | 1 | The housing stands 34 mm off the wall |

**Assembly**
1. Seat the FPM383F from the front: its lip goes into the recess, the body through the hole. Fix it from inside with the clamp, turning the sensor so its connector lines up with the clamp window.
2. Place the display face down in the housing.
3. Screw the back cover onto the 4 standoffs (M2), then to the housing corners (M3).
4. Plug the sensor wires into the pin header. They pass through the notch at the edge of the back-case opening.
5. Hang the wall bracket on the keyhole screws and add the bottom safety screw.
6. Lower the housing so its two lugs sit just inside the bracket cheeks. Screw a knob from the outside through each cheek into its lug, set the angle and tighten by hand.

For face recognition, a tilt of 0–10° works best. At larger angles the camera looks over people's heads unless
the unit is mounted low.

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
| `mechanical/` | Wall-mount enclosure STL files (see *Wall-mount enclosure*) |
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

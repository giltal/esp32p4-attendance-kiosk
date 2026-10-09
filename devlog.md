# Fingerprint Attendance — Development Log

## Project Overview
Student entry/exit attendance system for a small educational institute.
- **Board**: Guition JC1060P470C_I_W_Y (ESP32-P4 + ESP32-C6 WiFi via ESP-Hosted), 7" 1024×600 touch
- **Sensor**: R503 capacitive fingerprint module (UART, 57600 baud, 200 templates, RGB ring LED)
- **Framework**: ESP-IDF v5.5.2 (`C:\Users\97254\esp\v5.5.2\esp-idf`), LVGL 9 via `esp_lvgl_port`
- **Board support**: copied from `C:\ESPIDFprojects\ESP32P4_7_HebClock` (display, touch, audio, RTC, SD, WiFi) — see its devlog for bring-up history and memory lessons.

## Behaviour (defaults chosen 2026-10-08)
- **Auto IN/OUT toggle**: a student's first scan of the day = IN, next = OUT, alternating.
  Repeat scans within `scan_gap` seconds (default 60) are ignored ("already registered").
- **Records on SD card** as CSV (UTF-8 with BOM so Excel shows Hebrew):
  - `/sd/students.csv` — `id,finger1,finger2,name` (finger = sensor slot, -1 = none)
  - `/sd/logs/YYYY-MM-DD.csv` — `date,time,id,direction,name` (IN / OUT / AUTO_OUT)
  - `/sd/config.txt` — settings (key=value)
- Today's log is replayed at boot, so reboots don't lose who is inside. State resets at midnight.
- **Admin screen** (gear button → PIN, default `1234`): students (add / rename / enroll 2 fingers / delete),
  today's report (incl. absentees, "mark everyone out"), settings (institute name, PIN, WiFi, TZ/DST,
  NTP, scan gap, volume, manual clock), sensor (status, orphan cleanup, wipe). Auto-closes after 3 min idle.
- Hebrew UI (RTL), Hebrew/English/digits on-screen keyboard.

## Wiring (R503 → board expansion header)
| Our module's wire (connector pos.) | Signal | Board pin |
|---|---|---|
| Red (1) | VCC 3.3V | 3V3 |
| Black (2) | GND | GND |
| Yellow (3) | TXD (out of sensor) | GPIO4 (P4 RX) |
| Green (4) | RXD (into sensor) | GPIO3 (P4 TX) |
| Blue (5) | WAKEUP | GPIO5 (optional, diagnostic only) |
| White (6) | 3.3VT | 3V3 |

Our module follows the standard R503 datasheet. Other clones use different colours (one listing had red = GND).
Go by function. The driver auto-detects swapped TX/RX and the baud rate (57600/9600/115200/19200/38400),
and logs the raw bytes of every probe. **Never feed 5V.**

## Source layout (main/)
| File | Role |
|---|---|
| `fp_sensor.c/h` | R503 UART packet protocol driver (GenImg, Img2Tz, Search, Store, RegModel, LED…) |
| `fp_service.c/h` | Task owning the sensor: scan loop, enrollment state machine, request queue → events |
| `student_db.c/h` | Student registry (thread-safe, atomic tmp+rename saves) |
| `attendance.c/h` | Day state, IN/OUT toggle, CSV log, replay, midnight reset |
| `settings_store.c/h` | `/sd/config.txt` (atomic save) |
| `ui_common.c/h` | Fonts w/ symbol fallback, palette, Hebrew keyboard, modal/confirm/toast/PIN |
| `ui_main.c/h` | Kiosk screen: clock, result card, present list, status footer |
| `ui_admin.c/h` | Admin tabview |
| `bsp_*.c/h`, `board_config.h` | Board support (from HebClock) + fingerprint pin config |

## Threading rules
- Only `fp_task` talks to the sensor. UI posts requests via `fp_service_*()` (non-blocking queue).
- `fp_task` emits events with no DB/attendance lock held; the callback takes the LVGL lock.
- UI code never blocks on the fp task → no deadlock between LVGL lock and sensor.

## Build & Flash
```powershell
& "C:\Users\97254\esp\v5.5.2\esp-idf\export.ps1"
idf.py -p COM11 flash monitor
```

## Log
### 2026-10-08 — v0.1.0 initial implementation
- Project scaffolded from HebClock BSP; full app written. Not yet tested on hardware (sensor not wired).
- Built & flashed to COM11 (app 1.6 MB, 61% partition free). Boots: display, touch, audio, RTC, SD, WiFi OK.
- **Boot-loop fix**: `assert failed: sdio_mempool_create` — ~95 KB of static arrays (students, attendance records)
  ate internal RAM that ESP-Hosted SDIO needs at startup. Fixed with `EXT_RAM_BSS_ATTR` (→ PSRAM).
  Rule: any large static buffer must be `EXT_RAM_BSS_ATTR`.
- With the sensor unplugged, floating RX (GPIO33) picked up TX (GPIO32) by crosstalk → driver saw its own
  command echoed. Fixed: RX pull-up + driver skips echoed command packets.
- The SD card still holds HebClock's `config.txt`; the shared keys (WiFi, timezone, DST, NTP) are reused.
- Sensor UART moved to GPIO1/GPIO2 (header rows 4-5) at the user's request.
- First wiring attempt: no bytes at any baud or pin order. The module's colour scheme turned out to be non-standard
  (red = GND, green = VCC, black = RX). Added auto-detection of pin order and baud.
- Checked the connector order on the module itself: standard datasheet pinout (red = VCC, black = GND).
- Jumper-loopback tests were misleading: the sensor's yellow (TX) wire was attached to the header at the same time,
  holding a line high. **Lesson: test pins one at a time (drive/readback) with nothing else attached.**
- Single-pin drive/readback (`pin_diag_selftest`): all header GPIOs 1-5, 20, 32, 33, 45-47 work normally.
  The "GPIO1/2 unusable" and "LP-IO bank" theories were wrong.
- Final pins: TX=GPIO3 -> green, RX=GPIO4 <- yellow. Schematic: GPIO1/2 go only to the header.
- **First R503 is dead**: wiring confirmed (black = GND by diode test), board TX verified on the GPIO3 pad, yet
  zero edges on its TX, WAKEUP never reacts to touch (GPIO5 watch), ring never lights. Replacing the sensor.
- Diagnostics kept in `pin_diag.c`; set `PIN_DIAG 1` in main.c to run them at boot
  (TX pad self-readback, RX edge count/baud estimate, WAKEUP watch on GPIO5).
- Schematic & spec PDFs saved in `board_docs/` (from GitHub mirrors of Guition's package).

### 2026-10-08 (evening) — sensor investigation, no working sensor yet
- The AliExpress listing describes an **ID809** module (DFRobot SEN0348 type: 80 templates, 160x160, IDfinger6.0),
  not an R503. Rewrote `fp_sensor.c` for the ID809 protocol from DFRobot's library
  (115200 8N1, 26-byte AA 55 frames, little-endian, IDs 1..80, 3-capture enroll + merge).
  `fp_service.c` / UI updated for 3 captures. Builds clean; **untested against a live sensor**.
- Pinout confirmed from a photo of the module connector: standard R503 datasheet order (red VCC, black GND,
  yellow TX, green RX, blue WAKEUP, white 3.3VT). An AliExpress table (red=GND, green=VCC) was wrong for
  this module; wiring by it made sensor #2 hot (3V3 -> RX protection diode -> VCC at GND). Sensor #2 likely damaged.
- Sensor #1, correctly wired: silent with both R503 and ID809 protocols, both pin orders, 12 baud rates.
  Board TX verified on the GPIO3 pad; sensor TX line never toggles; ring never lights. Treated as dead.
- GPIO4 could not be driven low while the sensor was attached (unusually strong for a TX output) — check
  yellow<->red resistance on a future module if it is silent.
- New sensors (with better documentation) ordered. When they arrive: check pinout and protocol from their docs;
  set `PIN_DIAG 1` in main.c for the line probe (TX pad readback, RX edge count, touch watch on GPIO5).
- Lesson: never rewire power pins based on an unverified seller table — confirm GND with a diode test on the
  module itself, and check for heat within seconds of first power-up.

### 2026-10-08/09 (night) — Camera stage 1: live preview ✅
- Camera: **OV02C10** on MIPI-CSI (1 lane), SCCB on the shared I2C bus (GPIO7/8, addr 0x36, 100 kHz),
  own 24 MHz oscillator, no reset/pwdn pins. Uses the display's MIPI PHY LDO (ch3, 2.5 V).
- Stack: Guition's vendored `components/espressif__esp_cam_sensor` (1.2.1 + OV02C10 driver, not in the registry)
  + registry `esp_video` 1.1.0 (+ esp_ipa 1.0.1) — the same versions as Guition's IDF 5.5 `video_lcd_display` demo.
  Mode 1288x728 RAW10 30fps (1080p on 1 lane overflows the ISP on P4 rev <3). ISP -> RGB565, IPA JSON (AE/AWB) is
  the real default config (luma target 79).
- **Pin component versions!** Adding esp_video made the component manager upgrade esp_hosted 2.12.8 -> 2.12.13
  (boot-loop: `assert sdio_mempool_create`) and LVGL 9.5 -> 9.6 (sdkconfig mismatch). idf_component.yml now pins
  esp_hosted 2.12.8, esp_wifi_remote 1.6.0, esp_lvgl_port 2.8.0~1, lvgl 9.5.0. After changing pins: delete sdkconfig.
- `camera.c`: init once (never deinit — known to hang), MMAP x2, task with STREAMON/OFF on demand.
  **After STREAMOFF all buffers must be re-queued before STREAMON** (V4L2 semantics) — otherwise no frames.
- `ui_camera.c`: admin tab "מצלמה": PPA scale x0.625 -> 805x455 (mirrored), ~10 fps cap, two PSRAM buffers
  swapped into an lv_canvas.
- Headless self-test results (CAMERA_SELFTEST=1): 30 fps capture; PPA scale 43 ms/frame; stop/start OK;
  on-screen preview 10 s: ~9 fps display, camera ~22 fps meanwhile, no WDT, heap stable (int 93 KB, PSRAM 26 MB).
  Thumbnails are dumped over serial (`CAMTHUMB:`) and can be rendered to PNG (scratch script thumb2png.py).
- Not yet verified (room was dark at night): image quality / colour order (if colours look wrong toggle PPA
  `rgb_swap`), orientation, mirror direction.
- Next: stage 2 — face detection (ESP-DL / ESP-WHO models for P4).

### 2026-10-09 — Camera stage 2: face detection ✅ (device side)
- `espressif/human_face_detect` 0.5.0 (+ esp-dl 3.3.13, dl_fft, esp_new_jpeg). Model MSR+MNP S8 v1 in flash rodata.
  Pinned versions held (esp_hosted 2.12.8, lvgl 9.5.0) — check NOTICE lines after any dependency change.
- App grew to ~4.5 MB: **factory partition enlarged 0x3F0000 -> 0x7F0000 (8 MB)**; NVS offset unchanged.
- `face_detect.cpp`: C wrapper around HumanFaceDetect (RGB565LE input). Detection runs in the camera task on the
  805x455 scaled + mirrored preview frame, so boxes map 1:1 onto the canvas (canvas base_dir LTR, boxes are
  its children). Up to 4 green boxes; "זיהוי פנים" button toggles detection.
- Measured: model load 78 ms, detection 28 ms/frame, preview ~8 fps with detection, internal heap ~72 KB free, stable.
- Gotcha: editing files with PowerShell Get-Content/Set-Content without -Encoding utf8 turns "—" into "â€”"
  (and adds a BOM). Use the Edit tool or Python with explicit UTF-8.
- Next: stage 3 — face recognition (embeddings per student, stored on SD), enrollment flow, attendance hookup.
- Fix: camera start failed intermittently on the first press after boot — the OV02C10 answered its first SCCB
  reads with zeros ("Camera sensor is not OV02C10, PID=0x0"), and esp_video_init() can't be re-run (retries then
  failed with "video name=ISP id=20 has been registered"). Now: `sensor_wake_probe()` polls the chip ID
  (0x300A/0x300B, up to 10x50 ms) before the one-shot esp_video_init(); all hardware init runs in the camera
  task (the start button is non-blocking) and errors are shown under the preview.

### 2026-10-09 — Camera stage 3: face recognition at the door ✅
- `espressif/human_face_recognition` 0.3.2 (requires human_face_detect ~0.4.1 -> detect pinned to 0.4.2).
  Feature model MFN S8 v1 (rodata, 512-float L2-normalised signature, ~93 ms/face; loads in ~450 ms on first use).
  App now 5.8 MB of the 8 MB partition.
- `face_recog.cpp`: signature extraction (detected face + 5 keypoints). `face_db.c`: own signature store
  `/sd/faces.bin` keyed by student id (5 samples/student, atomic save, entries of deleted students dropped on load).
- `face_service.c` (runs in the camera task, ~6 analysed frames/s): PPA x0.5 working image (644x364, not mirrored)
  + x0.25 mirrored view (322x182) with the face box drawn in (white=far/none, yellow=checking, green=match,
  red=unknown). Kiosk: largest face >= 60 px wide -> signature -> best match; accept after 3 consecutive matches
  >= 0.55, per-student 8 s cooldown, "face not recognised" after 6 unmatched frames (5 s cooldown). Accepted scans go
  through attendance_register() and the same fp_event_t path as fingerprints (source = FP_SRC_FACE).
  Enroll: one face >= 70 px, 5 samples 400 ms apart, duplicate check against other students, 30 s timeout.
- Camera ownership: kiosk on while the main screen is shown (setting "זיהוי פנים בכניסה", default on); admin screen
  takes it (camera tab / enrollment) and hands it back on close. Camera task stack raised to 12 KB.
- First live test: enrolled, recognised with similarity 0.59–0.80, entry registered; repeats within the scan gap
  correctly reported as "already registered".
- Added: reset reason logged at every boot (one unexplained reboot was seen right after adding a student — not
  reproduced; check "Reset reason" if it happens again).
- Gotcha: the Bash tool turns "\n" inside heredoc Python patches into a real newline — write patch scripts with the
  Write tool instead.
- Ideas next: tune thresholds with more students/lighting, liveness (photo spoofing) mitigation, enrollment from the
  main screen for new students, export reports.
- Sensor #1 bench test on a CP2102 (COM8): red/black = 3.3 V, data wired per datasheet. Valid ID809 and R503 frames
  at all bauds only came back as an electrical echo (TXD->green->sensor->yellow->RXD, garbled at 115200); no real
  reply packet. Touching produced single bytes (FC at 115200, 00 at 9600/19200) = a touch pulse leaking onto yellow
  while white (3.3VT) was unpowered; with white powered the leak stops. Conclusion: processor dead -> wait for new
  sensors. Bench probe script: scratchpad fpprobe.py (both protocols; beware echo false-positives).
- Fix: times next to names showed swapped ("24:09" for 09:24). LVGL BiDi in an RTL context keeps digit runs but
  treats ':' as a neutral separator, so it reverses the two groups. Present-list time labels are now base_dir LTR;
  the admin report table is LTR with reversed column order (COL_* macros) and right-aligned cells.
  **Rule: any label/cell that shows a time must be LTR.**

### 2026-10-09 — Monthly reports ✅
- `report.c`: builds a month from the daily logs (IN -> OUT/AUTO_OUT pairs): days present, total time inside,
  average first arrival, days with no exit (today excluded). Includes current students + anyone in that month's
  logs (deleted students keep their logged name). Data in PSRAM.
- Export `/sd/reports/YYYY-MM.csv` (UTF-8 BOM, atomic): id, name, days, total HH:MM, avg arrival, missing exits,
  then one column per day ("08:05-14:30" / "08:05-?").
- Admin: "דוח חודשי" button on the today tab -> dialog with month navigation, LTR table (reversed columns, MCOL_*),
  "יצוא לכרטיס SD". Auto-export of the previous month on the 1st (ui_main day-change handler).

### 2026-10-09 — In/out policy
- Setting "זמן בין כניסה ליציאה" (min time between a student's IN and OUT): 1/5/10/15/30/60/120 min,
  **default 30 min**. Stored as `inout_gap` (seconds) — the old `scan_gap` key is ignored so the new default applies.
  Scans inside the gap = "already registered"; the card shows when they entered and when exit is allowed.
- OUT card shows the entry time ("יציאה - להתראות! (נכנסת ב-HH:MM)").
- `ui_fmt_time_in_rtl()`: times embedded in Hebrew sentences are written minutes-first so LVGL BiDi shows HH:MM.
- **Auto check-out at end of day:** on day rollover everyone still inside gets a `DAY_END` exit at 23:59:59; at boot
  yesterday's open entries are closed too (device off at midnight). Reports treat DAY_END as "exit not recorded"
  (no hours). Admin "mark everyone out" stays AUTO_OUT at the real time (hours counted).
- "Goodbye although not inside" report: replay showed all events were the right person — the always-on camera
  recognised the user while passing/working near the screen (IN/OUT flips 10:09, 10:11, 10:12 with the old 60 s gap).
  Fixes: 30 min gap, MIN_FACE_W 60 -> 80 px (work image; ~160 px full frame), face width logged per recognition.
- Boot now logs every replayed event of today (`ATTENDANCE:   replay HH:MM:SS #id DIR name`).

# Smart Monitor

A tiny external system monitor: ESP32‑C3 + 128×64 SH1106 OLED driven over USB serial by a Python host script. Clean black‑and‑white UI with subtle animations and a cute Tamagotchi‑style face reacting to system load.

## ✨ Features
- Two‑column Macintosh‑style layout on 128×64 OLED (SH1106, I2C)
- Header shows temperature and the current active app name (macOS)
- Left column: compact CPU and RAM gauges with easing
- Right column: Tamagotchi face with blink/wink/sweat/head‑bob; sleep mode on disconnect or prolonged low load
- Bottom ticker with CPU, free RAM, disk free, and uptime
- Robust serial JSON protocol; resilient parsing with ArduinoJson

## 🧰 Hardware
- ESP32‑C3 development board (Arduino framework via PlatformIO)
- 1.3" 128×64 OLED, SH1106 controller, I²C address 0x3C

Wiring (I²C):
- 3V3 → OLED VCC
- GND → OLED GND
- Board SDA → OLED SDA
- Board SCL → OLED SCL

Notes:
- The sketch uses `Wire.begin()` with your board’s default SDA/SCL. Adjust if needed.
- SH1106 address is 0x3C (7‑bit). If your module uses another address, update the init in `src/main.cpp`.

## 🧪 Firmware (ESP32‑C3)
This is a PlatformIO project. Required libraries are fetched automatically:
- Adafruit_GFX
- Adafruit_SH110X
- ArduinoJson (v6)

Build/flash from the project root:

```bash
# Build
platformio run

# Upload to the board
platformio run --target upload

# View serial logs (115200 baud)
platformio device monitor
```

If the screen stays blank at boot you’ll still get logs on the serial monitor (JSON errors, etc.).

## 🖥️ Host bridge (Python)
The host script collects system metrics and optional weather, then streams one JSON object per line to the MCU.

Setup (Python 3.9+ recommended):

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r tools/requirements.txt
```

Run:

```bash
# macOS example (find your port first: ls /dev/tty.usb*)
python tools/host_bridge.py --port /dev/tty.usbmodemXXXX --lat 48.8566 --lon 2.3522 --verbose
```

Useful flags:
- `--baud` serial speed (default 115200)
- `--interval` seconds between updates (default 2)
- `--lat/--lon` to enable weather; omit to skip weather

macOS: the script also sends the active app name via AppleScript. On Linux/Windows the field may be omitted.

## 🔌 Serial protocol
Line‑delimited JSON (UTF‑8). Example:

```json
{"cpu":23.4,"ram":16329872,"ram_used":8234567,
 "weather":{"temp":21.3,"desc":"Cloudy"},
 "host":"my-mac","time":1723200000,"uptime":54321,
 "disk_free":1024_000,"net":{"rx":120.5,"tx":80.2},
 "app":"Electron"}
```

Known fields (optional unless noted):
- `cpu` number 0–100 (required for gauges)
- `ram` and `ram_used` in KB (used to compute RAM bar and free MB in ticker)
- `weather.temp` in °C (header/ticker)
- `host`, `time` (epoch seconds), `uptime` (seconds)
- `disk_free` in KB
- `net.rx`/`net.tx` in KB/s (used for an adaptive network scale internally)
- `app` active app name (macOS)

The firmware copes with missing fields and keeps previous values where sensible.

## 🖼️ UI overview
- Header: inverted bar with temperature (left) and active app name (centered)
- Left column: CPU and RAM progress bars (compact, retro look)
- Right column: Tamagotchi face
	- Blink (periodic), wink (occasional), sweat (under high load), subtle head bob
	- Sleep mode when no data for a few seconds or sustained low load
- Bottom ticker: scrolling line with temperature, CPU, free RAM, disk free and uptime

## ⚙️ Configuration
Edit `src/main.cpp` to tweak:
- I²C address or display controller init
- Animation timings (blink/wink/sweat cadence, frame cap ~16 FPS)
- Sleep thresholds and durations
- Ticker cadence and content

## 🧰 Troubleshooting
- Nothing on screen
	- Check power and I²C wiring (SDA/SCL). SH1106 address should be 0x3C.
	- Open the serial monitor at 115200 to inspect logs (JSON parse errors, etc.).
- Garbled or overlapping text
	- Ensure your display is SH1106 (not SSD1306). If SSD1306, switch library/init accordingly.
- No active app name
	- macOS only. Ensure Accessibility/Automation permissions allow AppleScript to query the frontmost app.
- Weather not showing
	- Omit `--lat/--lon` to disable, or provide valid coordinates and network connectivity.

## 📂 Project layout
```
platformio.ini
include/
lib/
src/
	main.cpp
test/
tools/
	host_bridge.py
	requirements.txt
```

## 🙏 Acknowledgements
- Adafruit GFX and SH110X libraries
- ArduinoJson by Benoit Blanchon
- psutil/pyserial on the host side

## 📜 License
Choose a license that fits your use (MIT is a good default). If missing, add a `LICENSE` file to the repo.

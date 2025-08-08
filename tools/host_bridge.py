#!/usr/bin/env python3
"""
Host bridge script for Smart Monitor
- Collects CPU, RAM and Weather from the host (macOS/Linux/Windows)
- Sends compact JSON lines to the MCU over a serial port.

Data schema (one line per update):
{
  "cpu": float (0-100),
  "ram": int total_kb,
  "ram_used": int used_kb,
  "weather": { "temp": float, "desc": str }
}

Requirements:
- Python 3.9+
- pip install -r tools/requirements.txt

Usage:
  python tools/host_bridge.py --port /dev/tty.usbmodemXXXX --lat 48.8566 --lon 2.3522

Notes:
- Weather provider: Open-Meteo (no API key). If network fails, weather fields are omitted.
- On macOS, the ESP32-C3 often appears as /dev/tty.usbmodem* or /dev/tty.usbserial*.
"""
from __future__ import annotations
import argparse
import json
import sys
import time
from dataclasses import dataclass
import subprocess
import platform
import socket
from typing import Optional
import threading

TRAY_AVAILABLE = False
try:
    # rumps provides a simple macOS status bar app
    import rumps  # type: ignore
    TRAY_AVAILABLE = (platform.system() == "Darwin")
except Exception:
    TRAY_AVAILABLE = False

import psutil
import serial  # pyserial
import requests
from serial.serialutil import SerialException

try:
    from serial_utils import autodetect_port
except Exception:
    # Fallback local import path when run outside repo root
    try:
        from tools.serial_utils import autodetect_port  # type: ignore
    except Exception:
        autodetect_port = lambda preferred=None: None  # noqa: E731

OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"

@dataclass
class Weather:
    temp: Optional[float]
    desc: str
    code: Optional[int]


def get_weather(lat: float, lon: float, timeout: float = 4.0) -> Weather:
    params = {
        "latitude": lat,
        "longitude": lon,
        "current": ["temperature_2m", "weather_code"],
        "timezone": "auto",
    }
    try:
        r = requests.get(OPEN_METEO_URL, params=params, timeout=timeout)
        r.raise_for_status()
        data = r.json()
        current = data.get("current", {})
        temp = current.get("temperature_2m")
        code = current.get("weather_code")
        desc = WEATHER_CODE_MAP.get(code, "") if code is not None else ""
        return Weather(temp=temp, desc=desc, code=code)
    except Exception:
        return Weather(temp=None, desc="", code=None)


WEATHER_CODE_MAP = {
    # Simplified mapping of WMO weather codes
    0: "Clair",
    1: "Plutôt clair",
    2: "Partiellement nuageux",
    3: "Couvert",
    45: "Brouillard",
    48: "Brouillard givrant",
    51: "Bruine légère",
    53: "Bruine",
    55: "Bruine forte",
    61: "Pluie faible",
    63: "Pluie",
    65: "Pluie forte",
    71: "Neige faible",
    73: "Neige",
    75: "Neige forte",
    95: "Orage",
}


def get_system_stats() -> tuple[float, int, int]:
    # CPU percent averaged over 0.5s
    cpu = psutil.cpu_percent(interval=0.5)

    vm = psutil.virtual_memory()
    total_kb = int(vm.total / 1024)
    used_kb = int((vm.total - vm.available) / 1024)
    return cpu, total_kb, used_kb


def get_disk_free_kb() -> int:
    try:
        du = psutil.disk_usage("/")
        return int(du.free / 1024)
    except Exception:
        return -1


def get_active_app_name_macos() -> Optional[str]:
    if platform.system() != "Darwin":
        return None
    try:
        # AppleScript pour récupérer l’app au premier plan
        script = 'tell application "System Events" to get name of application processes whose frontmost is true'
        r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=1.5)
        if r.returncode == 0:
            out = r.stdout.strip()
            # Peut retourner une liste séparée par ", "; prendre le premier nom non vide
            if out:
                first = out.split(", ")[0].strip()
                return first if first else None
    except Exception:
        pass
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Smart Monitor host bridge")
    parser.add_argument("--port", required=False, help="Serial port, e.g. /dev/tty.usbmodemXXXX. If omitted, autodetect.")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--lat", type=float, required=False, help="Latitude for weather")
    parser.add_argument("--lon", type=float, required=False, help="Longitude for weather")
    parser.add_argument("--interval", type=float, default=2.0, help="Update interval seconds")
    parser.add_argument("--verbose", action="store_true", help="Print debug info and each payload sent")
    parser.add_argument("--tray", action="store_true", help="Run as a macOS tray app (status bar). Requires rumps.")
    args = parser.parse_args()

    # If tray requested, try it; on failure or unavailability, fall back to headless bridge
    if args.tray:
        if TRAY_AVAILABLE:
            rc = run_tray(args)
            if rc == 0:
                return 0
            if args.verbose:
                print("[host_bridge] Tray failed/unavailable at runtime; falling back to headless mode")
        else:
            if args.verbose:
                print("[host_bridge] Tray not available on this platform; falling back to headless mode")

    # Connect/reconnect loop
    ser = None
    preferred = args.port
    last_port_print = 0.0
    while ser is None:
        port = autodetect_port(preferred)
        if port is None:
            if args.verbose and (time.time() - last_port_print > 3):
                print("[host_bridge] Waiting for device... (no serial ports found)")
                last_port_print = time.time()
            time.sleep(1.0)
            continue
        try:
            ser = serial.Serial(port, args.baud, timeout=1)
            if args.verbose:
                print(f"[host_bridge] Connected: {port} @ {args.baud}")
            time.sleep(2.0)
            preferred = port
        except Exception as e:
            if args.verbose:
                print(f"[host_bridge] Unable to open {port}: {e}")
            ser = None
            time.sleep(1.0)

    last_weather: Optional[Weather] = None
    last_weather_ts = 0.0
    # Net IO baseline for rates
    last_net = psutil.net_io_counters()
    last_net_ts = time.time()

    try:
        while True:
            cpu, total_kb, used_kb = get_system_stats()
            disk_free_kb = get_disk_free_kb()

            payload = {
                "cpu": round(cpu, 1),
                "ram": total_kb,
                "ram_used": used_kb,
            }

            # Host/time/uptime
            try:
                payload["host"] = socket.gethostname()
            except Exception:
                pass
            payload["time"] = int(time.time())
            try:
                payload["uptime"] = int(time.time() - psutil.boot_time())
            except Exception:
                pass
            if disk_free_kb >= 0:
                payload["disk_free"] = disk_free_kb

            # Network RX/TX rate (KB/s)
            try:
                now = time.time()
                cur = psutil.net_io_counters()
                dt = max(0.1, now - last_net_ts)
                rx_rate = (cur.bytes_recv - last_net.bytes_recv) / 1024.0 / dt
                tx_rate = (cur.bytes_sent - last_net.bytes_sent) / 1024.0 / dt
                payload["net"] = {"rx": round(rx_rate, 1), "tx": round(tx_rate, 1)}
                last_net, last_net_ts = cur, now
            except Exception:
                pass

            # Application active (macOS)
            try:
                app = get_active_app_name_macos()
                if app:
                    payload["app"] = app
            except Exception:
                pass

            # Refresh weather every 5 minutes
            now = time.time()
            if args.lat is not None and args.lon is not None and (now - last_weather_ts > 300 or last_weather is None):
                last_weather = get_weather(args.lat, args.lon)
                last_weather_ts = now

            if last_weather is not None:
                w = {}
                if last_weather.temp is not None:
                    w["temp"] = round(float(last_weather.temp), 1)
                if last_weather.desc:
                    w["desc"] = last_weather.desc
                if last_weather.code is not None:
                    try:
                        w["wcode"] = int(last_weather.code)
                    except Exception:
                        pass
                if w:
                    payload["weather"] = w

            line = json.dumps(payload, separators=(",", ":")) + "\n"
            if args.verbose:
                print(f"[host_bridge] TX: {line.strip()}")
            try:
                ser.write(line.encode("utf-8"))
                ser.flush()
            except Exception as e:
                if args.verbose:
                    print(f"[host_bridge] Serial write failed: {e}. Reconnecting...")
                # Close and attempt full reconnect loop
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                # wait and re-enter connect loop
                time.sleep(1.0)
                while ser is None:
                    port = autodetect_port(preferred)
                    if port is None:
                        time.sleep(1.0)
                        continue
                    try:
                        ser = serial.Serial(port, args.baud, timeout=1)
                        if args.verbose:
                            print(f"[host_bridge] Reconnected: {port} @ {args.baud}")
                        time.sleep(1.5)
                        preferred = port
                    except Exception:
                        ser = None
                        time.sleep(1.0)

            time.sleep(max(0.1, args.interval))
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.close()
        except Exception:
            pass

    return 0


# ------------------------- Tray Mode (macOS) -------------------------
def run_tray(args) -> int:
    if not TRAY_AVAILABLE:
        print("Tray mode requires macOS and the 'rumps' package.")
        return 1

    class BridgeThread(threading.Thread):
        def __init__(self, args):
            super().__init__(daemon=True)
            self.args = args
            self.stop_flag = False

        def run(self):
            # run a reduced copy of main() loop; reuse functions above
            preferred = self.args.port
            ser = None
            last_weather = None
            last_weather_ts = 0.0
            last_net = psutil.net_io_counters()
            last_net_ts = time.time()
            while not self.stop_flag:
                # ensure connection
                while ser is None and not self.stop_flag:
                    port = autodetect_port(preferred)
                    if port is None:
                        time.sleep(1.0)
                        continue
                    try:
                        ser = serial.Serial(port, self.args.baud, timeout=1)
                        time.sleep(2.0)
                        preferred = port
                    except Exception:
                        ser = None
                        time.sleep(1.0)

                if self.stop_flag:
                    break

                # Build payload
                cpu, total_kb, used_kb = get_system_stats()
                disk_free_kb = get_disk_free_kb()
                payload = {"cpu": round(cpu, 1), "ram": total_kb, "ram_used": used_kb}
                try:
                    payload["host"] = socket.gethostname()
                except Exception:
                    pass
                payload["time"] = int(time.time())
                try:
                    payload["uptime"] = int(time.time() - psutil.boot_time())
                except Exception:
                    pass
                if disk_free_kb >= 0:
                    payload["disk_free"] = disk_free_kb
                try:
                    now_ts = time.time()
                    cur = psutil.net_io_counters()
                    dt = max(0.1, now_ts - last_net_ts)
                    rx_rate = (cur.bytes_recv - last_net.bytes_recv) / 1024.0 / dt
                    tx_rate = (cur.bytes_sent - last_net.bytes_sent) / 1024.0 / dt
                    payload["net"] = {"rx": round(rx_rate, 1), "tx": round(tx_rate, 1)}
                    last_net, last_net_ts = cur, now_ts
                except Exception:
                    pass
                try:
                    app = get_active_app_name_macos()
                    if app:
                        payload["app"] = app
                except Exception:
                    pass
                now_ts = time.time()
                if args.lat is not None and args.lon is not None and (now_ts - last_weather_ts > 300 or last_weather is None):
                    last_weather = get_weather(args.lat, args.lon)
                    last_weather_ts = now_ts
                if last_weather is not None:
                    w = {}
                    if last_weather.temp is not None:
                        w["temp"] = round(float(last_weather.temp), 1)
                    if last_weather.desc:
                        w["desc"] = last_weather.desc
                    if last_weather.code is not None:
                        try:
                            w["wcode"] = int(last_weather.code)
                        except Exception:
                            pass
                    if w:
                        payload["weather"] = w

                line = json.dumps(payload, separators=(",", ":")) + "\n"
                try:
                    ser.write(line.encode("utf-8"))
                    ser.flush()
                except Exception:
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
                    time.sleep(1.0)

                time.sleep(max(0.1, args.interval))

            try:
                if ser:
                    ser.close()
            except Exception:
                pass

    class TrayApp(rumps.App):
        def __init__(self, args):
            super().__init__("S")
            self.args = args
            self.bridge = BridgeThread(args)
            self.verbose_item = rumps.MenuItem(title="Verbose", callback=self.toggle_verbose)
            # Initialize state based on current verbosity
            try:
                self.verbose_item.state = bool(self.args.verbose)
            except Exception:
                pass
            self.menu = [
                self.verbose_item,
                None,
                rumps.MenuItem(title="Quit", callback=self.quit_app),
            ]

        def toggle_verbose(self, sender):
            self.args.verbose = not self.args.verbose
            try:
                sender.state = bool(self.args.verbose)
            except Exception:
                pass

        def quit_app(self, _):
            self.bridge.stop_flag = True
            rumps.quit_application()

        def run(self):
            self.bridge.start()
            super().run()

    try:
        print("[host_bridge] Tray starting...", flush=True)
        TrayApp(args).run()
        print("[host_bridge] Tray exited normally", flush=True)
        return 0
    except Exception as e:
        # Ensure we capture any startup error in logs when run under launchd
        import traceback
        traceback.print_exc()
        try:
            sys.stderr.write(f"[host_bridge] Tray failed: {e}\n")
            sys.stderr.flush()
        except Exception:
            pass
        return 78  # EX_CONFIG to surface misconfiguration

if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
from __future__ import annotations

"""
Smart Monitor Tray App (macOS)
- Menubar icon that runs the host bridge in a background thread
- Requires: rumps, psutil, pyserial, requests, pyobjc (installed via tools/requirements.txt)
"""

import threading
import time
import socket
import json
import psutil
import requests
import serial
import platform

import rumps  # type: ignore


# Import bridge helpers from host_bridge.py
from host_bridge import (
	autodetect_port,
	get_system_stats,
	get_disk_free_kb,
	get_active_app_name_macos,
	get_weather,
	Weather,
)


class BridgeThread(threading.Thread):
	def __init__(self, interval: float = 2.0, baud: int = 115200, lat: float | None = None, lon: float | None = None, verbose: bool = False):
		super().__init__(daemon=True)
		self.interval = interval
		self.baud = baud
		self.lat = lat
		self.lon = lon
		self.verbose = verbose
		self.stop_flag = False
		self.preferred_port: str | None = None
		self.ser: serial.Serial | None = None
		self.last_weather: Weather | None = None
		self.last_weather_ts: float = 0.0
		self.last_net = psutil.net_io_counters()
		self.last_net_ts = time.time()

	def log(self, msg: str):
		if self.verbose:
			print(f"[tray_bridge] {msg}")

	def ensure_serial(self):
		while not self.stop_flag and self.ser is None:
			port = autodetect_port(self.preferred_port)
			if port is None:
				self.log("waiting for device…")
				time.sleep(1.0)
				continue
			try:
				self.ser = serial.Serial(port, self.baud, timeout=1)
				time.sleep(2.0)
				self.preferred_port = port
				self.log(f"connected {port} @{self.baud}")
			except Exception as e:
				self.log(f"open {port} failed: {e}")
				self.ser = None
				time.sleep(1.0)

	def run(self):
		while not self.stop_flag:
			if self.ser is None:
				self.ensure_serial()
				if self.ser is None:
					continue

			# Build payload
			cpu, total_kb, used_kb = get_system_stats()
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

			disk_free_kb = get_disk_free_kb()
			if disk_free_kb >= 0:
				payload["disk_free"] = disk_free_kb

			# Net rate
			try:
				now_ts = time.time()
				cur = psutil.net_io_counters()
				dt = max(0.1, now_ts - self.last_net_ts)
				rx_rate = (cur.bytes_recv - self.last_net.bytes_recv) / 1024.0 / dt
				tx_rate = (cur.bytes_sent - self.last_net.bytes_sent) / 1024.0 / dt
				payload["net"] = {"rx": round(rx_rate, 1), "tx": round(tx_rate, 1)}
				self.last_net, self.last_net_ts = cur, now_ts
			except Exception:
				pass

			# Active app (macOS)
			if platform.system() == "Darwin":
				try:
					app = get_active_app_name_macos()
					if app:
						payload["app"] = app
				except Exception:
					pass

			# Weather
			now_ts = time.time()
			if self.lat is not None and self.lon is not None and (self.last_weather is None or now_ts - self.last_weather_ts > 300):
				self.last_weather = get_weather(self.lat, self.lon)
				self.last_weather_ts = now_ts
			if self.last_weather is not None:
				w = {}
				if self.last_weather.temp is not None:
					w["temp"] = round(float(self.last_weather.temp), 1)
				if self.last_weather.desc:
					w["desc"] = self.last_weather.desc
				if self.last_weather.code is not None:
					try:
						w["wcode"] = int(self.last_weather.code)
					except Exception:
						pass
				if w:
					payload["weather"] = w

			line = json.dumps(payload, separators=(",", ":")) + "\n"
			try:
				assert self.ser is not None
				self.ser.write(line.encode("utf-8"))
				self.ser.flush()
			except Exception:
				try:
					if self.ser:
						self.ser.close()
				except Exception:
					pass
				self.ser = None

			time.sleep(max(0.1, self.interval))

		try:
			if self.ser:
				self.ser.close()
		except Exception:
			pass


class TrayApp(rumps.App):
	def __init__(self):
		super().__init__("S")
		self.interval = 2.0
		self.lat = None
		self.lon = None
		self.verbose = False
		self.bridge = BridgeThread(interval=self.interval, lat=self.lat, lon=self.lon, verbose=self.verbose)

		self.item_verbose = rumps.MenuItem("Verbose", callback=self.toggle_verbose)
		self.menu = [self.item_verbose, None, rumps.MenuItem("Quit", callback=self.quit_app)]

	def toggle_verbose(self, sender):
		self.verbose = not self.verbose
		try:
			sender.state = bool(self.verbose)
		except Exception:
			pass
		self.bridge.verbose = self.verbose

	def quit_app(self, _):
		self.bridge.stop_flag = True
		rumps.quit_application()

	def run(self):
		if not self.bridge.is_alive():
			self.bridge.start()
		super().run()


if __name__ == "__main__":
	TrayApp().run()

#!/usr/bin/env python3
# Quick utility to list serial ports on the system.
from __future__ import annotations
import sys
from serial.tools import list_ports

for p in list_ports.comports():
    print(f"{p.device}\t{p.description}\t({p.hwid})")

if not list_ports.comports():
    print("No serial ports found", file=sys.stderr)

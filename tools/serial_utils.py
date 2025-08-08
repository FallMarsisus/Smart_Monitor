from __future__ import annotations
from typing import Optional, List, Tuple

from serial.tools import list_ports


def _score_port(p) -> int:
    """Heuristic scoring for likely ESP32-C3 / USB serial ports.

    Prioritize USB modem/serial and heavily penalize Bluetooth-like ports.
    """
    dev = (p.device or "").lower()
    man = (p.manufacturer or "").lower()
    prod = (getattr(p, "product", "") or "").lower()
    desc = (p.description or "").lower()

    score = 0

    # Hard demotion for Bluetooth-like ports
    if any(k in dev or k in desc or k in man or k in prod for k in ("bluetooth", "rfcomm")):
        score -= 100

    # Strong preference for USB modem/serial patterns
    if "usbmodem" in dev:
        score += 100
    if "usbserial" in dev:
        score += 90
    if "ttyacm" in dev:
        score += 80
    if "ttyusb" in dev:
        score += 70
    if dev.startswith("/dev/cu."):
        # On macOS prefer callout devices
        score += 5

    # Vendor/product hints
    for hint, val in (
        ("espressif", 20), ("cp210", 10), ("wch", 8), ("silicon labs", 8), ("ch340", 8),
        ("cdc", 5), ("usb jtag", 6)
    ):
        if hint in man or hint in prod or hint in desc:
            score += val

    # Small bonus if VID/PID is present (likely a USB device)
    try:
        if getattr(p, "vid", None) is not None and getattr(p, "pid", None) is not None:
            score += 3
    except Exception:
        pass

    return score


def autodetect_port(preferred: Optional[str] = None) -> Optional[str]:
    """Return best candidate serial port or None if nothing connected.

    If `preferred` is provided and currently present, it is returned immediately.
    """
    ports = list(list_ports.comports())
    if not ports:
        return None

    # If preferred still present, use it
    if preferred:
        for p in ports:
            if p.device == preferred:
                return preferred

    scored: List[Tuple[int, str]] = [(_score_port(p), p.device) for p in ports]
    scored.sort(key=lambda t: t[0], reverse=True)
    best = scored[0]
    if best[0] <= 0:
        # No strong hints; pick first
        return ports[0].device
    return best[1]

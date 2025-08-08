# Smart Monitor

Petit moniteur externe (ESP32-C3 + OLED 128x64) affichant CPU, RAM et météo.

## Côté MCU (ESP32-C3)
- Framework: Arduino (PlatformIO)
- Écran: SH1106 (Adafruit_SH110X + Adafruit_GFX)
- Protocole série: JSON par ligne (UTF-8)

Exemple de message:
```
{"cpu":23.4,"ram":16329872,"ram_used":8234567,"weather":{"temp":21.3,"desc":"Nuageux"}}
```

## Côté hôte (macOS/Linux/Windows)
Script Python `tools/host_bridge.py` qui récupère stats système + météo et envoie en série.

### Installation
```
python3 -m venv .venv
source .venv/bin/activate
pip install -r tools/requirements.txt
```

### Utilisation
Identifiez le port série (macOS: `ls /dev/tty.usb*`). Puis:
```
python tools/host_bridge.py --port /dev/tty.usbmodemXXXX --lat 48.8566 --lon 2.3522
```
Options:
- `--baud` (défaut 115200)
- `--interval` secondes entre mises à jour (défaut 2)

Si vous ne fournissez pas `--lat/--lon`, la météo est ignorée.

### Dépannage
- Si rien ne s'affiche, ouvrez le moniteur série de PlatformIO à 115200 pour voir les logs.
- L'écran SH1106 utilise l'adresse I2C 0x3C.
- Pour un autre contrôleur (SSD1306), adaptez la lib et l'init dans `src/main.cpp`.

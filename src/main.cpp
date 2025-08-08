// -----------------------------------------------------------------------------
// Dépendances
// -----------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ArduinoJson.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_system.h>
#endif

// -----------------------------------------------------------------------------
// Constantes matériel / écran
// -----------------------------------------------------------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define I2C_ADDRESS 0x3C  // Adresse 7-bit (0x78 >> 1)

// Ecran SH1106 1.3"
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// -----------------------------------------------------------------------------
// Helpers d'affichage
// -----------------------------------------------------------------------------
static int textWidth(const String &s, uint8_t size = 1) {
  return s.length() * 6 * size; // 5px glyph + 1px espacement
}

static void printRightAligned(int xRight, int y, const String &s) {
  int w = textWidth(s, 1);
  display.setCursor(xRight - w, y);
  display.print(s);
}

// Formatage rapide de valeurs
static String fmtPercent(int v) { return String(v) + "%"; }
static String fmtTempC(int t) { return String(t) + "C"; }
static String fmtDisk(long kb) {
  if (kb < 0) return String("--");
  long mb = kb / 1024;
  if (mb > 9999) return String((int)(mb / 1024)) + "GB";
  return String((int)mb) + "MB";
}
static String fmtUptime(long seconds) {
  if (seconds < 0) return String("--");
  long m = seconds / 60; long h = m / 60; long d = h / 24; h %= 24; m %= 60;
  String s; if (d > 0) { s += d; s += "d "; } s += h; s += "h"; s += m; s += "m"; return s;
}

// Tronquer une chaîne pour tenir dans une largeur en pixels
static String clipToWidth(const String &s, int pxWidth) {
  int maxChars = pxWidth / 6; if (maxChars < 0) maxChars = 0;
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars);
}

// -----------------------------------------------------------------------------
// Style rétro Windows: panneaux en relief et barres compactes
// -----------------------------------------------------------------------------
static void drawSunkenPanel(int x, int y, int w, int h) {
  // Double bordure pour simuler un relief enfoncé (style Win95 monocrome)
  display.drawRect(x, y, w, h, SH110X_WHITE);
  if (w > 2 && h > 2) display.drawRect(x + 1, y + 1, w - 2, h - 2, SH110X_WHITE);
}

static void drawProgressBar95(int x, int y, int w, int h, float ratio) {
  if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
  drawSunkenPanel(x, y, w, h);
  int iw = w - 4; int ih = h - 4; if (iw < 1 || ih < 1) return;
  int fw = (int)(iw * ratio + 0.5f);
  if (fw > 0) display.fillRect(x + 2, y + 2, fw, ih, SH110X_WHITE);
}

// -----------------------------------------------------------------------------
// Etat des données et UI (séparés pour lisibilité)
// -----------------------------------------------------------------------------
struct DataState {
  float cpu = -1;           // 0..100
  long ram = -1;            // KB
  long ram_used = -1;       // KB
  float tempC = NAN;        // °C
  String weatherDesc = "";
  String host = "";
  long epoch = 0;           // s
  long uptime = -1;         // s
  long diskFreeKB = -1;     // KB
  float net_rx = NAN;       // KB/s
  float net_tx = NAN;       // KB/s
};

struct UIState {
  bool hasData = false;
  // cibles
  float tgtCpu = 0, tgtRamRatio = 0, tgtNetRatio = 0;
  // courants (animés)
  float curCpu = 0, curRamRatio = 0, curNetRatio = 0;
  float netMaxKBs = 1; // auto-échelle pour net

  // ticker bas
  String tickerText = "";
  int tickerX = SCREEN_WIDTH;
  int tickerW = 1;
  int gaugesBottomY = 0; // position Y après les jauges

  // Animation Tamagochi
  bool tamaBlink = false;
  unsigned long tamaBlinkUntil = 0;
  unsigned long tamaNextBlink = 0;
  uint8_t tamaMouthPhase = 0; // 0..3
  unsigned long tamaMouthMs = 0;
  // Animations mignonnes
  bool tamaWink = false;            // clin d'œil
  unsigned long tamaWinkUntil = 0;
  bool tamaSweat = false;           // goutte de sueur
  unsigned long tamaSweatUntil = 0;
  int8_t headBob = 0;               // petit mouvement vertical
  // Sommeil
  bool tamaSleeping = false;
  unsigned long lowLoadSince = 0;
  uint8_t sleepStep = 0;
  unsigned long sleepMs = 0;
};

static DataState data;
static UIState ui;
static String appName = ""; // Ajout de la variable globale appName

// -----------------------------------------------------------------------------
// Lecture JSON (une ligne) -> met à jour Data + UI
// -----------------------------------------------------------------------------
static bool updateFromJsonLine(const String &line) {
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("Erreur JSON: "); Serial.println(err.f_str());
    return false;
  }

  // Récupérer valeurs (avec défauts sûrs)
  data.cpu = doc["cpu"] | data.cpu;
  data.ram = doc["ram"] | data.ram;
  data.ram_used = doc["ram_used"] | data.ram_used;
  data.tempC = doc["weather"]["temp"] | data.tempC;
  data.host = String((const char*)(doc["host"] | data.host.c_str()));
  data.epoch = doc["time"] | data.epoch;
  data.uptime = doc["uptime"] | data.uptime;
  data.diskFreeKB = doc["disk_free"] | data.diskFreeKB;
  data.net_rx = doc["net"]["rx"] | data.net_rx;
  data.net_tx = doc["net"]["tx"] | data.net_tx;
  if (doc.containsKey("app")) {
    String newApp = doc["app"].as<String>(); // copie sûre de la chaîne
    newApp.trim();
    appName = newApp; // utilisé dans le header uniquement
  }

  // Mettre à jour cibles et auto-échelle réseau
  if (data.cpu >= 0) ui.tgtCpu = data.cpu;
  if (data.ram > 0 && data.ram_used >= 0) ui.tgtRamRatio = (float)data.ram_used / (float)data.ram;
  if (!isnan(data.net_rx) && !isnan(data.net_tx)) {
    float total = max(0.0f, data.net_rx + data.net_tx);
    if (total > ui.netMaxKBs) ui.netMaxKBs = total; // up rapide
    ui.tgtNetRatio = ui.netMaxKBs > 0 ? total / ui.netMaxKBs : 0;
    ui.netMaxKBs *= 0.996f; if (ui.netMaxKBs < 1) ui.netMaxKBs = 1; // decay lent
  }

  // Ticker
  String t;
  if (!isnan(data.tempC)) { t += " "; t += (int)data.tempC; t += "C"; }
  if (data.cpu >= 0) { t += "  CPU "; t += (int)data.cpu; t += "%"; }
  if (data.ram > 0 && data.ram_used >= 0) { long freeMB = (data.ram - data.ram_used)/1024; t += "  RAM "; t += (int)freeMB; t += "MB"; }
  if (data.diskFreeKB >= 0) { t += "  DISK "; t += fmtDisk(data.diskFreeKB); }
  if (data.uptime >= 0) { t += "  UPT "; t += fmtUptime(data.uptime); }
  if (t.length() == 0) t = " Smart Monitor";
  ui.tickerText = t + "   ";
  ui.tickerW = textWidth(ui.tickerText, 1); if (ui.tickerW < 1) ui.tickerW = 1;
  if (ui.tickerX > SCREEN_WIDTH) ui.tickerX = SCREEN_WIDTH;

  ui.hasData = true;
  return true;
}

// -----------------------------------------------------------------------------
// Rendu: Header / Jauges / Infos / Ticker
// -----------------------------------------------------------------------------
static void drawHeader() {
  const int H = 10; // plus compact
  display.fillRect(0, 0, SCREEN_WIDTH, H, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK, SH110X_WHITE);
  display.setTextSize(1);

  // Temp à gauche
  display.setCursor(2, 2);
  String tempStr = isnan(data.tempC) ? String("--C") : fmtTempC((int)data.tempC);
  display.print(tempStr);

  // Titre = nom de l'app (ou fallback)
  String title = appName.length() ? appName : String("SMON");
  // Espace dispo à droite de la température
  int tempW = textWidth(tempStr, 1);
  int xAvail = 2 + tempW + 4; // petite marge
  int availW = SCREEN_WIDTH - xAvail - 2; if (availW < 0) availW = 0;
  String clipped = clipToWidth(title, availW);
  int tw = textWidth(clipped, 1);
  int tx = xAvail + (availW - tw) / 2; if (tx < xAvail) tx = xAvail;
  display.setCursor(tx, 2);
  display.print(clipped);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK); // reset
}

static void drawGauges() {
  // Deux colonnes: gauche = jauges demi-largeur, droite = infos texte
  const int headerH = 10;
  const int leftX = 2;
  const int gap = 2;
  // Barres plus étroites (2/3 de la demi-largeur) mais plus hautes
  const int halfW = (SCREEN_WIDTH - 4 - gap) / 2;
  const int colW = (halfW * 2) / 3;
  const int rightX = leftX + colW + gap;

  // --- Colonne gauche: jauges ---
  int y = headerH + 2;
  const int labelX = max(0, leftX - 2); // libellés un peu plus à gauche
  // CPU
  display.setCursor(labelX, y); display.print("CPU:");
  y += 8; // plus d'espace sous le libellé pour lisibilité
  display.drawRect(leftX, y, colW, 7, SH110X_WHITE);
  {
    int iw = colW - 2; int fw = (int)(iw * (ui.curCpu / 100.0f) + 0.5f); if (fw < 0) fw = 0; if (fw > iw) fw = iw;
    if (fw > 0) display.fillRect(leftX + 1, y + 1, fw, 5, SH110X_WHITE);
  }
  y += 7 + 3;
  // RAM
  display.setCursor(labelX, y); display.print("RAM:");
  y += 8; // même espacement accru
  display.drawRect(leftX, y, colW, 7, SH110X_WHITE);
  {
    int iw = colW - 2; int fw = (int)(iw * ui.curRamRatio + 0.5f); if (fw < 0) fw = 0; if (fw > iw) fw = iw;
    if (fw > 0) display.fillRect(leftX + 1, y + 1, fw, 5, SH110X_WHITE);
  }
  y += 7;
  ui.gaugesBottomY = y;
}

static void drawInfoLines() {
  // Colonne droite: Tamagochi animé selon l'état CPU+RAM
  const int headerH = 10;
  const int leftX = 2;
  const int gap = 2; // align with gauges
  const int halfW = (SCREEN_WIDTH - 4 - gap) / 2;
  const int leftColW = (halfW * 2) / 3;              // même largeur que les jauges
  const int startRight = leftX + leftColW + gap;     // bord gauche de la colonne droite
  const int colW = SCREEN_WIDTH - startRight - 2;    // largeur utile
  int y = headerH + 2;

  // Zone centrée dans la colonne droite (pas de cercle de contour)
  // Dimensions et centrage strict dans la colonne droite (entre header et ticker)
  const int tickH = 9;
  const int topY = headerH + 2;
  const int bottomY = SCREEN_HEIGHT - tickH; // juste au-dessus du ticker
  int areaH = bottomY - topY;
  int areaW = colW;
  int w = min(areaW, areaH);
  if (w < 22) w = 22; if (w > 50) w = 50;
  int cx = startRight + areaW / 2;
  int cy = topY + areaH / 2 + ui.headBob;

  // Humeur selon moyenne CPU/RAM
  float load = 0.0f;
  if (data.cpu >= 0) load += (data.cpu/100.0f);
  if (data.ram > 0 && data.ram_used >= 0) load += ((float)data.ram_used/(float)data.ram);
  load *= 0.5f;

  // Visage (yeux, sourcils, bouche)
  int r = w/2 - 1;
  int eyeY = cy - r/4;
  int eyeDX = r/2; // écartement d'origine
  int eyeW = max(2, r/5);
  int eyeHLeft = (ui.tamaBlink || ui.tamaWink) ? 1 : max(2, r/5);
  int eyeHRight = ui.tamaBlink ? 1 : max(2, r/5);
  // Yeux (mode sommeil: lignes fermées)
  if (ui.tamaSleeping) {
    display.drawFastHLine(cx - eyeDX - eyeW/2, eyeY, eyeW, SH110X_WHITE);
    display.drawFastHLine(cx + eyeDX - eyeW/2, eyeY, eyeW, SH110X_WHITE);
  } else {
    display.fillRect(cx - eyeDX - eyeW/2, eyeY - eyeHLeft/2, eyeW, eyeHLeft, SH110X_WHITE);
    display.fillRect(cx + eyeDX - eyeW/2, eyeY - eyeHRight/2, eyeW, eyeHRight, SH110X_WHITE);
  }
  // Sourcils (indicatifs humeur)
  int mouthY = cy + r/4;
  int mouthW = max(6, (int)(r * 0.7f)); // bouche un peu moins large
  if (ui.tamaSleeping) {
    // bouche neutre
    display.drawFastHLine(cx - mouthW/2, mouthY, mouthW, SH110X_WHITE);
  } else if (load < 0.42f) {
  // heureux: petits arcs au-dessus des yeux (mignons)
  int lx0 = cx - eyeDX - eyeW;
  int lx1 = cx - eyeDX + eyeW;
  int lxc = (lx0 + lx1) / 2;
  int ly  = eyeY - eyeHLeft - 4; // un peu plus haut
  display.drawLine(lx0, ly, lxc, ly - 2, SH110X_WHITE);
  display.drawLine(lxc, ly - 2, lx1, ly, SH110X_WHITE);

  int rx0 = cx + eyeDX - eyeW;
  int rx1 = cx + eyeDX + eyeW;
  int rxc = (rx0 + rx1) / 2;
  int ry  = eyeY - eyeHRight - 4;
  display.drawLine(rx0, ry, rxc, ry - 2, SH110X_WHITE);
  display.drawLine(rxc, ry - 2, rx1, ry, SH110X_WHITE);
  } else if (load > 0.68f) {
    display.drawLine(cx - eyeDX - eyeW, eyeY - eyeHLeft - 1, cx - eyeDX + eyeW, eyeY - eyeHLeft - 0, SH110X_WHITE);
    display.drawLine(cx + eyeDX - eyeW, eyeY - eyeHRight - 0, cx + eyeDX + eyeW, eyeY - eyeHRight - 1, SH110X_WHITE);
  }

  // Zz bulle de sommeil
  if (ui.tamaSleeping) {
    // animation lente des 'Z'
    if (millis() - ui.sleepMs > 600) { ui.sleepMs = millis(); ui.sleepStep = (ui.sleepStep + 1) % 3; }
    int zx = cx + r - 4;
    int zy = cy - r + 4 + (ui.sleepStep == 1 ? -1 : ui.sleepStep == 2 ? -2 : 0);
    // deux petits Z superposés
    display.drawLine(zx, zy, zx + 3, zy, SH110X_WHITE);
    display.drawLine(zx + 1, zy - 1, zx + 1, zy + 2, SH110X_WHITE);
    display.drawLine(zx, zy + 2, zx + 3, zy + 2, SH110X_WHITE);
    display.drawLine(zx + 5, zy - 3, zx + 8, zy - 3, SH110X_WHITE);
    display.drawLine(zx + 6, zy - 4, zx + 6, zy - 1, SH110X_WHITE);
    display.drawLine(zx + 5, zy - 1, zx + 8, zy - 1, SH110X_WHITE);
  }

  if (!ui.tamaSleeping) {
    if (load < 0.42f) {
      // sourire
      display.drawLine(cx - mouthW/2, mouthY + 2, cx, mouthY + 4, SH110X_WHITE);
      display.drawLine(cx, mouthY + 4, cx + mouthW/2, mouthY + 2, SH110X_WHITE);
    } else if (load < 0.68f) {
      // neutre
      display.drawFastHLine(cx - mouthW/2, mouthY, mouthW, SH110X_WHITE);
    } else {
      // triste
      display.drawLine(cx - mouthW/2, mouthY + 2, cx, mouthY, SH110X_WHITE);
      display.drawLine(cx, mouthY, cx + mouthW/2, mouthY + 2, SH110X_WHITE);
    }
  }

  // Goutte de sueur (quand charge haute ou événement aléatoire)
  if (ui.tamaSweat) {
    int sx = cx + eyeDX + 2;
    int sy = eyeY - 2;
    display.drawLine(sx, sy, sx + 1, sy + 2, SH110X_WHITE);
    display.drawLine(sx + 1, sy + 2, sx, sy + 4, SH110X_WHITE);
  }
}

static void drawTicker() {
  const int tickH = 9;
  int tickY = SCREEN_HEIGHT - tickH + 2;
  display.drawFastHLine(0, tickY - 2, SCREEN_WIDTH, SH110X_WHITE);
  display.setCursor(ui.tickerX, tickY);
  display.print(ui.tickerText);
}

static void paintWaiting() {
  static unsigned long lastPaint = 0;
  if (millis() - lastPaint < 1000) return;
  lastPaint = millis();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print("Smart Monitor");
  display.setCursor(0, 16); display.print("En attente donnees...");
  display.setCursor(0, 28); display.print("Verifiez script host");
  display.setCursor(0, 40); display.print("115200 baud");
  display.display();
}

// -----------------------------------------------------------------------------
// Setup & Loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(1024);
#endif
  Wire.begin();

  delay(200);
  display.begin(I2C_ADDRESS, true);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print("Smart Monitor");
  display.display();

  // Seed aléatoire pour les animations
#if defined(ARDUINO_ARCH_ESP32)
  randomSeed(esp_random());
#else
  randomSeed(analogRead(0));
#endif
}

void loop() {
  // 1) Lecture série ligne par ligne (CR ou LF)
  static String line; static unsigned long lastDataMs = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length() > 0) {
        if (updateFromJsonLine(line)) lastDataMs = millis();
      }
      line = "";
    } else {
      // Autoriser des lignes JSON un peu plus longues
      if (line.length() < 1536) line += c;
    }
  }

  // 2) Connexion/attente: si jamais aucune donnée reçue, écran d'attente.
  //    Sinon, en cas de perte de données, on montre le tamagochi endormi au lieu d'un écran plein.
  if (lastDataMs == 0) {
    paintWaiting();
    return;
  }
  if ((millis() - lastDataMs) > 4000) {
    ui.tamaSleeping = true; // dort si plus de données récentes, mais on continue à rendre l'UI
  }

  // 3) Animation douce (moins d'agitation)
  static unsigned long lastAnim = 0;
  if (millis() - lastAnim < 60) return; // ~16 FPS max
  lastAnim = millis();

  ui.curCpu += (ui.tgtCpu - ui.curCpu) * 0.15f;
  if (ui.curCpu < 0) ui.curCpu = 0; if (ui.curCpu > 100) ui.curCpu = 100;
  ui.curRamRatio += (ui.tgtRamRatio - ui.curRamRatio) * 0.15f;
  if (ui.curRamRatio < 0) ui.curRamRatio = 0; if (ui.curRamRatio > 1) ui.curRamRatio = 1;
  ui.curNetRatio += (ui.tgtNetRatio - ui.curNetRatio) * 0.15f;
  if (ui.curNetRatio < 0) ui.curNetRatio = 0; if (ui.curNetRatio > 1) ui.curNetRatio = 1;

  // Ticker avance lentement
  ui.tickerX -= 1; if (ui.tickerX + ui.tickerW < 0) ui.tickerX = SCREEN_WIDTH;

  // Animation Tamagochi: clignement et phase bouche
  unsigned long now = millis();
  if (now > ui.tamaNextBlink) {
    ui.tamaBlink = true;
    ui.tamaBlinkUntil = now + 120; // cligne ~120ms
    ui.tamaNextBlink = now + 2000 + (now % 3000);
  }
  if (ui.tamaBlink && now > ui.tamaBlinkUntil) ui.tamaBlink = false;
  if (now - ui.tamaMouthMs > 300) { ui.tamaMouthMs = now; ui.tamaMouthPhase = (ui.tamaMouthPhase + 1) & 3; }

  // Clin d'œil occasionnel
  static unsigned long nextWink = 0;
  if (now > nextWink) {
    if ((random(100) < 10) && !ui.tamaBlink) { // 10% chance
      ui.tamaWink = true; ui.tamaWinkUntil = now + 120;
    }
    nextWink = now + 1500 + random(2000);
  }
  if (ui.tamaWink && now > ui.tamaWinkUntil) ui.tamaWink = false;

  // (langue désactivée)

  // Sueur quand charge haute ou au hasard
  static unsigned long nextSweat = 0;
  float loadNow = 0.0f; loadNow += ui.curCpu/100.0f; loadNow += ui.curRamRatio; loadNow *= 0.5f;
  if (now > nextSweat) {
    int prob = (int)(max(0.0f, (loadNow - 0.7f)) * 100); // augmente >70%
    prob += 5; // légère chance même basse charge
    if (random(100) < prob) { ui.tamaSweat = true; ui.tamaSweatUntil = now + 500; }
    nextSweat = now + 2000 + random(2000);
  }
  if (ui.tamaSweat && now > ui.tamaSweatUntil) ui.tamaSweat = false;

  // Head bob léger
  ui.headBob = (int8_t)(sin(now / 400.0) * 1.5);

  // Sommeil: si charge faible prolongée, entrer en sommeil
  float curLoad = 0.0f; curLoad += ui.curCpu/100.0f; curLoad += ui.curRamRatio; curLoad *= 0.5f;
  if (curLoad < 0.22f) { // seuil un peu plus permissif
    if (ui.lowLoadSince == 0) ui.lowLoadSince = now;
    if (!ui.tamaSleeping && now - ui.lowLoadSince > 9000) ui.tamaSleeping = true; // 9s faible
  } else {
    ui.lowLoadSince = 0;
    // Ne pas réveiller si la connexion est perdue (on garde le dodo)
    if ((millis() - lastDataMs) <= 4000) ui.tamaSleeping = false;
    ui.sleepStep = 0;
  }

  // 4) Rendu
  display.clearDisplay();
  drawHeader();
  drawGauges();
  drawInfoLines();
  drawTicker();
  display.display();
}
// -----------------------------------------------------------------------------
// Setup & Loop
// -----------------------------------------------------------------------------
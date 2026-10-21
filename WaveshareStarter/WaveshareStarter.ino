#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include "TouchDrvCST.hpp"
#include "esp_random.h"
#include "pin_config.h"
#include "XPowersLib.h"

// Gridley launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// The existing Wi-Fi and battery settings remain in the "starter" namespace.

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

XPowersPMU power;
TouchDrvCST92xx touch;
Preferences settings;
WebServer web(80);
DNSServer dns;

enum NetworkMode { CONNECTING, SETUP, CONNECTED };
enum AppScreen { LAUNCHER, NIGHT_SPINNER, STATUS_SETTINGS };
NetworkMode networkMode = CONNECTING;
AppScreen screen = LAUNCHER;

struct Bar {
  String name;
  String address;
};
constexpr uint8_t MAX_BARS = 16;
Bar barOptions[MAX_BARS];
uint8_t barCount = 0;
String areaLabel = "60612";

const char *DEFAULT_BARS =
    "Cobra Lounge|235 N Ashland Ave\n"
    "The Press Room|1134 W Washington Blvd\n"
    "Inner Town Pub|1935 W Thomas St\n"
    "Empty Bottle|1035 N Western Ave\n"
    "Sportsman's Club|948 N Western Ave\n";

constexpr uint16_t BG = 0x0000;
constexpr uint16_t CYAN = 0x07FF;
constexpr uint16_t BLUE = 0x03BF;
constexpr uint16_t PURPLE = 0xA81F;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t MUTED = 0x9CD3;
constexpr uint16_t PANEL = 0x10A2;
constexpr uint16_t GRID_BRIGHT = 0x04BF;
constexpr uint16_t GRID_MID = 0x0294;
constexpr uint16_t GRID_DIM = 0x014A;

bool displayReady = false;
bool powerReady = false;
bool touchReady = false;
bool screenDirty = true;
volatile bool touchPending = false;
String apName;
String apPassword;
String savedSsid;
String savedPassword;
unsigned long connectStarted = 0;
unsigned long restartAt = 0;
unsigned long bootPressedAt = 0;
unsigned long lastDraw = 0;
unsigned long lastLog = 0;
unsigned long lastTap = 0;

int selectedBar = -1;
int displayedBar = -1;
int nextBar = -1;
uint8_t spinFrame = 0;
bool spinning = false;
unsigned long lastSpinFrame = 0;

void IRAM_ATTR onTouchInterrupt() { touchPending = true; }

String makeSetupPassword() {
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String result;
  for (int i = 0; i < 12; ++i) {
    result += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  return result;
}

bool loadBars(const String &raw) {
  Bar parsed[MAX_BARS];
  uint8_t count = 0;
  int start = 0;
  while (start < raw.length()) {
    int end = raw.indexOf('\n', start);
    if (end < 0) end = raw.length();
    String line = raw.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.isEmpty()) continue;
    int separator = line.indexOf('|');
    if (separator < 1 || line.indexOf('|', separator + 1) >= 0 || count >= MAX_BARS) {
      return false;
    }
    String name = line.substring(0, separator);
    String address = line.substring(separator + 1);
    name.trim();
    address.trim();
    if (name.isEmpty() || address.isEmpty() || name.length() > 32 ||
        address.length() > 45) return false;
    parsed[count++] = {name, address};
  }
  if (count == 0) return false;
  for (uint8_t i = 0; i < count; ++i) barOptions[i] = parsed[i];
  barCount = count;
  return true;
}

String barListText() {
  String result;
  for (uint8_t i = 0; i < barCount; ++i) {
    result += barOptions[i].name + " | " + barOptions[i].address + "\n";
  }
  return result;
}

String htmlEscape(const String &raw) {
  String escaped;
  escaped.reserve(raw.length() + 20);
  for (size_t i = 0; i < raw.length(); ++i) {
    switch (raw[i]) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += raw[i];
    }
  }
  return escaped;
}

void textCentered(const String &value, int y, uint8_t size = 2,
                  uint16_t color = WHITE) {
  if (!displayReady) return;
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  int width = static_cast<int>(value.length()) * 6 * size;
  gfx->setCursor(max(12, (LCD_WIDTH - width) / 2), y);
  gfx->print(value);
}

void button(int x, int y, int w, int h, const String &label,
            uint16_t color = CYAN) {
  gfx->fillRoundRect(x, y, w, h, 18, PANEL);
  gfx->drawRoundRect(x, y, w, h, 18, color);
  int size = 2;
  int labelWidth = label.length() * 6 * size;
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x + (w - labelWidth) / 2, y + (h - 16) / 2);
  gfx->print(label);
}

void drawPerspectiveGrid() {
  // Two receding planes leave a black corridor behind the menu buttons.
  // Lines are drawn directly, so the grid needs no bitmap or extra flash.
  const int topRows[] = {10, 29, 47, 64, 79, 93, 105, 116, 126, 134, 141, 147};
  const int bottomRows[] = {456, 437, 419, 402, 387, 373, 361, 350, 340, 332, 325, 319};
  for (int i = 0; i < 12; ++i) {
    uint16_t color = i < 3 ? GRID_BRIGHT : i < 7 ? GRID_MID : GRID_DIM;
    gfx->drawFastHLine(0, topRows[i], LCD_WIDTH, color);
    gfx->drawFastHLine(0, bottomRows[i], LCD_WIDTH, color);
  }
  for (int x = 0; x < LCD_WIDTH; x += 51) {
    int vanishingX = 233 + (x - 233) / 20;
    int topMidX = x + (vanishingX - x) * 110 / 155;
    int bottomMidX = x + (vanishingX - x) * 100 / 154;
    gfx->drawLine(x, 0, topMidX, 110, GRID_MID);
    gfx->drawLine(topMidX, 110, vanishingX, 155, GRID_DIM);
    gfx->drawLine(x, 465, bottomMidX, 365, GRID_MID);
    gfx->drawLine(bottomMidX, 365, vanishingX, 311, GRID_DIM);
  }
}

void chrome(const String &title, bool showBack = true, bool grid = false) {
  gfx->fillScreen(BG);
  if (grid) {
    drawPerspectiveGrid();
    gfx->fillRoundRect(172, 74, 122, 40, 10, BG);
  }
  gfx->drawCircle(233, 233, 224, BLUE);
  gfx->drawCircle(233, 233, 218, PANEL);
  if (showBack) {
    button(91, 91, 93, 45, "< BACK", BLUE);
  }
  textCentered(title, showBack ? 65 : 88, 2, CYAN);
}

String batteryText() {
  if (!powerReady) return "Battery: unknown";
  if (!power.isBatteryConnect()) return "Battery: not detected";
  return "Battery: " + String(power.getBattVoltage() / 1000.0f, 2) + " V";
}

void drawLauncher() {
  chrome("GRIDLEY", false, true);
  gfx->fillRoundRect(91, 115, 284, 39, 12, BG);
  textCentered("PICK A PROGRAM, USER", 124, 2, CYAN);
  button(84, 170, 298, 70, "CHICAGO SPINNER", CYAN);
  button(84, 262, 298, 70, "SETTINGS", PURPLE);
}

void drawSpinner() {
  chrome("NIGHT SPINNER");
  textCentered("CHICAGO / " + areaLabel, 139, 2, MUTED);
  gfx->drawCircle(233, 237, 94, CYAN);
  gfx->drawCircle(233, 237, 88, BLUE);

  if (displayedBar < 0 || barCount == 0) {
    textCentered("WHERE TO?", 222, 3, WHITE);
  } else {
    const String &name = barOptions[displayedBar].name;
    int size = name.length() > 18 ? 2 : 3;
    textCentered(name, 210, size, WHITE);
    if (!spinning) {
      String address = barOptions[displayedBar].address;
      if (address.length() > 28) address = address.substring(0, 28);
      textCentered(address, 257, 2, MUTED);
    }
  }
  button(95, 347, 276, 63, spinning ? "SPINNING" :
         selectedBar < 0 ? "SPIN" : "SPIN AGAIN", CYAN);
  if (!spinning && selectedBar >= 0) {
    textCentered("Check hours before going", 416, 1, MUTED);
  }
}

void drawSettings() {
  chrome("SETTINGS");
  textCentered("GRIDLEY STATUS", 138, 2, CYAN);
  textCentered(batteryText(), 173, 2, WHITE);
  if (powerReady) {
    textCentered(power.isVbusIn() ? "USB power: yes" : "USB power: no", 202, 2, MUTED);
    textCentered(power.isCharging() ? "Charging: yes" : "Charging: no", 231, 2, MUTED);
  }
  if (networkMode == CONNECTED) {
    textCentered("Wi-Fi connected", 271, 2, CYAN);
    textCentered(WiFi.localIP().toString(), 300, 2, WHITE);
    textCentered("Edit bars: open IP on phone", 335, 2, MUTED);
  } else if (networkMode == SETUP) {
    textCentered("Wi-Fi setup network", 271, 2, CYAN);
    textCentered(apName, 300, 2, WHITE);
    textCentered(apPassword, 328, 2, WHITE);
    textCentered("Open 192.168.4.1", 356, 2, MUTED);
  } else {
    textCentered("Connecting to Wi-Fi", 281, 2, CYAN);
  }
  textCentered("Area: " + areaLabel + "  /  " + String(barCount) + " bars", 389, 2, MUTED);
  textCentered("Hold BOOT 3s: reset Wi-Fi", 417, 1, MUTED);
}

void drawScreen() {
  if (!displayReady) return;
  switch (screen) {
    case LAUNCHER: drawLauncher(); break;
    case NIGHT_SPINNER: drawSpinner(); break;
    case STATUS_SETTINGS: drawSettings(); break;
  }
}

void startSpin() {
  if (barCount == 0 || spinning) return;
  nextBar = esp_random() % barCount;
  if (barCount > 1 && nextBar == selectedBar) {
    nextBar = (nextBar + 1 + esp_random() % (barCount - 1)) % barCount;
  }
  spinning = true;
  spinFrame = 0;
  lastSpinFrame = 0;
  screenDirty = true;
}

void updateSpin() {
  if (!spinning) return;
  unsigned long now = millis();
  unsigned long interval = spinFrame < 8 ? 80 : 140 + (spinFrame - 8) * 35;
  if (lastSpinFrame && now - lastSpinFrame < interval) return;
  lastSpinFrame = now;
  ++spinFrame;
  if (spinFrame >= 13) {
    selectedBar = nextBar;
    displayedBar = selectedBar;
    spinning = false;
    Serial.println("Spinner picked: " + barOptions[selectedBar].name);
  } else {
    displayedBar = esp_random() % barCount;
  }
  screenDirty = true;
}

void handleTap(int x, int y) {
  if (screen == LAUNCHER) {
    if (x >= 84 && x <= 382 && y >= 170 && y <= 240) screen = NIGHT_SPINNER;
    else if (x >= 84 && x <= 382 && y >= 262 && y <= 332) screen = STATUS_SETTINGS;
  } else {
    if (x >= 91 && x <= 184 && y >= 91 && y <= 136) {
      screen = LAUNCHER;
    } else if (screen == NIGHT_SPINNER && x >= 95 && x <= 371 &&
               y >= 347 && y <= 410) {
      startSpin();
    }
  }
  screenDirty = true;
}

void pollTouch() {
  if (!touchReady) return;
  noInterrupts();
  bool pending = touchPending;
  touchPending = false;
  interrupts();
  if (!pending) return;
  int16_t x[1] = {}, y[1] = {};
  if (!touch.getPoint(x, y, 1)) return;
  if (millis() - lastTap < 260) return;
  lastTap = millis();
  Serial.printf("Touch: %d,%d\n", x[0], y[0]);
  handleTap(x[0], y[0]);
}

void sendWifiSetupPage() {
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html",
           "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Gridley Wi-Fi setup</title><style>body{font:18px system-ui;max-width:32rem;"
           "margin:3rem auto;padding:0 1rem}label{display:block;margin:1rem 0}"
           "input{display:block;font:inherit;width:100%;padding:.6rem;box-sizing:border-box}"
           "button{font:inherit;padding:.7rem 1.2rem}</style><h1>Gridley Wi-Fi setup</h1>"
           "<p>Enter a 2.4 GHz Wi-Fi network. <a href='/bars'>Edit the bar list</a>.</p>"
           "<form method='post' action='/save'><label>Network name<input name='ssid'"
           " maxlength='32' required></label><label>Wi-Fi password<input name='password'"
           " type='password' maxlength='63'></label><button>Connect</button></form></html>");
}

void saveWifi() {
  String ssid = web.arg("ssid");
  String password = web.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 ||
      (!password.isEmpty() && (password.length() < 8 || password.length() > 63))) {
    web.send(400, "text/plain", "Enter a network name and a password of 8-63 characters, or leave it empty for an open network.");
    return;
  }
  settings.putString("ssid", ssid);
  settings.putString("password", password);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html", "<h1>Saved</h1><p>Gridley is restarting to connect.</p>");
  restartAt = millis() + 2500;
}

void sendBarPage() {
  String page =
      "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Gridley bars</title><style>body{font:18px system-ui;max-width:36rem;"
      "margin:2rem auto;padding:0 1rem;background:#10131c;color:#eee}"
      "input,textarea{display:block;width:100%;box-sizing:border-box;padding:.7rem;"
      "font:inherit;margin:.4rem 0 1rem;background:#202635;color:#fff;border:1px solid #49dce9}"
      "textarea{height:19rem}button{font:inherit;padding:.7rem 1.2rem;"
      "background:#49dce9;border:0;border-radius:.4rem}small{color:#abb4c5}</style>"
      "<h1>Chicago Night Spinner</h1><p>Edit Gridley's local bar list.</p>"
      "<form method='post' action='/bars'><label>Area label<input name='area' maxlength='20' value='";
  page += htmlEscape(areaLabel);
  page += "' required></label><label>Bars, one per line as Name | Street address"
          "<textarea name='bars' maxlength='1800' required>";
  page += htmlEscape(barListText());
  page += "</textarea></label><button>Save to Gridley</button></form>"
          "<p><small>The starter list is around 60612. Changing the area label does not"
          " search for bars; edit the list for a new area. Check hours before going."
          " Up to 16 bars, 32 characters per name.</small></p></html>";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html", page);
}

void saveBars() {
  String newArea = web.arg("area");
  String raw = web.arg("bars");
  newArea.trim();
  if (newArea.isEmpty() || newArea.length() > 20 || raw.length() > 1800 ||
      !loadBars(raw)) {
    web.send(400, "text/plain", "Use 1-16 lines: Name | Street address. Names must be 32 characters or fewer, addresses 45 or fewer.");
    return;
  }
  areaLabel = newArea;
  settings.putString("area", areaLabel);
  settings.putString("bars", barListText());
  selectedBar = displayedBar = -1;
  spinning = false;
  screenDirty = true;
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html", "<h1>Saved to Gridley</h1><p><a href='/bars'>Edit again</a></p>");
}

void configureWeb() {
  web.on("/", HTTP_GET, []() {
    if (networkMode == SETUP) sendWifiSetupPage();
    else sendBarPage();
  });
  web.on("/save", HTTP_POST, saveWifi);
  web.on("/bars", HTTP_GET, sendBarPage);
  web.on("/bars", HTTP_POST, saveBars);
  web.onNotFound([]() {
    if (networkMode == SETUP) {
      web.sendHeader("Location", "http://192.168.4.1/");
      web.send(302, "text/plain", "Open http://192.168.4.1/");
    } else web.send(404, "text/plain", "Not found");
  });
}

void startSetup() {
  networkMode = SETUP;
  WiFi.mode(WIFI_AP_STA);
  if (apPassword.isEmpty()) {
    apPassword = makeSetupPassword();
    settings.putString("apkey", apPassword);
  }
  if (!WiFi.softAP(apName.c_str(), apPassword.c_str())) {
    Serial.println("ERROR: Could not start setup network");
  }
  dns.start(53, "*", WiFi.softAPIP());
  screenDirty = true;
}

void startConnecting() {
  networkMode = CONNECTING;
  connectStarted = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  screenDirty = true;
}

void logStatus() {
  Serial.print("Wi-Fi: ");
  Serial.print(networkMode == SETUP ? "setup" :
               networkMode == CONNECTED ? "connected" : "connecting");
  Serial.print(" | ");
  Serial.print(batteryText());
  if (powerReady) {
    Serial.print(" | USB: ");
    Serial.print(power.isVbusIn() ? "yes" : "no");
    Serial.print(" | charging: ");
    Serial.print(power.isCharging() ? "yes" : "no");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  settings.begin("starter", false);
  pinMode(0, INPUT_PULLUP);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  uint64_t chipId = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06X", static_cast<unsigned>(chipId & 0xFFFFFF));
  apName = "Gridley-" + String(suffix);
  apPassword = settings.getString("apkey", "");
  savedSsid = settings.getString("ssid", "");
  savedPassword = settings.getString("password", "");
  areaLabel = settings.getString("area", "60612");
  if (!loadBars(settings.getString("bars", DEFAULT_BARS))) {
    loadBars(DEFAULT_BARS);
  }

  Wire.begin(IIC_SDA, IIC_SCL);
  powerReady = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (powerReady) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableVbusVoltageMeasure();
    power.enableSystemVoltageMeasure();
    power.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_100MA);
    power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  } else Serial.println("WARNING: AXP2101 power manager not found");

  displayReady = gfx->begin();
  if (displayReady) gfx->setBrightness(160);
  else Serial.println("WARNING: Display did not initialize");

  touch.setPins(TP_RST, TP_INT);
  touchReady = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  if (touchReady) {
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
  } else Serial.println("WARNING: Touch controller not found");

  configureWeb();
  if (savedSsid.isEmpty()) startSetup();
  else startConnecting();
  web.begin();
  drawScreen();
}

void loop() {
  if (networkMode == SETUP) dns.processNextRequest();
  web.handleClient();
  if (networkMode == CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      networkMode = CONNECTED;
      screenDirty = true;
    } else if (millis() - connectStarted > 20000) {
      WiFi.disconnect();
      startSetup();
    }
  } else if (networkMode == CONNECTED && WiFi.status() != WL_CONNECTED) {
    startConnecting();
  }

  if (digitalRead(0) == LOW) {
    if (bootPressedAt == 0) bootPressedAt = millis();
    if (millis() - bootPressedAt > 3000) {
      settings.remove("ssid");
      settings.remove("password");
      Serial.println("Wi-Fi settings cleared");
      ESP.restart();
    }
  } else bootPressedAt = 0;

  if (restartAt && static_cast<int32_t>(millis() - restartAt) >= 0) ESP.restart();
  pollTouch();
  updateSpin();
  if (screenDirty || (screen == STATUS_SETTINGS && millis() - lastDraw > 5000)) {
    drawScreen();
    screenDirty = false;
    lastDraw = millis();
  }
  if (millis() - lastLog > 5000) {
    logStatus();
    lastLog = millis();
  }
  delay(10);
}

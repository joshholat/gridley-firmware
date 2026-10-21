#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include "esp_random.h"
#include "pin_config.h"
#include "XPowersLib.h"

// A small first program for Waveshare's aluminum-case 1.75C board.
// Wi-Fi credentials are entered on a phone or computer, never in this sketch.

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

XPowersPMU power;
Preferences settings;
WebServer web(80);
DNSServer dns;

enum ScreenMode { CONNECTING, SETUP, CONNECTED };
ScreenMode mode = CONNECTING;
bool displayReady = false;
bool powerReady = false;
String apName;
String apPassword;
String savedSsid;
String savedPassword;
unsigned long connectStarted = 0;
unsigned long restartAt = 0;
unsigned long bootPressedAt = 0;
unsigned long lastDraw = 0;
unsigned long lastLog = 0;
bool screenDirty = true;

String makeSetupPassword() {
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String result;
  for (int i = 0; i < 12; ++i) {
    result += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  return result;
}

void line(const String &text, int y, uint16_t color = 0xFFFF) {
  if (!displayReady) return;
  gfx->setTextColor(color);
  gfx->setTextSize(2);
  int x = (LCD_WIDTH - static_cast<int>(text.length()) * 12) / 2;
  if (x < 20) x = 20;
  gfx->setCursor(x, y);
  gfx->print(text);
}

String batteryText() {
  if (!powerReady) return "Battery: unknown";
  if (!power.isBatteryConnect()) return "Battery: not detected";
  const uint16_t millivolts = power.getBattVoltage();
  return "Battery: " + String(millivolts / 1000.0f, 2) + " V";
}

void drawScreen() {
  if (!displayReady) return;
  gfx->fillScreen(0x0000);
  line("WAVESHARE STARTER", 84, 0x07FF);

  if (mode == SETUP) {
    line("Wi-Fi setup", 130);
    line("Join this network:", 168);
    line(apName, 200, 0xFFE0);
    line("Password:", 238);
    line(apPassword, 270, 0xFFE0);
    line("Open 192.168.4.1", 316);
  } else if (mode == CONNECTING) {
    line("Connecting to Wi-Fi", 150);
    line(savedSsid.substring(0, 25), 190, 0xFFE0);
  } else {
    line("Wi-Fi connected", 136, 0x07E0);
    line(savedSsid.substring(0, 25), 176);
    line("IP: " + WiFi.localIP().toString(), 220, 0xFFE0);
    line("Hold BOOT 3s: setup", 285);
  }

  line(batteryText(), 356);
  if (powerReady) {
    line(power.isVbusIn() ? "USB power: yes" : "USB power: no", 382);
    line(power.isCharging() ? "Charging: yes" : "Charging: no", 408);
  }
}

void sendSetupPage() {
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("Content-Security-Policy",
                 "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'");
  web.send(200, "text/html",
           "<!doctype html><html><meta name='viewport' content='width=device-width,"
           "initial-scale=1'><title>Waveshare Wi-Fi setup</title>"
           "<style>body{font:18px system-ui;max-width:32rem;margin:3rem auto;"
           "padding:0 1rem}label{display:block;margin:1rem 0}input{display:block;"
           "font:inherit;width:100%;box-sizing:border-box;padding:.6rem}"
           "button{font:inherit;padding:.7rem 1.2rem}</style>"
           "<h1>Waveshare Wi-Fi setup</h1>"
           "<p>Enter the name and password of a 2.4 GHz Wi-Fi network.</p>"
           "<form method='post' action='/save'>"
           "<label>Network name<input name='ssid' maxlength='32' required></label>"
           "<label>Wi-Fi password<input name='password' type='password' "
           "maxlength='63'></label><button type='submit'>Connect</button></form>"
           "</html>");
}

void saveWifi() {
  String ssid = web.arg("ssid");
  String password = web.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32 ||
      (!password.isEmpty() && (password.length() < 8 || password.length() > 63))) {
    web.send(400, "text/plain",
             "Enter a network name and a password of 8-63 characters, "
             "or leave the password empty for an open network.");
    return;
  }

  settings.putString("ssid", ssid);
  settings.putString("password", password);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html",
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<h1>Saved</h1><p>The display is restarting and will connect to Wi-Fi. "
           "If it cannot connect, this setup network will return.</p>");
  restartAt = millis() + 2500;
}

void startSetup() {
  mode = SETUP;
  WiFi.mode(WIFI_AP_STA);
  if (apPassword.isEmpty()) {
    apPassword = makeSetupPassword();
    settings.putString("apkey", apPassword);
  }
  if (!WiFi.softAP(apName.c_str(), apPassword.c_str())) {
    Serial.println("ERROR: Could not start setup network");
  }
  dns.start(53, "*", WiFi.softAPIP());
  web.on("/", HTTP_GET, sendSetupPage);
  web.on("/save", HTTP_POST, saveWifi);
  web.onNotFound([]() {
    web.sendHeader("Location", "http://192.168.4.1/");
    web.send(302, "text/plain", "Open http://192.168.4.1/");
  });
  web.begin();
  Serial.println("Setup network: " + apName);
  Serial.println("Setup page: http://192.168.4.1/");
  screenDirty = true;
}

void startConnecting() {
  mode = CONNECTING;
  connectStarted = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  screenDirty = true;
}

void logStatus() {
  Serial.print("Wi-Fi: ");
  Serial.print(mode == SETUP ? "setup" :
               mode == CONNECTED ? "connected" : "connecting");
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
  // USB logs must never stall the application when it runs on battery.
  Serial.setTxTimeoutMs(0);
  delay(300);
  settings.begin("starter", false);
  pinMode(0, INPUT_PULLUP);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  uint64_t chipId = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06X",
           static_cast<unsigned>(chipId & 0xFFFFFF));
  apName = "Waveshare-" + String(suffix);
  apPassword = settings.getString("apkey", "");
  savedSsid = settings.getString("ssid", "");
  savedPassword = settings.getString("password", "");

  Wire.begin(IIC_SDA, IIC_SCL);
  powerReady = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (powerReady) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableVbusVoltageMeasure();
    power.enableSystemVoltageMeasure();
    // A modest charge rate for the user's 320 mAh single-cell LiPo.
    power.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_100MA);
    power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  } else {
    Serial.println("WARNING: AXP2101 power manager not found");
  }

  displayReady = gfx->begin();
  if (displayReady) gfx->setBrightness(160);
  else Serial.println("WARNING: Display did not initialize");

  if (savedSsid.isEmpty()) startSetup();
  else startConnecting();
  drawScreen();
}

void loop() {
  if (mode == SETUP) {
    dns.processNextRequest();
    web.handleClient();
  } else if (mode == CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      mode = CONNECTED;
      screenDirty = true;
    } else if (millis() - connectStarted > 20000) {
      WiFi.disconnect();
      startSetup();
    }
  } else if (WiFi.status() != WL_CONNECTED) {
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
  } else {
    bootPressedAt = 0;
  }

  if (restartAt && static_cast<int32_t>(millis() - restartAt) >= 0) {
    ESP.restart();
  }

  if (screenDirty || millis() - lastDraw > 5000) {
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

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <math.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <freertos/queue.h>
#include <time.h>
#include <ImuDrv.hpp>
#include "TouchDrvCST.hpp"
#include "es8311.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "osm_root_ca.h"
#include "pin_config.h"
#include "XPowersLib.h"

#if __has_include("audio_clip.h")
#include "audio_clip.h"
#define HAS_AUDIO_CLIP 1
#else
#define HAS_AUDIO_CLIP 0
#endif

// Gridley launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// The existing Wi-Fi and battery settings remain in the "starter" namespace.

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

XPowersPMU power;
TouchDrvCST92xx touch;
SensorQMI8658 imu;
Preferences settings;
WebServer web(80);
DNSServer dns;
I2SClass audioI2S;
es8311_handle_t audioCodec = nullptr;

enum NetworkMode { CONNECTING, SETUP, CONNECTED };
enum AppScreen { LAUNCHER, NIGHT_SPINNER, GRID_MAZE, STATUS_SETTINGS };
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
String manualAreaLabel = "60612";
bool autoBars = true;
bool usingOsmBars = false;
bool mdnsReady = false;
String barFetchStatus = "Waiting for Wi-Fi";
unsigned long nextBarRefreshAt = 0;
bool barFetchRunning = false;

// The 60612 ZIP centroid is only an approximate home location. Gridley has no GPS.
constexpr float BAR_LATITUDE = 41.8812f;
constexpr float BAR_LONGITUDE = -87.6865f;
constexpr int BAR_RADIUS_METERS = 3200;
constexpr uint32_t BAR_REFRESH_MS = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t BAR_RETRY_MS = 60UL * 60UL * 1000UL;
constexpr uint32_t BAR_MANUAL_MIN_MS = 5UL * 60UL * 1000UL;
unsigned long lastBarRequestAt = 0;

struct FetchedBar {
  char name[33];
  char address[46];
};
struct BarFetchResult {
  bool success;
  uint8_t count;
  FetchedBar bars[MAX_BARS];
  char error[64];
};
QueueHandle_t barFetchQueue = nullptr;

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
constexpr uint16_t GRID_BRIGHT = 0x07FF;
constexpr uint16_t GRID_MID = 0x04DF;
constexpr uint16_t GRID_DIM = 0x0297;
constexpr uint16_t MAZE_FLOOR = 0x000C;
constexpr uint16_t MAZE_WALL = 0x0824;
constexpr uint16_t MAZE_GRID = 0x19B0;
constexpr uint16_t MAZE_GOLD = 0xFEA0;
constexpr uint16_t MAZE_ENEMY = 0xF81F;
constexpr int MAZE_SIZE = 9;
constexpr int MAZE_CELL = 28;
constexpr int MAZE_X = 107;
constexpr int MAZE_Y = 143;
constexpr uint32_t MAZE_STEP_MS = 170;
constexpr float MAZE_TILT_THRESHOLD = 0.17f;
constexpr float MAZE_DOWN_TILT_THRESHOLD = 0.32f;
constexpr uint32_t MAZE_BURST_MS = 960;
constexpr uint32_t MAZE_RESTART_MS = 3600;
constexpr uint32_t MAZE_ENEMY_STEP_MS = 650;
constexpr uint32_t MAZE_CAUGHT_MS = 1800;
char mazeCells[MAZE_SIZE][MAZE_SIZE + 1];
constexpr int VOLUME_TRACK_X = 139;
constexpr int VOLUME_TRACK_WIDTH = 188;
constexpr int VOLUME_BUTTON_Y = 348;
constexpr int VOLUME_BUTTON_SIZE = 43;
constexpr int VOLUME_MINUS_X = 83;
constexpr int VOLUME_PLUS_X = 340;

bool displayReady = false;
bool powerReady = false;
bool touchReady = false;
bool imuReady = false;
bool audioReady = false;
bool audioPlaying = false;
bool screenDirty = true;
uint8_t audioVolume = 60;
volatile bool touchPending = false;
String apName;
String apPassword;
String settingsBatterySnapshot;
String settingsPowerSnapshot;
String savedSsid;
String savedPassword;
unsigned long connectStarted = 0;
unsigned long restartAt = 0;
unsigned long bootPressedAt = 0;
unsigned long bootLastChangeAt = 0;
unsigned long bootLastTapAt = 0;
unsigned long audioPowerDownAt = 0;
unsigned long lastLog = 0;
unsigned long lastTap = 0;
size_t audioOffset = 0;
bool bootRawPressed = false;
bool bootStablePressed = false;
int mazeRow = 1;
int mazeCol = 1;
bool mazeWon = false;
bool mazeCaught = false;
float mazeTiltX = 0;
float mazeTiltY = 0;
unsigned long lastMazeStep = 0;
unsigned long mazeWonAt = 0;
unsigned long mazeCaughtAt = 0;
unsigned long lastEnemyStep = 0;
int mazeWinFrame = -1;
int enemyRow = -1;
int enemyCol = -1;
int enemyPrevRow = -1;
int enemyPrevCol = -1;

constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint32_t BOOT_DEBOUNCE_MS = 35;
constexpr uint32_t BOOT_DOUBLE_TAP_MS = 450;
constexpr uint32_t BOOT_LONG_PRESS_MS = 3000;

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

void useManualBars() {
  if (!loadBars(settings.getString("bars", DEFAULT_BARS))) loadBars(DEFAULT_BARS);
  areaLabel = manualAreaLabel;
  usingOsmBars = false;
  selectedBar = displayedBar = -1;
  spinning = false;
  screenDirty = true;
}

void useCachedOsmBars() {
  if (loadBars(settings.getString("osmBars", ""))) {
    areaLabel = "60612";
    usingOsmBars = true;
    selectedBar = displayedBar = -1;
    spinning = false;
    screenDirty = true;
  } else {
    useManualBars();
  }
}

String formEncode(const String &input) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(input.length() * 3);
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(input[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 15];
    }
  }
  return encoded;
}

bool parseOsmBars(const String &csv, BarFetchResult &result) {
  int start = csv.indexOf('\n');
  if (start < 0) return false;
  String header = csv.substring(0, start);
  header.trim();
  if (!header.startsWith("name\t") ||
      header.indexOf("addr:housenumber") < 0 ||
      header.indexOf("addr:street") < 0) return false;

  int valid = 0;
  ++start;
  while (start < csv.length()) {
    int end = csv.indexOf('\n', start);
    if (end < 0) end = csv.length();
    String line = csv.substring(start, end);
    start = end + 1;
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (line.isEmpty()) continue;
    int a = line.indexOf('\t');
    int b = a < 0 ? -1 : line.indexOf('\t', a + 1);
    int c = b < 0 ? -1 : line.indexOf('\t', b + 1);
    if (a < 0 || b < 0 || c < 0 || line.indexOf('\t', c + 1) >= 0) continue;
    String name = line.substring(0, a);
    String number = line.substring(a + 1, b);
    String street = line.substring(b + 1, c);
    String full = line.substring(c + 1);
    name.trim(); number.trim(); street.trim(); full.trim();
    if (name.isEmpty() || name.length() > 32 || name.indexOf('|') >= 0) continue;
    String address = full;
    if (address.isEmpty()) address = number + (number.isEmpty() || street.isEmpty() ? "" : " ") + street;
    if (address.isEmpty()) address = "Address not listed";
    address.replace('|', '/');
    if (address.length() > 45) address = address.substring(0, 45);

    bool duplicate = false;
    for (uint8_t i = 0; i < result.count; ++i) {
      if (name.equalsIgnoreCase(result.bars[i].name) &&
          address.equalsIgnoreCase(result.bars[i].address)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    ++valid;
    uint32_t slot = result.count < MAX_BARS ? result.count : esp_random() % valid;
    if (slot < MAX_BARS) {
      name.toCharArray(result.bars[slot].name, sizeof(result.bars[slot].name));
      address.toCharArray(result.bars[slot].address, sizeof(result.bars[slot].address));
      if (result.count < MAX_BARS) ++result.count;
    }
  }
  return result.count > 0;
}

void fetchOsmBarsTask(void *) {
  BarFetchResult result = {};
  unsigned long clockStarted = millis();
  while (time(nullptr) < 1700000000 && millis() - clockStarted < 12000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (time(nullptr) < 1700000000) {
    snprintf(result.error, sizeof(result.error), "Internet unavailable; using saved bars");
  } else {
    WiFiClientSecure client;
    client.setCACert(OSM_ROOT_CA);
    const String query =
        "[out:csv(name,\"addr:housenumber\",\"addr:street\",\"addr:full\")][timeout:20];"
        "nwr(around:" + String(BAR_RADIUS_METERS) + "," +
        String(BAR_LATITUDE, 4) + "," + String(BAR_LONGITUDE, 4) +
        ")[\"amenity\"~\"^(bar|pub)$\"][\"name\"];out 120;";
    const String body = "data=" + formEncode(query);
    const char *servers[] = {
        "https://overpass.private.coffee/api/interpreter",
        "https://overpass-api.de/api/interpreter"};
    for (const char *server : servers) {
      HTTPClient http;
      http.setConnectTimeout(7000);
      http.setTimeout(25000);
      if (!http.begin(client, server)) {
        snprintf(result.error, sizeof(result.error), "Map service unavailable; using saved bars");
        continue;
      }
      http.setUserAgent("Gridley/1.0 (personal ESP32-S3 bar spinner)");
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      int code = http.POST(body);
      if (code == HTTP_CODE_OK && http.getSize() <= 30000) {
        String csv = http.getString();
        result.success = csv.length() <= 30000 && parseOsmBars(csv, result);
        if (!result.success) snprintf(result.error, sizeof(result.error), "Map data unavailable; using saved bars");
      } else {
        snprintf(result.error, sizeof(result.error), "Map service unavailable; using saved bars");
      }
      if (!result.success) {
        char tlsError[96] = {};
        client.lastError(tlsError, sizeof(tlsError));
        Serial.printf("OpenStreetMap %s: HTTP %d (%s); TLS: %s\n", server,
                      code, HTTPClient::errorToString(code).c_str(), tlsError);
      }
      http.end();
      if (result.success || code == 406 || code == 429) break;
    }
  }
  xQueueSend(barFetchQueue, &result, portMAX_DELAY);
  vTaskDelete(nullptr);
}

void processBarFetchResult() {
  if (!barFetchQueue || spinning) return;
  BarFetchResult result;
  if (xQueueReceive(barFetchQueue, &result, 0) != pdPASS) return;
  barFetchRunning = false;
  nextBarRefreshAt = millis() + (result.success ? BAR_REFRESH_MS : BAR_RETRY_MS);
  if (!result.success) {
    barFetchStatus = result.error;
    Serial.println("OpenStreetMap refresh: " + barFetchStatus);
    return;
  }
  String saved;
  for (uint8_t i = 0; i < result.count; ++i) {
    saved += String(result.bars[i].name) + " | " + result.bars[i].address + "\n";
  }
  settings.putString("osmBars", saved);
  barFetchStatus = "Updated from OpenStreetMap";
  Serial.printf("OpenStreetMap refresh: %u bars\n", result.count);
  if (autoBars && loadBars(saved)) {
    usingOsmBars = true;
    areaLabel = "60612";
    selectedBar = displayedBar = -1;
    screenDirty = true;
  }
}

void maybeRefreshBars() {
  if (!autoBars || networkMode != CONNECTED || barFetchRunning ||
      !barFetchQueue || screen == GRID_MAZE ||
      static_cast<int32_t>(millis() - nextBarRefreshAt) < 0 ||
      (lastBarRequestAt && millis() - lastBarRequestAt < BAR_MANUAL_MIN_MS)) return;
  barFetchRunning = true;
  lastBarRequestAt = millis();
  barFetchStatus = "Refreshing from OpenStreetMap...";
  if (xTaskCreatePinnedToCore(fetchOsmBarsTask, "osm-bars", 12288, nullptr,
                              1, nullptr, 0) != pdPASS) {
    barFetchRunning = false;
    barFetchStatus = "Could not start map refresh";
    nextBarRefreshAt = millis() + BAR_RETRY_MS;
  }
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

void headlineCentered(const String &value, int y) {
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  int x = (LCD_WIDTH - static_cast<int>(value.length()) * 18) / 2;
  gfx->setCursor(x, y);
  gfx->print(value);
  gfx->setCursor(x + 1, y);
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
  // Thick, bright lines remain visible on the small round AMOLED.
  // The upper and lower planes recede into the dark menu corridor.
  const int topRows[] = {18, 37, 55, 72, 88, 104, 118, 130, 140, 149, 157, 164};
  const int bottomRows[] = {448, 429, 411, 394, 378, 362, 348, 336, 326, 317, 309, 302};
  for (int i = 0; i < 12; ++i) {
    uint16_t color = i < 5 ? GRID_BRIGHT : i < 9 ? GRID_MID : GRID_DIM;
    gfx->fillRect(0, topRows[i], LCD_WIDTH, 2, color);
    gfx->fillRect(0, bottomRows[i], LCD_WIDTH, 2, color);
  }
  for (int x = 0; x < LCD_WIDTH; x += 42) {
    int vanishingX = 233 + (x - 233) / 16;
    int topMidX = x + (vanishingX - x) * 112 / 168;
    int bottomMidX = x + (vanishingX - x) * 112 / 167;
    gfx->drawLine(x, 0, topMidX, 112, GRID_BRIGHT);
    gfx->drawLine(x + 1, 0, topMidX + 1, 112, GRID_BRIGHT);
    gfx->drawLine(topMidX, 112, vanishingX, 168, GRID_MID);
    gfx->drawLine(topMidX + 1, 112, vanishingX + 1, 168, GRID_MID);
    gfx->drawLine(x, 465, bottomMidX, 353, GRID_BRIGHT);
    gfx->drawLine(x + 1, 465, bottomMidX + 1, 353, GRID_BRIGHT);
    gfx->drawLine(bottomMidX, 353, vanishingX, 298, GRID_MID);
    gfx->drawLine(bottomMidX + 1, 353, vanishingX + 1, 298, GRID_MID);
  }
}

void chrome(const String &title, bool showBack = true, bool grid = false) {
  gfx->fillScreen(BG);
  if (grid) drawPerspectiveGrid();
  gfx->drawCircle(233, 233, 224, BLUE);
  gfx->drawCircle(233, 233, 218, PANEL);
  if (showBack) {
    button(91, 91, 93, 45, "< BACK", BLUE);
  }
  if (!title.isEmpty()) textCentered(title, showBack ? 65 : 88, 2, CYAN);
}

String batteryText() {
  if (!powerReady) return "Battery: unknown";
  if (!power.isBatteryConnect()) return "Battery: not detected";
  return "Battery: " + String(power.getBattVoltage() / 1000.0f, 2) + " V";
}

String powerStatusText() {
  if (!powerReady) return "";
  return String("USB: ") + (power.isVbusIn() ? "yes" : "no") +
         "   CHARGING: " + (power.isCharging() ? "yes" : "no");
}

void drawLauncher() {
  chrome("", false, true);
  gfx->fillRoundRect(151, 51, 164, 46, 12, BG);
  headlineCentered("GRIDLEY", 63);
  gfx->fillRoundRect(91, 100, 284, 37, 12, BG);
  textCentered("PICK A PROGRAM, USER", 110, 2, CYAN);
  button(84, 145, 298, 64, "CHICAGO SPINNER", CYAN);
  button(84, 224, 298, 64, "GRID MAZE", MAZE_GOLD);
  button(84, 303, 298, 64, "SETTINGS", PURPLE);
#if HAS_AUDIO_CLIP
  textCentered("BOOT x2: PLAY SOUND", 385, 1, MUTED);
#endif
}

void generateMaze() {
  for (int row = 0; row < MAZE_SIZE; ++row) {
    for (int col = 0; col < MAZE_SIZE; ++col) mazeCells[row][col] = '#';
    mazeCells[row][MAZE_SIZE] = '\0';
  }

  // Random depth-first carving visits every room, so start and exit connect.
  bool visited[4][4] = {};
  uint8_t stackRow[16] = {0};
  uint8_t stackCol[16] = {0};
  const int8_t dRow[] = {-1, 1, 0, 0};
  const int8_t dCol[] = {0, 0, -1, 1};
  int top = 0;
  visited[0][0] = true;
  mazeCells[1][1] = ' ';
  while (top >= 0) {
    int roomRow = stackRow[top];
    int roomCol = stackCol[top];
    int choices[4];
    int count = 0;
    for (int dir = 0; dir < 4; ++dir) {
      int nextRow = roomRow + dRow[dir];
      int nextCol = roomCol + dCol[dir];
      if (nextRow >= 0 && nextRow < 4 && nextCol >= 0 && nextCol < 4 &&
          !visited[nextRow][nextCol]) choices[count++] = dir;
    }
    if (count == 0) {
      --top;
      continue;
    }
    int dir = choices[esp_random() % count];
    int nextRow = roomRow + dRow[dir];
    int nextCol = roomCol + dCol[dir];
    mazeCells[1 + 2 * roomRow + dRow[dir]]
             [1 + 2 * roomCol + dCol[dir]] = ' ';
    mazeCells[1 + 2 * nextRow][1 + 2 * nextCol] = ' ';
    visited[nextRow][nextCol] = true;
    stackRow[++top] = nextRow;
    stackCol[top] = nextCol;
  }

  // A couple of loops leave room to dodge the moving sentinel.
  int added = 0;
  for (int attempt = 0; attempt < 80 && added < 2; ++attempt) {
    int row = 1 + esp_random() % 7;
    int col = 1 + esp_random() % 7;
    if (mazeCells[row][col] != '#') continue;
    bool horizontal = row % 2 == 1 && col % 2 == 0 &&
                      mazeCells[row][col - 1] != '#' &&
                      mazeCells[row][col + 1] != '#';
    bool vertical = row % 2 == 0 && col % 2 == 1 &&
                    mazeCells[row - 1][col] != '#' &&
                    mazeCells[row + 1][col] != '#';
    if (horizontal || vertical) {
      mazeCells[row][col] = ' ';
      ++added;
    }
  }
  mazeCells[1][1] = 'S';
  mazeCells[7][7] = 'G';
}

bool mazeWall(int row, int col) {
  return row < 0 || row >= MAZE_SIZE || col < 0 || col >= MAZE_SIZE ||
         mazeCells[row][col] == '#';
}

void placeMazeEnemy() {
  uint8_t candidatesRow[MAZE_SIZE * MAZE_SIZE];
  uint8_t candidatesCol[MAZE_SIZE * MAZE_SIZE];
  int count = 0;
  for (int row = 1; row < MAZE_SIZE - 1; ++row) {
    for (int col = 1; col < MAZE_SIZE - 1; ++col) {
      if (!mazeWall(row, col) && mazeCells[row][col] != 'G' &&
          abs(row - 1) + abs(col - 1) >= 7) {
        candidatesRow[count] = row;
        candidatesCol[count++] = col;
      }
    }
  }
  enemyRow = enemyCol = -1;
  if (count > 0) {
    int pick = esp_random() % count;
    enemyRow = candidatesRow[pick];
    enemyCol = candidatesCol[pick];
  }
  enemyPrevRow = enemyPrevCol = -1;
  lastEnemyStep = millis();
  Serial.printf("Grid Maze generated; sentinel at %d,%d\n", enemyRow, enemyCol);
}

void drawMazeCell(int row, int col) {
  int x = MAZE_X + col * MAZE_CELL;
  int y = MAZE_Y + row * MAZE_CELL;
  bool wall = mazeWall(row, col);
  gfx->fillRect(x, y, MAZE_CELL, MAZE_CELL, wall ? MAZE_WALL : MAZE_FLOOR);
  gfx->drawRect(x, y, MAZE_CELL, MAZE_CELL, MAZE_GRID);
  if (wall) return;

  // Bright edges face the playable route; the dim tiles make a Tron grid.
  if (mazeWall(row - 1, col)) gfx->fillRect(x, y, MAZE_CELL, 3, CYAN);
  if (mazeWall(row + 1, col)) gfx->fillRect(x, y + MAZE_CELL - 3, MAZE_CELL, 3, CYAN);
  if (mazeWall(row, col - 1)) gfx->fillRect(x, y, 3, MAZE_CELL, CYAN);
  if (mazeWall(row, col + 1)) gfx->fillRect(x + MAZE_CELL - 3, y, 3, MAZE_CELL, CYAN);
  if (mazeCells[row][col] == 'G') {
    gfx->drawCircle(x + 14, y + 14, 10, WHITE);
    gfx->fillCircle(x + 14, y + 14, 7, MAZE_GOLD);
  } else if (mazeCells[row][col] == 'S') {
    gfx->drawCircle(x + 14, y + 14, 9, BLUE);
  }
}

void drawMazePlayer() {
  int x = MAZE_X + mazeCol * MAZE_CELL + MAZE_CELL / 2;
  int y = MAZE_Y + mazeRow * MAZE_CELL + MAZE_CELL / 2;
  gfx->fillCircle(x, y, 10, CYAN);
  gfx->fillCircle(x, y, 6, WHITE);
}

void drawMazeEnemy() {
  if (enemyRow < 0) return;
  int x = MAZE_X + enemyCol * MAZE_CELL + MAZE_CELL / 2;
  int y = MAZE_Y + enemyRow * MAZE_CELL + MAZE_CELL / 2;
  gfx->fillRect(x - 9, y - 9, 18, 18, MAZE_ENEMY);
  gfx->drawRect(x - 10, y - 10, 20, 20, WHITE);
  gfx->fillRect(x - 3, y - 3, 6, 6, BG);
}

void drawMazeBurst(int frame) {
  gfx->fillScreen(BG);
  gfx->drawCircle(233, 233, 224, BLUE);
  for (int p = 66; p <= 402; p += 28) {
    gfx->drawFastVLine(p, 66, 336, MAZE_WALL);
    gfx->drawFastHLine(66, p, 336, MAZE_WALL);
  }

  // Chunky color shards radiate like an 8-bit spray-paint explosion.
  const int8_t dx[] = {8, 7, 6, 3, 0, -3, -6, -7,
                       -8, -7, -6, -3, 0, 3, 6, 7};
  const int8_t dy[] = {0, 3, 6, 7, 8, 7, 6, 3,
                       0, -3, -6, -7, -8, -7, -6, -3};
  const uint16_t paint[] = {CYAN, MAZE_GOLD, PURPLE, WHITE, BLUE};
  int reach = 18 + frame * 20;
  for (int i = 0; i < 16; ++i) {
    int x = 233 + reach * dx[i] / 8;
    int y = 233 + reach * dy[i] / 8;
    int size = 6 + (i % 3) * 3;
    gfx->fillRect(x - size / 2, y - size / 2, size, size,
                  paint[(i + frame) % 5]);
    if (frame > 1) {
      int trailX = 233 + (reach - 27) * dx[i] / 8;
      int trailY = 233 + (reach - 27) * dy[i] / 8;
      gfx->fillRect(trailX - 2, trailY - 2, 5, 5, paint[(i + 2) % 5]);
    }
  }
  gfx->setTextSize(4);
  gfx->setTextColor(PURPLE);
  gfx->setCursor(165, 219);
  gfx->print("KAPOW!");
  gfx->setTextColor(CYAN);
  gfx->setCursor(161, 215);
  gfx->print("KAPOW!");
}

void drawMazeWinMessage() {
  gfx->fillScreen(BG);
  gfx->drawCircle(233, 233, 224, BLUE);
  for (int i = 0; i < 20; ++i) {
    int x = 87 + (i * 73) % 292;
    int y = 85 + (i * 109) % 295;
    gfx->fillRect(x, y, 5 + i % 3 * 3, 5 + i % 3 * 3,
                  i % 3 == 0 ? MAZE_GOLD : i % 3 == 1 ? CYAN : PURPLE);
  }
  gfx->fillRoundRect(71, 164, 324, 140, 18, MAZE_WALL);
  gfx->drawRoundRect(71, 164, 324, 140, 18, CYAN);
  textCentered("GRID CLEARED", 187, 3, WHITE);
  textCentered("CONGRATS, USER", 239, 2, MAZE_GOLD);
  textCentered("MAZE RESTARTING...", 279, 1, MUTED);
}

void drawMazeCaught() {
  gfx->fillScreen(BG);
  gfx->drawCircle(233, 233, 224, MAZE_ENEMY);
  for (int i = 0; i < 14; ++i) {
    int x = 95 + (i * 79) % 275;
    int y = 92 + (i * 113) % 280;
    gfx->fillRect(x, y, 7, 7, i % 2 ? MAZE_ENEMY : PURPLE);
  }
  gfx->fillRoundRect(76, 170, 314, 130, 18, MAZE_WALL);
  gfx->drawRoundRect(76, 170, 314, 130, 18, MAZE_ENEMY);
  textCentered("ZAPPED!", 190, 3, WHITE);
  textCentered("SENTINEL GOT YOU", 241, 2, MAZE_ENEMY);
  textCentered("NEW GRID INCOMING...", 279, 1, MUTED);
}

void drawMazeStatus() {
  gfx->fillRect(104, 407, 258, 23, BG);
  if (imuReady) textCentered("DODGE PINK  /  TILT OR TAP", 412, 1, MUTED);
  else textCentered("DODGE PINK  /  TAP TO MOVE", 412, 1, MAZE_GOLD);
}

void drawMaze() {
  if (mazeCaught) {
    drawMazeCaught();
    return;
  }
  if (mazeWon) {
    unsigned long elapsed = millis() - mazeWonAt;
    if (elapsed < MAZE_BURST_MS) drawMazeBurst(elapsed / 120);
    else drawMazeWinMessage();
    return;
  }
  chrome("GRID MAZE");
  textCentered("REACH THE GOLD EXIT", 121, 1, MUTED);
  for (int row = 0; row < MAZE_SIZE; ++row) {
    for (int col = 0; col < MAZE_SIZE; ++col) drawMazeCell(row, col);
  }
  gfx->drawRect(MAZE_X - 2, MAZE_Y - 2,
                MAZE_SIZE * MAZE_CELL + 4, MAZE_SIZE * MAZE_CELL + 4, CYAN);
  drawMazeEnemy();
  drawMazePlayer();
  drawMazeStatus();
}

void resetMaze() {
  generateMaze();
  placeMazeEnemy();
  mazeRow = 1;
  mazeCol = 1;
  mazeWon = false;
  mazeCaught = false;
  mazeTiltX = mazeTiltY = 0;
  lastMazeStep = 0;
  mazeWonAt = 0;
  mazeCaughtAt = 0;
  mazeWinFrame = -1;
  screenDirty = true;
}

void catchMazePlayer() {
  mazeCaught = true;
  mazeCaughtAt = millis();
  Serial.println("Grid Maze: sentinel caught player");
  drawMazeCaught();
}

bool stepMaze(int dRow, int dCol) {
  int nextRow = mazeRow + dRow;
  int nextCol = mazeCol + dCol;
  if (mazeWon || mazeCaught || mazeWall(nextRow, nextCol)) return false;
  int oldRow = mazeRow;
  int oldCol = mazeCol;
  mazeRow = nextRow;
  mazeCol = nextCol;
  if (mazeRow == enemyRow && mazeCol == enemyCol) {
    catchMazePlayer();
    return true;
  }
  mazeWon = mazeCells[mazeRow][mazeCol] == 'G';
  drawMazeCell(oldRow, oldCol);
  drawMazeCell(mazeRow, mazeCol);
  if (oldRow == enemyRow && oldCol == enemyCol) drawMazeEnemy();
  drawMazePlayer();
  if (mazeWon) {
    Serial.println("Grid Maze cleared");
    mazeWonAt = millis();
    mazeWinFrame = 0;
    drawMazeBurst(0);
  }
  return true;
}

void updateMazeCelebration() {
  unsigned long elapsed = millis() - mazeWonAt;
  if (elapsed >= MAZE_RESTART_MS) {
    resetMaze();
  } else if (elapsed >= MAZE_BURST_MS) {
    if (mazeWinFrame != 100) {
      drawMazeWinMessage();
      mazeWinFrame = 100;
    }
  } else {
    int frame = elapsed / 120;
    if (frame != mazeWinFrame) {
      drawMazeBurst(frame);
      mazeWinFrame = frame;
    }
  }
}

void updateMazeEnemy() {
  if (enemyRow < 0 || millis() - lastEnemyStep < MAZE_ENEMY_STEP_MS) return;
  lastEnemyStep = millis();
  const int8_t dRow[] = {-1, 1, 0, 0};
  const int8_t dCol[] = {0, 0, -1, 1};
  int options[4];
  int count = 0;
  for (int dir = 0; dir < 4; ++dir) {
    int row = enemyRow + dRow[dir];
    int col = enemyCol + dCol[dir];
    if (!mazeWall(row, col) && mazeCells[row][col] != 'G') options[count++] = dir;
  }
  if (count == 0) return;

  // The sentinel follows a clear straight corridor, otherwise it patrols.
  int chosen = -1;
  if (enemyRow == mazeRow && abs(enemyCol - mazeCol) <= 5) {
    int toward = mazeCol > enemyCol ? 1 : -1;
    bool clear = true;
    for (int col = enemyCol + toward; col != mazeCol; col += toward) {
      if (mazeWall(enemyRow, col)) { clear = false; break; }
    }
    if (clear) {
      for (int i = 0; i < count; ++i) {
        int dir = options[i];
        if (dRow[dir] == 0 && dCol[dir] == toward) chosen = dir;
      }
    }
  } else if (enemyCol == mazeCol && abs(enemyRow - mazeRow) <= 5) {
    int toward = mazeRow > enemyRow ? 1 : -1;
    bool clear = true;
    for (int row = enemyRow + toward; row != mazeRow; row += toward) {
      if (mazeWall(row, enemyCol)) { clear = false; break; }
    }
    if (clear) {
      for (int i = 0; i < count; ++i) {
        int dir = options[i];
        if (dCol[dir] == 0 && dRow[dir] == toward) chosen = dir;
      }
    }
  }
  if (chosen < 0) {
    int patrol[4];
    int patrolCount = 0;
    for (int i = 0; i < count; ++i) {
      int dir = options[i];
      if (count == 1 || enemyRow + dRow[dir] != enemyPrevRow ||
          enemyCol + dCol[dir] != enemyPrevCol) patrol[patrolCount++] = dir;
    }
    chosen = patrol[esp_random() % patrolCount];
  }

  int oldRow = enemyRow;
  int oldCol = enemyCol;
  enemyRow += dRow[chosen];
  enemyCol += dCol[chosen];
  enemyPrevRow = oldRow;
  enemyPrevCol = oldCol;
  if (enemyRow == mazeRow && enemyCol == mazeCol) {
    catchMazePlayer();
    return;
  }
  drawMazeCell(oldRow, oldCol);
  drawMazeCell(enemyRow, enemyCol);
  drawMazeEnemy();
}

void updateMaze() {
  if (screen != GRID_MAZE || !displayReady) return;
  if (mazeCaught) {
    if (millis() - mazeCaughtAt >= MAZE_CAUGHT_MS) resetMaze();
    return;
  }
  if (mazeWon) {
    updateMazeCelebration();
    return;
  }
  updateMazeEnemy();
  if (mazeCaught) return;
  if (!imuReady) return;
  unsigned long now = millis();
  if (now - lastMazeStep < MAZE_STEP_MS) return;
  AccelerometerData accel;
  if (!imu.readAccel(accel)) return;
  // The IMU is rotated relative to the display: Y drives screen X,
  // and X drives screen Y with the directions matched to physical tilt.
  mazeTiltX = 0.55f * mazeTiltX + 0.45f * accel.mps2.y / 9.80665f;
  mazeTiltY = 0.55f * mazeTiltY + 0.45f * accel.mps2.x / 9.80665f;
  float x = fabsf(mazeTiltX);
  float y = fabsf(mazeTiltY);
  int dCol = x >= MAZE_TILT_THRESHOLD ? (mazeTiltX > 0 ? 1 : -1) : 0;
  float rowThreshold = mazeTiltY > 0 ? MAZE_DOWN_TILT_THRESHOLD : MAZE_TILT_THRESHOLD;
  int dRow = y >= rowThreshold ? (mazeTiltY > 0 ? 1 : -1) : 0;
  if (x >= y) {
    if (!dCol || !stepMaze(0, dCol)) {
      if (dRow) stepMaze(dRow, 0);
    }
  } else {
    if (!dRow || !stepMaze(dRow, 0)) {
      if (dCol) stepMaze(0, dCol);
    }
  }
  lastMazeStep = now;
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
  if (usingOsmBars) textCentered("Data: OpenStreetMap contributors", 418, 1, MUTED);
}

String programFlashText() {
  const esp_partition_t *partition = esp_ota_get_running_partition();
  uint32_t usedBytes = ESP.getSketchSize();
  if (!partition || !partition->size || !usedBytes) {
    return "Program flash: unavailable";
  }
  uint32_t percent = (static_cast<uint64_t>(usedBytes) * 100 +
                      partition->size / 2) / partition->size;
  return "Program flash: " + String(min(percent, uint32_t(100))) + "% used";
}

void drawSettings() {
  chrome("SETTINGS");
  textCentered(settingsBatterySnapshot, 145, 2, WHITE);
  if (!settingsPowerSnapshot.isEmpty())
    textCentered(settingsPowerSnapshot, 173, 2, MUTED);
  if (networkMode == CONNECTED) {
    textCentered("Wi-Fi connected", 209, 2, CYAN);
    textCentered(WiFi.localIP().toString(), 238, 2, WHITE);
    textCentered(mdnsReady ? "Open gridley.local on phone" :
                 "Open IP on phone", 265, 1, MUTED);
  } else if (networkMode == SETUP) {
    textCentered("Wi-Fi setup network", 207, 2, CYAN);
    textCentered(apName, 232, 2, WHITE);
    textCentered(apPassword, 257, 2, WHITE);
    textCentered("Open 192.168.4.1", 282, 1, MUTED);
  } else {
    textCentered("Connecting to Wi-Fi", 220, 2, CYAN);
  }
  textCentered(String(usingOsmBars ? "OSM" : "Saved") + ": " + areaLabel +
               " / " + String(barCount) + " bars", 294, 1, MUTED);
  textCentered(programFlashText(), 307, 2, CYAN);
  textCentered("VOLUME " + String(audioVolume) + "%", 327, 2, WHITE);

  gfx->fillRoundRect(VOLUME_MINUS_X, VOLUME_BUTTON_Y,
                     VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE, 12, PANEL);
  gfx->drawRoundRect(VOLUME_MINUS_X, VOLUME_BUTTON_Y,
                     VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE, 12, CYAN);
  gfx->fillRect(VOLUME_MINUS_X + 12, VOLUME_BUTTON_Y + 20, 19, 3, WHITE);
  gfx->fillRoundRect(VOLUME_PLUS_X, VOLUME_BUTTON_Y,
                     VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE, 12, PANEL);
  gfx->drawRoundRect(VOLUME_PLUS_X, VOLUME_BUTTON_Y,
                     VOLUME_BUTTON_SIZE, VOLUME_BUTTON_SIZE, 12, CYAN);
  gfx->fillRect(VOLUME_PLUS_X + 12, VOLUME_BUTTON_Y + 20, 19, 3, WHITE);
  gfx->fillRect(VOLUME_PLUS_X + 20, VOLUME_BUTTON_Y + 12, 3, 19, WHITE);

  int filled = VOLUME_TRACK_WIDTH * audioVolume / 100;
  gfx->fillRoundRect(VOLUME_TRACK_X, 364, VOLUME_TRACK_WIDTH, 12, 6, PANEL);
  if (filled > 0) gfx->fillRoundRect(VOLUME_TRACK_X, 364, filled, 12, 6, CYAN);
  gfx->fillCircle(VOLUME_TRACK_X + filled, 370, 14, WHITE);
#if HAS_AUDIO_CLIP
  textCentered("BOOT x2: play sound", 405, 1, MUTED);
#endif
  textCentered("Hold BOOT 3s: reset Wi-Fi", 419, 1, MUTED);
}

bool initAudio() {
  pinMode(AUDIO_PA, OUTPUT);
  digitalWrite(AUDIO_PA, LOW);
  audioI2S.setPins(AUDIO_BCLK, AUDIO_WS, AUDIO_DOUT, AUDIO_DIN, AUDIO_MCLK);
  if (!audioI2S.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE,
                      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                      I2S_STD_SLOT_BOTH)) {
    Serial.println("WARNING: I2S speaker bus did not initialize");
    return false;
  }

  audioCodec = es8311_create(0, ES8311_ADDRRES_0);
  if (!audioCodec) {
    Serial.println("WARNING: ES8311 codec did not initialize");
    audioI2S.end();
    return false;
  }
  es8311_clock_config_t clock = {};
  clock.mclk_from_mclk_pin = true;
  clock.mclk_frequency = AUDIO_SAMPLE_RATE * 256;
  clock.sample_frequency = AUDIO_SAMPLE_RATE;
  if (es8311_init(audioCodec, &clock, ES8311_RESOLUTION_16,
                  ES8311_RESOLUTION_16) != ESP_OK ||
      es8311_sample_frequency_config(audioCodec, clock.mclk_frequency,
                                     clock.sample_frequency) != ESP_OK ||
      es8311_microphone_config(audioCodec, false) != ESP_OK ||
      es8311_voice_volume_set(audioCodec, audioVolume, nullptr) != ESP_OK) {
    Serial.println("WARNING: ES8311 speaker setup failed");
    es8311_delete(audioCodec);
    audioCodec = nullptr;
    audioI2S.end();
    return false;
  }
  Serial.println("Speaker ready");
  return true;
}

void setAudioVolume(uint8_t value) {
  if (value == audioVolume) return;
  audioVolume = value;
  settings.putUChar("volume", audioVolume);
  if (audioReady && audioCodec &&
      es8311_voice_volume_set(audioCodec, audioVolume, nullptr) != ESP_OK) {
    Serial.println("WARNING: Could not update speaker volume");
  }
  Serial.printf("Speaker volume: %u%%\n", audioVolume);
  screenDirty = true;
}

void playAudioClip() {
#if HAS_AUDIO_CLIP
  if (!audioReady) {
    Serial.println("Speaker unavailable");
    return;
  }
  audioOffset = 0;
  audioPowerDownAt = 0;
  audioPlaying = true;
  digitalWrite(AUDIO_PA, HIGH);
  Serial.println("BOOT double tap: playing sound");
#else
  Serial.println("BOOT double tap: run tools/prepare_audio.py to add a clip");
#endif
}

void pumpAudio() {
#if HAS_AUDIO_CLIP
  if (audioPlaying) {
    size_t remaining = AUDIO_CLIP_BYTES - audioOffset;
    size_t count = min(remaining, static_cast<size_t>(2048));
    audioOffset += audioI2S.write(AUDIO_CLIP_PCM + audioOffset, count);
    if (audioOffset >= AUDIO_CLIP_BYTES) {
      audioPlaying = false;
      audioPowerDownAt = millis() + 150;
      Serial.println("Sound finished");
    }
  }
  if (audioPowerDownAt &&
      static_cast<int32_t>(millis() - audioPowerDownAt) >= 0) {
    digitalWrite(AUDIO_PA, LOW);
    audioPowerDownAt = 0;
  }
#endif
}

void pollBootButton() {
  unsigned long now = millis();
  bool pressed = digitalRead(0) == LOW;
  if (pressed != bootRawPressed) {
    bootRawPressed = pressed;
    bootLastChangeAt = now;
  }
  if (pressed == bootStablePressed || now - bootLastChangeAt < BOOT_DEBOUNCE_MS) {
    return;
  }
  bootStablePressed = pressed;
  if (pressed) {
    bootPressedAt = now;
    return;
  }

  unsigned long held = now - bootPressedAt;
  if (held >= BOOT_LONG_PRESS_MS) {
    bootLastTapAt = 0;
    settings.remove("ssid");
    settings.remove("password");
    Serial.println("Wi-Fi settings cleared");
    // Restart after release so GPIO0 cannot put the board into flashing mode.
    ESP.restart();
  } else if (held < 600) {
    if (bootLastTapAt && now - bootLastTapAt <= BOOT_DOUBLE_TAP_MS) {
      bootLastTapAt = 0;
      playAudioClip();
    } else {
      bootLastTapAt = now;
    }
  } else {
    bootLastTapAt = 0;
  }
}

void drawScreen() {
  if (!displayReady) return;
  switch (screen) {
    case LAUNCHER: drawLauncher(); break;
    case NIGHT_SPINNER: drawSpinner(); break;
    case GRID_MAZE: drawMaze(); break;
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
    if (x >= 84 && x <= 382 && y >= 145 && y <= 209) screen = NIGHT_SPINNER;
    else if (x >= 84 && x <= 382 && y >= 224 && y <= 288) {
      resetMaze();
      screen = GRID_MAZE;
    } else if (x >= 84 && x <= 382 && y >= 303 && y <= 367) {
      settingsBatterySnapshot = batteryText();
      settingsPowerSnapshot = powerStatusText();
      screen = STATUS_SETTINGS;
    }
  } else {
    if (x >= 91 && x <= 184 && y >= 91 && y <= 136) {
      screen = LAUNCHER;
    } else if (screen == NIGHT_SPINNER && x >= 95 && x <= 371 &&
               y >= 347 && y <= 410) {
      startSpin();
    } else if (screen == GRID_MAZE) {
      if (mazeWon || mazeCaught) return;
      if (x >= MAZE_X && x < MAZE_X + MAZE_SIZE * MAZE_CELL &&
          y >= MAZE_Y && y < MAZE_Y + MAZE_SIZE * MAZE_CELL) {
        int tapCol = (x - MAZE_X) / MAZE_CELL;
        int tapRow = (y - MAZE_Y) / MAZE_CELL;
        int dCol = tapCol > mazeCol ? 1 : tapCol < mazeCol ? -1 : 0;
        int dRow = tapRow > mazeRow ? 1 : tapRow < mazeRow ? -1 : 0;
        if (abs(tapCol - mazeCol) >= abs(tapRow - mazeRow)) {
          if (!dCol || !stepMaze(0, dCol)) {
            if (dRow) stepMaze(dRow, 0);
          }
        } else if (!dRow || !stepMaze(dRow, 0)) {
          if (dCol) stepMaze(0, dCol);
        }
      }
    } else if (screen == STATUS_SETTINGS && y >= 343 && y <= 394) {
      if (x >= VOLUME_MINUS_X && x < VOLUME_MINUS_X + VOLUME_BUTTON_SIZE) {
        setAudioVolume(audioVolume > 5 ? audioVolume - 5 : 0);
      } else if (x >= VOLUME_PLUS_X && x < VOLUME_PLUS_X + VOLUME_BUTTON_SIZE) {
        setAudioVolume(min(100, static_cast<int>(audioVolume) + 5));
      } else if (x >= VOLUME_TRACK_X && x <= VOLUME_TRACK_X + VOLUME_TRACK_WIDTH) {
        int raw = (x - VOLUME_TRACK_X) * 100 / VOLUME_TRACK_WIDTH;
        setAudioVolume(min(100, ((raw + 2) / 5) * 5));
      }
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
      "input[type=hidden]{display:none}"
      "textarea{height:19rem}button{font:inherit;padding:.7rem 1.2rem;"
      "background:#49dce9;border:0;border-radius:.4rem}form{margin:1rem 0}"
      "small{color:#abb4c5}li{margin:.3rem 0}</style>"
      "<h1>Chicago Night Spinner</h1><p>Gridley picks from ";
  page += usingOsmBars ? "OpenStreetMap bars near 60612." : "your saved list.";
  page += "</p><p><strong>" + String(barCount) + " bars loaded.</strong>";
  if (autoBars) page += " " + htmlEscape(barFetchStatus) + ".";
  page += "</p>";
  if (autoBars) {
    page += "<form method='post' action='/bars/refresh'><button>Refresh nearby bars now</button></form>";
    page += "<form method='post' action='/bars/source'><input type='hidden' name='mode' value='manual'>"
            "<button>Use my saved list</button></form>";
  } else {
    page += "<form method='post' action='/bars/source'><input type='hidden' name='mode' value='auto'>"
            "<button>Use nearby OpenStreetMap bars</button></form>";
  }
  if (networkMode != CONNECTED) {
    page += "<p><small>Connect Gridley to 2.4 GHz Wi-Fi for automatic updates. "
            "Your saved bars work without internet.</small></p>";
  }
  page += "<h2>On the spinner</h2><ul>";
  for (uint8_t i = 0; i < barCount; ++i) {
    page += "<li>" + htmlEscape(barOptions[i].name) + " — " +
            htmlEscape(barOptions[i].address) + "</li>";
  }
  page += "</ul><h2>My saved list</h2><p>Editing this list switches Gridley to your list. "
          "You can switch back to nearby bars above.</p>"
          "<form method='post' action='/bars'><label>Area label<input name='area' maxlength='20' value='";
  page += htmlEscape(manualAreaLabel);
  page += "' required></label><label>Bars, one per line as Name | Street address"
          "<textarea name='bars' maxlength='1800' required>";
  page += htmlEscape(settings.getString("bars", DEFAULT_BARS));
  page += "</textarea></label><button>Save my list</button></form>"
          "<p><small>Automatic search covers bars and pubs within about 2 miles of "
          "the center of 60612. It refreshes daily while Gridley is online; "
          "missing addresses may show as ‘Address not listed’. "
          "Data © <a href='https://www.openstreetmap.org/copyright'>OpenStreetMap contributors</a>."
          "</small></p></html>";
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
  manualAreaLabel = newArea;
  areaLabel = manualAreaLabel;
  autoBars = false;
  usingOsmBars = false;
  settings.putBool("autoBars", false);
  settings.putString("area", manualAreaLabel);
  settings.putString("bars", barListText());
  selectedBar = displayedBar = -1;
  spinning = false;
  screenDirty = true;
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html", "<h1>Saved to Gridley</h1><p><a href='/bars'>Edit again</a></p>");
}

void setBarSource() {
  String mode = web.arg("mode");
  if (mode == "auto") {
    autoBars = true;
    settings.putBool("autoBars", true);
    useCachedOsmBars();
    nextBarRefreshAt = millis();
    barFetchStatus = networkMode != CONNECTED ? "Waiting for Wi-Fi" :
        lastBarRequestAt && millis() - lastBarRequestAt < BAR_MANUAL_MIN_MS ?
        "Recently checked; try again shortly" : "Refresh starting";
  } else if (mode == "manual") {
    autoBars = false;
    settings.putBool("autoBars", false);
    useManualBars();
  } else {
    web.send(400, "text/plain", "Choose nearby bars or my saved list.");
    return;
  }
  web.sendHeader("Location", "/bars");
  web.send(303, "text/plain", "Opening bars");
}

void requestBarRefresh() {
  autoBars = true;
  settings.putBool("autoBars", true);
  if (!usingOsmBars) useCachedOsmBars();
  nextBarRefreshAt = millis();
  barFetchStatus = networkMode != CONNECTED ? "Waiting for Wi-Fi" :
      barFetchRunning ? "Refreshing from OpenStreetMap..." :
      lastBarRequestAt && millis() - lastBarRequestAt < BAR_MANUAL_MIN_MS ?
      "Recently checked; try again shortly" : "Refresh starting";
  web.sendHeader("Location", "/bars");
  web.send(303, "text/plain", "Opening bars");
}

void configureWeb() {
  web.on("/", HTTP_GET, []() {
    if (networkMode == SETUP) sendWifiSetupPage();
    else sendBarPage();
  });
  web.on("/save", HTTP_POST, saveWifi);
  web.on("/bars", HTTP_GET, sendBarPage);
  web.on("/bars", HTTP_POST, saveBars);
  web.on("/bars/source", HTTP_POST, setBarSource);
  web.on("/bars/refresh", HTTP_POST, requestBarRefresh);
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
  if (mdnsReady) MDNS.end();
  mdnsReady = false;
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
  Serial.print(" | speaker: ");
  Serial.print(audioReady ? "ready" : "unavailable");
  Serial.print(" | ");
  Serial.print(programFlashText());
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
  audioVolume = settings.getUChar("volume", 60);
  if (audioVolume > 100) audioVolume = 100;
  manualAreaLabel = settings.getString("area", "60612");
  autoBars = settings.getBool("autoBars", true);
  useManualBars();
  if (autoBars) useCachedOsmBars();
  barFetchStatus = usingOsmBars ? "Using saved OpenStreetMap bars" : "Waiting for Wi-Fi";
  barFetchQueue = xQueueCreate(1, sizeof(BarFetchResult));

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

  imuReady = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!imuReady) imuReady = imu.begin(Wire, QMI8658_H_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuReady) {
    imuReady = imu.setAccelFullScaleRange(AccelFullScaleRange::FS_2G) &&
               imu.setAccelOutputDataRate(56.0f) && imu.enableAccel();
    Serial.println(imuReady ? "Grid Maze tilt sensor ready" :
                   "WARNING: Grid Maze tilt sensor setup failed");
  } else Serial.println("WARNING: Grid Maze tilt sensor not found");

  audioReady = initAudio();

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
      mdnsReady = MDNS.begin("gridley");
      if (mdnsReady) MDNS.addService("http", "tcp", 80);
      configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
      if (!nextBarRefreshAt) nextBarRefreshAt = millis() + 3000;
      screenDirty = true;
    } else if (millis() - connectStarted > 20000) {
      WiFi.disconnect();
      startSetup();
    }
  } else if (networkMode == CONNECTED && WiFi.status() != WL_CONNECTED) {
    startConnecting();
  }

  pollBootButton();

  if (restartAt && static_cast<int32_t>(millis() - restartAt) >= 0) ESP.restart();
  pollTouch();
  updateSpin();
  processBarFetchResult();
  maybeRefreshBars();
  pumpAudio();
  if (screenDirty) {
    drawScreen();
    screenDirty = false;
  }
  updateMaze();
  if (millis() - lastLog > 5000) {
    logStatus();
    lastLog = millis();
  }
  delay(10);
}

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESP_I2S.h>
#include <math.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <ImuDrv.hpp>
#include "TouchDrvCST.hpp"
#include "es7210.h"
#include "es8311.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
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
enum AppScreen { LAUNCHER, GRID_MAZE, SOUND_ART, GRIDLEY_SAYS,
                 DISC_DEFENDER, STATUS_SETTINGS };
NetworkMode networkMode = CONNECTING;
AppScreen screen = LAUNCHER;
bool mdnsReady = false;

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
constexpr uint32_t MAZE_PULSE_COOLDOWN_MS = 7000;
constexpr uint32_t MAZE_PULSE_STUN_MS = 1800;
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
bool micReady = false;
bool micActive = false;
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
unsigned long mazePulseReadyAt = 0;
unsigned long mazePulseFlashUntil = 0;
unsigned long mazeEnemyStunnedUntil = 0;
unsigned long mazeLastStatusDrawAt = 0;
int mazeWinFrame = -1;
int enemyRow = -1;
int enemyCol = -1;
int enemyPrevRow = -1;
int enemyPrevCol = -1;
constexpr int SOUND_BARS = 32;
constexpr int SOUND_FRAMES = 256;
int16_t micSamples[SOUND_FRAMES * 2];
float soundBars[SOUND_BARS] = {};
float soundLevel = 0;
float soundNoiseFloor = 0;
float soundPulse = 0;
float soundDbFs = -90;
unsigned long lastSoundFrame = 0;
unsigned long lastSoundLog = 0;
uint32_t soundFrameCount = 0;
Arduino_Canvas *soundCanvas = nullptr;

enum GridMove : uint8_t {
  GRID_SHAKE, GRID_FLICK, GRID_TILT_LEFT, GRID_TILT_RIGHT,
  GRID_TAP, GRID_SWIPE, GRID_PRESS
};
enum GridPhase : uint8_t { GRID_READY, GRID_PROMPT, GRID_SUCCESS, GRID_OVER };
GridPhase gridPhase = GRID_READY;
GridMove gridMove = GRID_TAP;
GridMove gridPreviousMove = GRID_TAP;
bool gyroReady = false;
uint16_t gridScore = 0;
uint16_t gridBest = 0;
uint32_t gridPromptAt = 0;
uint32_t gridTimeLimit = 3500;
uint32_t gridFeedbackAt = 0;
uint32_t gridOverAt = 0;
constexpr uint32_t GRID_LOSS_FRAME_MS = 120;
constexpr uint32_t GRID_LOSS_ANIM_MS = 1440;
int gridLossFrame = -1;
Arduino_Canvas *gridLossCanvas = nullptr;
bool gridLossCanvasTried = false;
uint32_t gridLastMotionPoll = 0;
uint32_t gridTiltStartedAt = 0;
uint32_t gridTouchArmedAt = 0;
uint32_t gridLastTouchPoll = 0;
uint32_t gridLastTouchSeen = 0;
uint32_t gridTouchStartAt = 0;
float gridBaseY = 0;
float gridLastAccelX = 0;
float gridLastAccelY = 0;
float gridLastAccelZ = 0;
bool gridLastAccelValid = false;
bool gridTouchTracking = false;
int gridTouchStartX = 0;
int gridTouchStartY = 0;
constexpr int GRID_TIMER_SEGMENTS = 16;
int gridLastTimerSegments = -1;

enum DiscPhase : uint8_t { DISC_READY, DISC_PLAYING, DISC_OVER };
DiscPhase discPhase = DISC_READY;
uint16_t discScore = 0;
uint16_t discBest = 0;
uint16_t discStreak = 0;
uint8_t discLives = 3;
uint16_t discShotNumber = 0;
float discShieldAngle = -PI / 2;
float discShotAngle = -PI / 2;
float discShotRadius = 120;
bool discShotActive = false;
uint32_t discLastUpdateAt = 0;
uint32_t discLastFrameAt = 0;
uint32_t discNextShotAt = 0;
uint32_t discFeedbackUntil = 0;
uint8_t discFeedback = 0;
uint32_t discBoostUntil = 0;
uint32_t discLastTouchPoll = 0;
uint32_t discLastTouchSeen = 0;
bool discTouchTracking = false;
Arduino_Canvas *discCanvas = nullptr;
bool discCanvasTried = false;
constexpr float DISC_TWIST_GAIN = 3.0f;
uint32_t gameTonePhase = 0;
uint32_t gameToneStep = 0;
uint32_t gameToneFramesLeft = 0;
uint8_t mazeSoundKind = 0;
uint8_t mazeSoundNote = 0;

void playMazeSound(bool won);

constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint32_t BOOT_DEBOUNCE_MS = 35;
constexpr uint32_t BOOT_DOUBLE_TAP_MS = 450;
constexpr uint32_t BOOT_LONG_PRESS_MS = 3000;

void IRAM_ATTR onTouchInterrupt() { touchPending = true; }

String makeSetupPassword() {
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String result;
  for (int i = 0; i < 12; ++i) {
    result += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  return result;
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
  button(84, 146, 298, 48, "GRID MAZE", MAZE_GOLD);
  button(84, 201, 298, 48, "SOUND GRID", BLUE);
  button(84, 256, 298, 48, "GRIDLEY SAYS", PURPLE);
  button(84, 311, 298, 48, "DISC DEFENDER", CYAN);
  // Settings is a small utility control below the programs. Its solid
  // backing clears the perspective lines so the label stays legible.
  gfx->fillRoundRect(145, 384, 176, 46, 20, BG);
  gfx->drawRoundRect(145, 384, 176, 46, 20, PURPLE);
  // Bright gear teeth and label remain visible over the lower grid.
  for (int tooth = 0; tooth < 8; ++tooth) {
    float angle = tooth * (2.0f * PI / 8);
    int innerX = 178 + static_cast<int>(cosf(angle) * 9);
    int innerY = 407 + static_cast<int>(sinf(angle) * 9);
    int outerX = 178 + static_cast<int>(cosf(angle) * 14);
    int outerY = 407 + static_cast<int>(sinf(angle) * 14);
    gfx->drawLine(innerX, innerY, outerX, outerY, WHITE);
    gfx->drawLine(innerX + 1, innerY, outerX + 1, outerY, WHITE);
  }
  gfx->fillCircle(178, 407, 9, WHITE);
  gfx->fillCircle(178, 407, 5, BG);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(203, 400);
  gfx->print("SETTINGS");
}

void drawSoundArt() {
  chrome("SOUND GRID");
  if (!micActive) textCentered("MIC UNAVAILABLE", 145, 1, MAZE_ENEMY);
  lastSoundFrame = 0;
}

void drawSoundArtFrame() {
  Arduino_GFX *frameGfx = soundCanvas ?
      static_cast<Arduino_GFX *>(soundCanvas) : static_cast<Arduino_GFX *>(gfx);
  // Render the moving art offscreen, then transfer its rectangular region in
  // one display write. The title remains outside that region.
  const int offsetX = soundCanvas ? 49 : 0;
  const int offsetY = soundCanvas ? 87 : 0;
  frameGfx->fillScreen(BG);
  if (!soundCanvas) {
    frameGfx->setTextColor(CYAN);
    frameGfx->setTextSize(2);
    frameGfx->setCursor((LCD_WIDTH - 10 * 12) / 2, 65);
    frameGfx->print("SOUND GRID");
  }
  frameGfx->drawCircle(233 - offsetX, 233 - offsetY, 224, BLUE);
  frameGfx->drawCircle(233 - offsetX, 233 - offsetY, 218, PANEL);
  const int centerX = 233 - offsetX;
  const int centerY = 263 - offsetY;
  frameGfx->drawCircle(centerX, centerY, 80, MAZE_GRID);
  frameGfx->drawCircle(centerX, centerY, 115, GRID_DIM);
  frameGfx->drawCircle(centerX, centerY, 150, BLUE);
  frameGfx->drawCircle(centerX, centerY, 175, GRID_DIM);
  uint16_t pulseColor = soundPulse > 0.55f ? WHITE :
                        soundPulse > 0.18f ? PURPLE : BLUE;
  float rotation = soundFrameCount * 0.018f;
  for (int i = 0; i < SOUND_BARS; ++i) {
    float angle = rotation + i * (2.0f * PI / SOUND_BARS);
    float dx = cosf(angle);
    float dy = sinf(angle);
    float reach = 115 + 46 * soundBars[i] + 11 * soundPulse;
    int x1 = centerX + static_cast<int>(dx * 82);
    int y1 = centerY + static_cast<int>(dy * 82);
    int x2 = centerX + static_cast<int>(dx * reach);
    int y2 = centerY + static_cast<int>(dy * reach);
    uint16_t color = soundBars[i] > 0.55f ? WHITE :
                     i % 4 == 0 ? CYAN : i % 2 == 0 ? PURPLE : BLUE;
    frameGfx->drawLine(x1, y1, x2, y2, color);
    frameGfx->drawLine(x1 + 1, y1, x2 + 1, y2, color);
    frameGfx->fillCircle(x2, y2, soundBars[i] > 0.55f ? 4 : 3, color);
  }
  int outerRadius = 101 + static_cast<int>(soundLevel * 39 + soundPulse * 12);
  frameGfx->drawCircle(centerX, centerY, outerRadius, pulseColor);
  frameGfx->drawCircle(centerX, centerY, outerRadius - 2, BLUE);
  int coreRadius = 46 + static_cast<int>(soundLevel * 20 + soundPulse * 5);
  frameGfx->fillCircle(centerX, centerY, coreRadius, PANEL);
  frameGfx->drawCircle(centerX, centerY, coreRadius, CYAN);
  frameGfx->drawCircle(centerX, centerY, coreRadius - 4, BLUE);
  char levelText[8];
  snprintf(levelText, sizeof(levelText), "%d",
           static_cast<int>(roundf(soundDbFs)));
  frameGfx->setTextColor(WHITE);
  frameGfx->setTextSize(3);
  frameGfx->setCursor((LCD_WIDTH - static_cast<int>(strlen(levelText)) * 18) / 2
                      - offsetX, 240 - offsetY);
  frameGfx->print(levelText);
  frameGfx->setTextColor(CYAN);
  frameGfx->setTextSize(2);
  frameGfx->setCursor(209 - offsetX, 278 - offsetY);
  frameGfx->print("dBFS");
  frameGfx->fillRoundRect(91 - offsetX, 91 - offsetY, 93, 45, 18, PANEL);
  frameGfx->drawRoundRect(91 - offsetX, 91 - offsetY, 93, 45, 18, BLUE);
  frameGfx->setTextColor(BLUE);
  frameGfx->setTextSize(2);
  frameGfx->setCursor(101 - offsetX, 105 - offsetY);
  frameGfx->print("< BACK");
  frameGfx->fillRoundRect(202 - offsetX, 96 - offsetY, 62, 19, 8, BG);
  frameGfx->setTextColor(CYAN);
  frameGfx->setTextSize(1);
  frameGfx->setCursor(209 - offsetX, 101 - offsetY);
  frameGfx->print("MIC LIVE");
  if (soundCanvas) soundCanvas->flush();
  ++soundFrameCount;
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
  if (millis() < mazePulseFlashUntil) gfx->drawCircle(x, y, 13, MAZE_GOLD);
  gfx->fillCircle(x, y, 10, CYAN);
  gfx->fillCircle(x, y, 6, WHITE);
}

void drawMazeEnemy() {
  if (enemyRow < 0) return;
  int x = MAZE_X + enemyCol * MAZE_CELL + MAZE_CELL / 2;
  int y = MAZE_Y + enemyRow * MAZE_CELL + MAZE_CELL / 2;
  uint16_t color = millis() < mazeEnemyStunnedUntil ? BLUE : MAZE_ENEMY;
  gfx->fillRect(x - 9, y - 9, 18, 18, color);
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
  unsigned long now = millis();
  if (now < mazePulseFlashUntil)
    textCentered("EMP! SENTINEL REPELLED", 412, 1, MAZE_GOLD);
  else if (now < mazePulseReadyAt)
    textCentered("EMP RECHARGING " + String((mazePulseReadyAt - now + 999) / 1000) + "s",
                 412, 1, MUTED);
  else textCentered("TILT/TAP MOVE  /  BUTTON: EMP", 412, 1, MAZE_GOLD);
  mazeLastStatusDrawAt = now;
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
  mazePulseReadyAt = 0;
  mazePulseFlashUntil = 0;
  mazeEnemyStunnedUntil = 0;
  mazeLastStatusDrawAt = 0;
  screenDirty = true;
}

void catchMazePlayer() {
  mazeCaught = true;
  mazeCaughtAt = millis();
  Serial.println("Grid Maze: sentinel caught player");
  playMazeSound(false);
  drawMazeCaught();
}

void fireMazePulse() {
  unsigned long now = millis();
  if (screen != GRID_MAZE || mazeWon || mazeCaught || now < mazePulseReadyAt ||
      enemyRow < 0) return;

  // The EMP relocates the sentinel to a distant reachable tile, even when
  // a narrow corridor leaves no physical space to push it back one square.
  int8_t distance[MAZE_SIZE][MAZE_SIZE];
  for (int row = 0; row < MAZE_SIZE; ++row)
    for (int col = 0; col < MAZE_SIZE; ++col) distance[row][col] = -1;
  uint8_t queueRow[MAZE_SIZE * MAZE_SIZE];
  uint8_t queueCol[MAZE_SIZE * MAZE_SIZE];
  int head = 0, tail = 0;
  queueRow[tail] = mazeRow;
  queueCol[tail++] = mazeCol;
  distance[mazeRow][mazeCol] = 0;
  const int8_t dRow[] = {-1, 1, 0, 0};
  const int8_t dCol[] = {0, 0, -1, 1};
  int bestDistance = -1;
  int bestRow = -1, bestCol = -1;
  while (head < tail) {
    int row = queueRow[head];
    int col = queueCol[head++];
    if (distance[row][col] > bestDistance &&
        mazeCells[row][col] != 'G' &&
        (row != enemyRow || col != enemyCol) &&
        (row != mazeRow || col != mazeCol)) {
      bestDistance = distance[row][col];
      bestRow = row;
      bestCol = col;
    }
    for (int dir = 0; dir < 4; ++dir) {
      int nextRow = row + dRow[dir];
      int nextCol = col + dCol[dir];
      if (mazeWall(nextRow, nextCol) || distance[nextRow][nextCol] >= 0) continue;
      distance[nextRow][nextCol] = distance[row][col] + 1;
      queueRow[tail] = nextRow;
      queueCol[tail++] = nextCol;
    }
  }
  if (bestRow < 0) return;
  enemyRow = bestRow;
  enemyCol = bestCol;
  enemyPrevRow = enemyPrevCol = -1;
  lastEnemyStep = now;
  mazePulseReadyAt = now + MAZE_PULSE_COOLDOWN_MS;
  mazePulseFlashUntil = now + 750;
  mazeEnemyStunnedUntil = now + MAZE_PULSE_STUN_MS;
  playDiscTone(720, 130);
  Serial.printf("Grid Maze EMP: sentinel repelled to %d,%d\n", enemyRow, enemyCol);
  screenDirty = true;
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
    playMazeSound(true);
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
  if (enemyRow < 0 || millis() < mazeEnemyStunnedUntil ||
      millis() - lastEnemyStep < MAZE_ENEMY_STEP_MS) return;
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
  unsigned long now = millis();
  if (mazePulseFlashUntil && now >= mazePulseFlashUntil) {
    mazePulseFlashUntil = 0;
    screenDirty = true;
  }
  if (now - mazeLastStatusDrawAt >= 250) drawMazeStatus();
  updateMazeEnemy();
  if (mazeCaught) return;
  if (!imuReady) return;
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

bool startSoundArt() {
  if (!audioReady) return false;
  if (!micReady) {
    // The ES7210 microphones share the speaker's clocks and I2S receive pin.
    Wire.beginTransmission(ES7210_ADDR);
    if (Wire.endTransmission() != 0) {
      Serial.println("WARNING: ES7210 microphone ADC not found");
      return false;
    }
    audio_hal_codec_config_t config = {};
    config.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
    config.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
    config.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
    config.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
    config.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
    config.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
    const es7210_input_mics_t onboardMics = static_cast<es7210_input_mics_t>(
        ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2);
    if (es7210_adc_init(&Wire, &config) != ESP_OK ||
        es7210_adc_config_i2s(config.codec_mode, &config.i2s_iface) != ESP_OK ||
        es7210_mic_select(onboardMics) != ESP_OK ||
        es7210_adc_set_gain(onboardMics, GAIN_37_5DB) != ESP_OK) {
      Serial.println("WARNING: ES7210 microphone setup failed");
      return false;
    }
    micReady = true;
    Serial.println("Microphones ready");
  }
  if (es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE,
                            AUDIO_HAL_CTRL_START) != ESP_OK) {
    Serial.println("WARNING: ES7210 microphones would not start");
    return false;
  }
  audioI2S.setTimeout(40);
  if (!soundCanvas && displayReady) {
    soundCanvas = new Arduino_Canvas(368, 356, gfx, 49, 87);
    if (!soundCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete soundCanvas;
      soundCanvas = nullptr;
      Serial.println("WARNING: Sound Grid framebuffer unavailable");
    }
  }
  micActive = true;
  soundNoiseFloor = 0;
  soundLevel = soundPulse = 0;
  soundDbFs = -90;
  memset(soundBars, 0, sizeof(soundBars));
  soundFrameCount = 0;
  lastSoundLog = 0;
  Serial.println("Sound Grid listening");
  return true;
}

void stopSoundArt() {
  if (!micActive) return;
  micActive = false;
  es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
  audioI2S.setTimeout(1000);
  Serial.println("Sound Grid stopped");
}

void updateSoundArt() {
  if (screen != SOUND_ART || !micActive || !displayReady) return;
  unsigned long now = millis();
  if (lastSoundFrame && now - lastSoundFrame < 40) return;
  lastSoundFrame = now;
  size_t bytes = audioI2S.readBytes(reinterpret_cast<char *>(micSamples),
                                    sizeof(micSamples));
  if (bytes != sizeof(micSamples)) {
    if (now - lastSoundLog > 1000) {
      Serial.printf("Sound Grid microphone read: %u / %u bytes\n",
                    static_cast<unsigned>(bytes),
                    static_cast<unsigned>(sizeof(micSamples)));
      lastSoundLog = now;
    }
    return;
  }

  uint64_t energyLeft = 0, energyRight = 0;
  for (int i = 0; i < SOUND_FRAMES; ++i) {
    int32_t left = micSamples[i * 2];
    int32_t right = micSamples[i * 2 + 1];
    energyLeft += left * left;
    energyRight += right * right;
  }
  bool useRight = energyRight > energyLeft;
  float rms = sqrtf(static_cast<float>(useRight ? energyRight : energyLeft) /
                    SOUND_FRAMES);
  soundDbFs = max(-90.0f, 20.0f * log10f(max(rms, 1.0f) / 32768.0f));
  if (soundNoiseFloor == 0) soundNoiseFloor = rms;
  else if (rms < soundNoiseFloor)
    soundNoiseFloor = soundNoiseFloor * 0.94f + rms * 0.06f;
  else
    soundNoiseFloor = soundNoiseFloor * 0.997f + rms * 0.003f;
  float relative = (rms - soundNoiseFloor * 1.04f) /
                   (soundNoiseFloor * 0.55f + 18.0f);
  float absolute = (soundDbFs + 60.0f) / 21.0f;
  float target = constrain(max(relative, absolute), 0.0f, 1.0f);
  if (target > 0.20f && target > soundLevel * 1.35f) soundPulse = 1.0f;
  else soundPulse *= 0.84f;
  soundLevel = target > soundLevel ? soundLevel * 0.45f + target * 0.55f :
                                    soundLevel * 0.83f + target * 0.17f;
  for (int bar = 0; bar < SOUND_BARS; ++bar) {
    int peak = 0;
    for (int sample = 0; sample < SOUND_FRAMES / SOUND_BARS; ++sample) {
      int index = bar * (SOUND_FRAMES / SOUND_BARS) + sample;
      int value = abs(static_cast<int>(micSamples[index * 2 + (useRight ? 1 : 0)]));
      if (value > peak) peak = value;
    }
    float height = constrain((peak - soundNoiseFloor * 1.5f) /
                                 (soundNoiseFloor * 2.1f + 55.0f), 0.0f, 1.0f);
    height = height * (0.25f + target * 0.85f);
    height = max(height, target * 0.45f);
    soundBars[bar] = height > soundBars[bar] ?
        soundBars[bar] * 0.35f + height * 0.65f :
        soundBars[bar] * 0.70f + height * 0.30f;
  }
  drawSoundArtFrame();
  if (now - lastSoundLog > 1000) {
    Serial.printf("Sound Grid: rms %.0f, floor %.0f, %.0f dBFS, level %.2f, channel %c\n",
                  rms, soundNoiseFloor, soundDbFs, soundLevel,
                  useRight ? 'R' : 'L');
    lastSoundLog = now;
  }
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
  gameToneFramesLeft = 0;
  mazeSoundKind = 0;
  audioPowerDownAt = 0;
  audioPlaying = true;
  digitalWrite(AUDIO_PA, HIGH);
  Serial.println("BOOT double tap: playing sound");
#else
  Serial.println("BOOT double tap: run tools/prepare_audio.py to add a clip");
#endif
}

void startGameTone(uint16_t frequency, uint16_t durationMs) {
  if (!audioReady || !audioVolume || micActive || audioPlaying) return;
  gameTonePhase = 0;
  gameToneStep = (static_cast<uint64_t>(frequency) << 32) / AUDIO_SAMPLE_RATE;
  gameToneFramesLeft = AUDIO_SAMPLE_RATE * durationMs / 1000;
  audioPowerDownAt = 0;
  digitalWrite(AUDIO_PA, HIGH);
}

void playDiscTone(uint16_t frequency, uint16_t durationMs) {
  mazeSoundKind = 0;
  startGameTone(frequency, durationMs);
}

void advanceMazeSound() {
  const uint16_t winNotes[] = {523, 659, 784};
  const uint16_t loseNotes[] = {350, 260, 160};
  const uint16_t winLengths[] = {90, 90, 210};
  const uint16_t loseLengths[] = {120, 130, 240};
  if (mazeSoundNote >= 3) {
    mazeSoundKind = 0;
    audioPowerDownAt = millis() + 100;
    return;
  }
  int note = mazeSoundNote++;
  startGameTone(mazeSoundKind == 1 ? winNotes[note] : loseNotes[note],
                mazeSoundKind == 1 ? winLengths[note] : loseLengths[note]);
}

void playMazeSound(bool won) {
  if (!audioReady || !audioVolume || micActive || audioPlaying) return;
  mazeSoundKind = won ? 1 : 2;
  mazeSoundNote = 0;
  advanceMazeSound();
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
#endif
  if (!audioPlaying && gameToneFramesLeft) {
    int16_t samples[256 * 2];
    uint32_t frames = min(gameToneFramesLeft, uint32_t(256));
    for (uint32_t i = 0; i < frames; ++i) {
      int16_t level = min(gameToneFramesLeft - i, uint32_t(64)) * 70;
      int16_t sample = gameTonePhase & 0x80000000 ? level : -level;
      samples[i * 2] = sample;
      samples[i * 2 + 1] = sample;
      gameTonePhase += gameToneStep;
    }
    size_t written = audioI2S.write(reinterpret_cast<uint8_t *>(samples),
                                    frames * 4);
    gameToneFramesLeft -= min(frames, static_cast<uint32_t>(written / 4));
    if (!gameToneFramesLeft) audioPowerDownAt = millis() + 100;
  }
  if (!audioPlaying && !gameToneFramesLeft && mazeSoundKind)
    advanceMazeSound();
  if (audioPowerDownAt &&
      static_cast<int32_t>(millis() - audioPowerDownAt) >= 0) {
    digitalWrite(AUDIO_PA, LOW);
    audioPowerDownAt = 0;
  }
}

const char *gridMoveLabel(GridMove move) {
  switch (move) {
    case GRID_SHAKE: return "SHAKE IT";
    case GRID_FLICK: return "FLICK IT";
    case GRID_TILT_LEFT: return "TILT LEFT";
    case GRID_TILT_RIGHT: return "TILT RIGHT";
    case GRID_TAP: return "TAP IT";
    case GRID_SWIPE: return "SWIPE IT";
    case GRID_PRESS: return "PRESS BUTTON";
  }
  return "MOVE IT";
}

const char *gridMoveHint(GridMove move) {
  switch (move) {
    case GRID_SHAKE: return "QUICK BACK-AND-FORTH";
    case GRID_FLICK: return "TWIST THE PUCK QUICKLY";
    case GRID_TILT_LEFT: return "LEAN LEFT AND HOLD";
    case GRID_TILT_RIGHT: return "LEAN RIGHT AND HOLD";
    case GRID_TAP: return "TOUCH THE SCREEN";
    case GRID_SWIPE: return "DRAG ACROSS THE SCREEN";
    case GRID_PRESS: return "ONE SHORT BUTTON PRESS";
  }
  return "FOLLOW THE GRID";
}

uint16_t gridMoveColor(GridMove move) {
  if (move == GRID_PRESS || move == GRID_FLICK) return PURPLE;
  if (move == GRID_TILT_LEFT || move == GRID_TILT_RIGHT) return MAZE_GOLD;
  return CYAN;
}

int gridTimerSegmentsRemaining(uint32_t now) {
  uint32_t elapsed = now - gridPromptAt;
  if (elapsed >= gridTimeLimit) return 0;
  return (GRID_TIMER_SEGMENTS * (gridTimeLimit - elapsed) + gridTimeLimit - 1) /
         gridTimeLimit;
}

void drawGridleyLossFrame(int frame) {
  if (!gridLossCanvasTried && displayReady) {
    gridLossCanvasTried = true;
    gridLossCanvas = new Arduino_Canvas(268, 178, gfx, 99, 172);
    if (!gridLossCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete gridLossCanvas;
      gridLossCanvas = nullptr;
      Serial.println("WARNING: Gridley Says animation framebuffer unavailable");
    }
  }
  Arduino_GFX *panel = gridLossCanvas ?
      static_cast<Arduino_GFX *>(gridLossCanvas) : static_cast<Arduino_GFX *>(gfx);
  int ox = gridLossCanvas ? 0 : 99;
  int oy = gridLossCanvas ? 0 : 172;
  if (gridLossCanvas) panel->fillScreen(BG);
  else panel->fillRect(ox, oy, 268, 178, BG);
  panel->fillRoundRect(ox, oy, 268, 178, 16, PANEL);
  panel->drawRoundRect(ox, oy, 268, 178, 16, MAZE_ENEMY);

  // Falling pixels and a sagging face give the loss screen a small, sad beat.
  for (int i = 0; i < 8; ++i) {
    int x = 23 + (i * 59) % 222;
    int y = 44 + (frame * 9 + i * 23) % 86;
    int size = 4 + (i % 3) * 2;
    panel->fillRect(ox + x, oy + y, size, size,
                    i % 3 == 0 ? CYAN : i % 3 == 1 ? PURPLE : GRID_DIM);
  }
  int droop = min(frame, 7) * 2;
  panel->fillRect(ox + 98, oy + 45 + droop, 72, 82, BG);
  panel->drawRect(ox + 98, oy + 45 + droop, 72, 82, PURPLE);
  panel->fillRect(ox + 115, oy + 67 + droop, 9, 7, CYAN);
  panel->fillRect(ox + 144, oy + 67 + droop, 9, 7, CYAN);
  int tear = (frame * 7) % 33;
  panel->fillRect(ox + 119, oy + 80 + droop + tear, 5, 8, BLUE);
  panel->fillRect(ox + 148, oy + 85 + droop + (tear + 15) % 33, 5, 8, BLUE);
  panel->fillRect(ox + 116, oy + 107 + droop, 7, 5, MAZE_ENEMY);
  panel->fillRect(ox + 123, oy + 102 + droop, 7, 5, MAZE_ENEMY);
  panel->fillRect(ox + 130, oy + 99 + droop, 9, 5, MAZE_ENEMY);
  panel->fillRect(ox + 139, oy + 102 + droop, 7, 5, MAZE_ENEMY);
  panel->fillRect(ox + 146, oy + 107 + droop, 7, 5, MAZE_ENEMY);

  panel->setTextSize(3);
  panel->setTextColor(WHITE);
  panel->setCursor(ox + 53, oy + 10);
  panel->print("OH, PUCK.");
  panel->setTextSize(2);
  panel->setTextColor(MUTED);
  panel->setCursor(ox + 68, oy + 149);
  panel->print("END OF LINE");
  if (gridLossCanvas) gridLossCanvas->flush();
  gridLossFrame = frame;
}

void drawGridleySays() {
  chrome("GRIDLEY SAYS");
  textCentered("SCORE " + String(gridScore) + "  /  BEST " + String(gridBest),
               148, 2, MUTED);
  if (gridPhase == GRID_READY) {
    textCentered("FOLLOW THE COMMAND", 205, 2, WHITE);
    textCentered("SHAKE / FLICK / TILT", 251, 1, CYAN);
    textCentered("TAP / SWIPE / BUTTON", 269, 1, CYAN);
    button(101, 352, 264, 55, "START", PURPLE);
  } else if (gridPhase == GRID_PROMPT) {
    uint16_t color = gridMoveColor(gridMove);
    gfx->drawCircle(233, 243, 112, GRID_DIM);
    gfx->drawCircle(233, 243, 105, color);
    textCentered(gridMoveLabel(gridMove), 220, 3, color);
    textCentered(gridMoveHint(gridMove), 275, 1, WHITE);
    gfx->drawRoundRect(98, 365, 270, 20, 8, BLUE);
    int remaining = gridTimerSegmentsRemaining(millis());
    for (int segment = 0; segment < GRID_TIMER_SEGMENTS; ++segment) {
      gfx->fillRect(106 + segment * 16, 369, 14, 12,
                    segment < remaining ? color : PANEL);
    }
    gridLastTimerSegments = remaining;
  } else if (gridPhase == GRID_SUCCESS) {
    textCentered("NICE MOVE", 215, 3, CYAN);
    textCentered("NEXT COMMAND...", 270, 2, WHITE);
  } else {
    uint32_t elapsed = millis() - gridOverAt;
    if (elapsed < GRID_LOSS_ANIM_MS) {
      drawGridleyLossFrame(elapsed / GRID_LOSS_FRAME_MS);
    } else {
      textCentered("GRID OVER", 205, 3, MAZE_ENEMY);
      textCentered("SCORE " + String(gridScore), 263, 2, WHITE);
      button(101, 352, 264, 55, "TRY AGAIN", PURPLE);
    }
  }
  textCentered("< BACK TO THE GRID", 423, 1, MUTED);
}

void nextGridleyCommand() {
  GridMove choices[7];
  int count = 0;
  if (imuReady) {
    choices[count++] = GRID_SHAKE;
    choices[count++] = GRID_TILT_LEFT;
    choices[count++] = GRID_TILT_RIGHT;
  }
  if (gyroReady) choices[count++] = GRID_FLICK;
  if (touchReady) {
    choices[count++] = GRID_TAP;
    choices[count++] = GRID_SWIPE;
  }
  choices[count++] = GRID_PRESS;
  int chosen = esp_random() % count;
  if (count > 1 && choices[chosen] == gridPreviousMove) {
    chosen = (chosen + 1 + esp_random() % (count - 1)) % count;
  }
  gridMove = choices[chosen];
  gridPreviousMove = gridMove;
  gridPhase = GRID_PROMPT;
  gridPromptAt = millis();
  gridTimeLimit = max(1700, 3500 - static_cast<int>(gridScore) * 100);
  gridTouchArmedAt = gridPromptAt + 260;
  gridLastMotionPoll = 0;
  gridTiltStartedAt = 0;
  gridLastAccelValid = false;
  gridLastTimerSegments = -1;
  if (imuReady) {
    AccelerometerData accel;
    if (imu.readAccel(accel)) {
      gridBaseY = accel.mps2.y / 9.80665f;
      gridLastAccelX = accel.mps2.x / 9.80665f;
      gridLastAccelY = gridBaseY;
      gridLastAccelZ = accel.mps2.z / 9.80665f;
      gridLastAccelValid = true;
    }
  }
  Serial.printf("Gridley Says: %s, score %u\n", gridMoveLabel(gridMove),
                gridScore);
  screenDirty = true;
}

void startGridleySays() {
  gridScore = 0;
  gridPreviousMove = GRID_PRESS;
  nextGridleyCommand();
  // The START touch must be released before it can count as a game action.
  gridTouchArmedAt = millis() + 450;
}

void finishGridleySays() {
  gridPhase = GRID_OVER;
  gridOverAt = millis();
  gridLossFrame = -1;
  if (gridScore > gridBest) {
    gridBest = gridScore;
    settings.putUShort("gridBest", gridBest);
  }
  Serial.printf("Gridley Says over: %u, best %u\n", gridScore, gridBest);
  screenDirty = true;
}

void gridleyInput(GridMove input) {
  if (gridPhase != GRID_PROMPT || millis() < gridTouchArmedAt) return;
  if (input != gridMove) {
    finishGridleySays();
    return;
  }
  ++gridScore;
  gridPhase = GRID_SUCCESS;
  gridFeedbackAt = millis();
  Serial.printf("Gridley Says good: %u\n", gridScore);
  screenDirty = true;
}

void updateGridleySays() {
  if (screen != GRIDLEY_SAYS || !displayReady) return;
  uint32_t now = millis();
  if (gridPhase == GRID_OVER) {
    uint32_t elapsed = now - gridOverAt;
    if (elapsed >= GRID_LOSS_ANIM_MS) {
      if (gridLossFrame != 100) {
        gridLossFrame = 100;
        screenDirty = true;
      }
    } else {
      int frame = elapsed / GRID_LOSS_FRAME_MS;
      if (frame != gridLossFrame && !screenDirty) drawGridleyLossFrame(frame);
    }
    return;
  }
  if (gridPhase == GRID_SUCCESS) {
    if (now - gridFeedbackAt >= 480) nextGridleyCommand();
    return;
  }
  if (gridPhase != GRID_PROMPT) return;
  if (now - gridPromptAt >= gridTimeLimit) {
    finishGridleySays();
    return;
  }
  int remaining = gridTimerSegmentsRemaining(now);
  if (gridLastTimerSegments >= 0 && remaining < gridLastTimerSegments &&
      !screenDirty) {
    for (int segment = remaining; segment < gridLastTimerSegments; ++segment) {
      gfx->fillRect(106 + segment * 16, 369, 14, 12, PANEL);
    }
    gridLastTimerSegments = remaining;
  }
  if (!imuReady || now - gridLastMotionPoll < 25) return;
  gridLastMotionPoll = now;
  AccelerometerData accel;
  if (!imu.readAccel(accel)) return;
  float ax = accel.mps2.x / 9.80665f;
  float ay = accel.mps2.y / 9.80665f;
  float az = accel.mps2.z / 9.80665f;
  if (!gridLastAccelValid) {
    gridBaseY = ay;
    gridLastAccelValid = true;
  }
  float dx = ax - gridLastAccelX;
  float dy = ay - gridLastAccelY;
  float dz = az - gridLastAccelZ;
  gridLastAccelX = ax;
  gridLastAccelY = ay;
  gridLastAccelZ = az;
  if (now - gridPromptAt < 260) return;
  if (gridMove == GRID_SHAKE && dx * dx + dy * dy + dz * dz > 0.65f * 0.65f) {
    gridleyInput(GRID_SHAKE);
  } else if (gridMove == GRID_TILT_LEFT || gridMove == GRID_TILT_RIGHT) {
    float lean = ay - gridBaseY;
    bool inDirection = gridMove == GRID_TILT_LEFT ? lean < -0.38f :
                                                   lean > 0.38f;
    if (!inDirection) gridTiltStartedAt = 0;
    else if (!gridTiltStartedAt) gridTiltStartedAt = now;
    else if (now - gridTiltStartedAt >= 180) gridleyInput(gridMove);
  } else if (gridMove == GRID_FLICK && gyroReady) {
    // A flick is a quick twist around the screen's normal. A sideways lean
    // mainly turns the other axes and should not complete this command.
    GyroscopeData gyro;
    if (imu.readGyro(gyro)) {
      float twist = fabsf(gyro.dps.z);
      float leanRate = max(fabsf(gyro.dps.x), fabsf(gyro.dps.y));
      if (twist > 200 && twist > leanRate * 1.25f)
        gridleyInput(GRID_FLICK);
    }
  }
}

void pollGridleyTouch(bool pending) {
  uint32_t now = millis();
  if (!pending && (!gridTouchTracking || now - gridLastTouchPoll < 25)) return;
  gridLastTouchPoll = now;
  int16_t x[1] = {}, y[1] = {};
  if (!touch.getPoint(x, y, 1)) {
    if (gridTouchTracking && now - gridLastTouchSeen > 120)
      gridTouchTracking = false;
    return;
  }
  if (gridTouchTracking && now - gridLastTouchSeen > 180)
    gridTouchTracking = false;
  gridLastTouchSeen = now;
  if (!gridTouchTracking) {
    gridTouchTracking = true;
    gridTouchStartX = x[0];
    gridTouchStartY = y[0];
    gridTouchStartAt = now;
    if (x[0] >= 91 && x[0] <= 184 && y[0] >= 91 && y[0] <= 136) {
      gridTouchTracking = false;
      handleTap(x[0], y[0]);
      return;
    }
    if (gridPhase == GRID_READY || gridPhase == GRID_OVER) {
      if (y[0] >= 345 && y[0] <= 420) startGridleySays();
    } else if (gridPhase == GRID_PROMPT && now >= gridTouchArmedAt) {
      if (gridMove == GRID_TAP) gridleyInput(GRID_TAP);
      else if (gridMove != GRID_SWIPE) finishGridleySays();
    }
  } else if (gridPhase == GRID_PROMPT && gridMove == GRID_SWIPE &&
             now >= gridTouchArmedAt && now - gridTouchStartAt < 900 &&
             (abs(x[0] - gridTouchStartX) > 70 ||
              abs(y[0] - gridTouchStartY) > 70)) {
    gridleyInput(GRID_SWIPE);
  }
}

float discAngleDistance(float a, float b) {
  return fabsf(atan2f(sinf(a - b), cosf(a - b)));
}

void spawnDiscShot() {
  discShotAngle = discShotNumber++ == 0 ? -PI / 2 :
      (esp_random() % 6283) / 1000.0f - PI;
  discShotRadius = 112;
  discShotActive = true;
}

void startDiscDefender() {
  discPhase = DISC_PLAYING;
  discScore = 0;
  discStreak = 0;
  discLives = 3;
  discShotNumber = 0;
  discShieldAngle = -PI / 2;
  discFeedback = 0;
  discFeedbackUntil = 0;
  discBoostUntil = 0;
  discTouchTracking = false;
  discLastUpdateAt = millis();
  discLastFrameAt = 0;
  spawnDiscShot();
  screenDirty = true;
}

void finishDiscDefender() {
  discPhase = DISC_OVER;
  discShotActive = false;
  if (discScore > discBest) {
    discBest = discScore;
    settings.putUShort("discBest", discBest);
  }
  Serial.printf("Disc Defender over: %u, best %u\n", discScore, discBest);
  playDiscTone(180, 220);
  screenDirty = true;
}

void boostDiscShield() {
  if (discPhase != DISC_PLAYING) return;
  uint32_t now = millis();
  if (discShotActive && discShotRadius <= 89) {
    discFeedback = 5;
    discFeedbackUntil = now + 350;
    return;
  }
  discBoostUntil = now + 1100;
  discFeedback = 4;
  discFeedbackUntil = now + 320;
  playDiscTone(430, 45);
}

bool aimDiscShield(int x, int y) {
  if (discPhase != DISC_PLAYING) return false;
  float dx = x - 233;
  float dy = y - 282;
  float radiusSquared = dx * dx + dy * dy;
  if (radiusSquared < 50 * 50 || radiusSquared > 130 * 130) return false;
  discShieldAngle = atan2f(dy, dx);
  discLastFrameAt = 0;
  return true;
}

void drawDiscFrame() {
  if (!discCanvasTried && displayReady) {
    discCanvasTried = true;
    discCanvas = new Arduino_Canvas(296, 240, gfx, 85, 162);
    if (!discCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete discCanvas;
      discCanvas = nullptr;
      Serial.println("WARNING: Disc Defender framebuffer unavailable");
    }
  }
  Arduino_GFX *field = discCanvas ?
      static_cast<Arduino_GFX *>(discCanvas) : static_cast<Arduino_GFX *>(gfx);
  int ox = discCanvas ? 0 : 85;
  int oy = discCanvas ? 0 : 162;
  int cx = ox + 148;
  int cy = oy + 120;
  if (discCanvas) field->fillScreen(BG);
  else field->fillRect(ox, oy, 296, 240, BG);

  for (int spoke = 0; spoke < 12; ++spoke) {
    float angle = spoke * (PI / 6);
    field->drawLine(cx + cosf(angle) * 46, cy + sinf(angle) * 46,
                    cx + cosf(angle) * 117, cy + sinf(angle) * 117, GRID_DIM);
  }
  field->drawCircle(cx, cy, 118, BLUE);
  field->drawCircle(cx, cy, 89, GRID_MID);
  field->drawCircle(cx, cy, 43, GRID_DIM);
  field->fillCircle(cx, cy, 23, PANEL);
  field->drawCircle(cx, cy, 23, CYAN);
  field->fillCircle(cx, cy, 7, WHITE);
  field->setTextSize(1);
  field->setTextColor(MUTED);
  field->setCursor(cx - 12, cy + 28);
  field->print("CORE");

  bool boostActive = discBoostUntil && millis() < discBoostUntil;
  for (int step = -6; step <= 6; ++step) {
    float angle = discShieldAngle + step * 0.055f;
    field->fillCircle(cx + cosf(angle) * 89,
                      cy + sinf(angle) * 89, 5,
                      boostActive ? MAZE_GOLD : abs(step) < 3 ? WHITE : CYAN);
  }

  if (discShotActive) {
    for (int trail = 2; trail >= 1; --trail) {
      float radius = min(discShotRadius + trail * 9, 118.0f);
      field->fillCircle(cx + cosf(discShotAngle) * radius,
                        cy + sinf(discShotAngle) * radius, 2, PURPLE);
    }
    int shotX = cx + cosf(discShotAngle) * discShotRadius;
    int shotY = cy + sinf(discShotAngle) * discShotRadius;
    field->fillRect(shotX - 6, shotY - 6, 13, 13, MAZE_ENEMY);
    field->fillRect(shotX - 2, shotY - 2, 5, 5, WHITE);
  }
  if (discFeedback && millis() < discFeedbackUntil) {
    const char *label = discFeedback == 2 ? "BOOST +3" :
                        discFeedback == 1 ? "BLOCK +1" :
                        discFeedback == 3 ? "CORE HIT" :
                        discFeedback == 4 ? "SHIELD CHARGED" : "SHOT PASSED";
    int width = strlen(label) * 12;
    field->fillRoundRect(cx - width / 2 - 12, cy - 13,
                         width + 24, 29, 8, PANEL);
    field->setTextSize(2);
    field->setTextColor(discFeedback == 3 ? MAZE_ENEMY : CYAN);
    field->setCursor(cx - width / 2, cy - 6);
    field->print(label);
  }
  if (discCanvas) discCanvas->flush();
  discLastFrameAt = millis();
}

void drawDiscDefender() {
  chrome("DISC DEFENDER");
  if (discPhase == DISC_READY) {
    textCentered("DEFEND THE CORE", 198, 2, WHITE);
    textCentered(gyroReady ? "TWIST OR TOUCH RING" : "TOUCH RING TO AIM",
                 237, 2, CYAN);
    textCentered("BUTTON/CORE: BOOST", 266, 2, WHITE);
    textCentered("THREE HITS TO LOSE", 292, 2, MUTED);
    button(101, 352, 264, 55, "START", CYAN);
  } else if (discPhase == DISC_OVER) {
    textCentered("CORE DOWN", 205, 3, MAZE_ENEMY);
    textCentered("SCORE " + String(discScore), 257, 2, WHITE);
    textCentered("BEST " + String(discBest), 283, 2, CYAN);
    button(101, 352, 264, 55, "TRY AGAIN", CYAN);
  } else {
    textCentered("SCORE " + String(discScore) + "   HP " + String(discLives),
                 139, 2, WHITE);
    drawDiscFrame();
    textCentered("STREAK " + String(discStreak) + "  /  ARC BLOCKS  /  BUTTON BOOSTS",
                 423, 1, MUTED);
  }
  if (discPhase != DISC_PLAYING)
    textCentered("< BACK TO THE GRID", 423, 1, MUTED);
}

void updateDiscDefender() {
  if (screen != DISC_DEFENDER || discPhase != DISC_PLAYING || !displayReady) return;
  uint32_t now = millis();
  uint32_t elapsed = now - discLastUpdateAt;
  if (elapsed < 25) return;
  discLastUpdateAt = now;
  float seconds = min(elapsed, uint32_t(60)) / 1000.0f;
  if (gyroReady) {
    GyroscopeData gyro;
    if (imu.readGyro(gyro) && fabsf(gyro.dps.z) > 7)
      discShieldAngle -= gyro.dps.z * (PI / 180.0f) * seconds * DISC_TWIST_GAIN;
  }
  if (discShotActive) {
    float speed = 31 + min(discScore, uint16_t(20)) * 0.7f;
    float previousRadius = discShotRadius;
    discShotRadius -= speed * seconds;
    if (previousRadius > 89 && discShotRadius <= 89) {
      float angleError = discAngleDistance(discShieldAngle, discShotAngle);
      if (angleError <= 0.39f) {
        bool perfect = discBoostUntil && now <= discBoostUntil &&
                       angleError <= 0.23f;
        discScore += perfect ? 3 : 1;
        ++discStreak;
        discShotActive = false;
        discBoostUntil = 0;
        discNextShotAt = now + 450;
        discFeedback = perfect ? 2 : 1;
        discFeedbackUntil = now + 380;
        playDiscTone(perfect ? 880 : 620, perfect ? 150 : 100);
        Serial.printf("Disc Defender %s: score %u, streak %u\n",
                      perfect ? "perfect" : "block", discScore, discStreak);
        screenDirty = true;
        return;
      }
      discBoostUntil = 0;
      discFeedback = 5;
      discFeedbackUntil = now + 350;
    }
    if (discShotRadius <= 28) {
      discShotActive = false;
      discStreak = 0;
      --discLives;
      if (!discLives) {
        finishDiscDefender();
        return;
      }
      discFeedback = 3;
      discFeedbackUntil = now + 420;
      discNextShotAt = now + 550;
      playDiscTone(230, 150);
      screenDirty = true;
    }
  } else if (static_cast<int32_t>(now - discNextShotAt) >= 0) {
    spawnDiscShot();
  }
  if (!screenDirty && now - discLastFrameAt >= 33) drawDiscFrame();
}

void pollDiscTouch(bool pending) {
  uint32_t now = millis();
  if (!pending && (!discTouchTracking || now - discLastTouchPoll < 25)) return;
  discLastTouchPoll = now;
  int16_t x[1] = {}, y[1] = {};
  if (!touch.getPoint(x, y, 1)) {
    if (discTouchTracking && now - discLastTouchSeen > 120)
      discTouchTracking = false;
    return;
  }
  if (discTouchTracking && now - discLastTouchSeen > 180)
    discTouchTracking = false;
  discLastTouchSeen = now;
  if (!discTouchTracking) {
    discTouchTracking = true;
    if (x[0] >= 91 && x[0] <= 184 && y[0] >= 91 && y[0] <= 136) {
      discTouchTracking = false;
      handleTap(x[0], y[0]);
      return;
    }
    if (!aimDiscShield(x[0], y[0])) {
      float dx = x[0] - 233;
      float dy = y[0] - 282;
      if (dx * dx + dy * dy <= 50 * 50) boostDiscShield();
    }
  } else {
    aimDiscShield(x[0], y[0]);
  }
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
    if (screen == GRIDLEY_SAYS) {
      bootLastTapAt = 0;
      if (gridPhase == GRID_READY || gridPhase == GRID_OVER) startGridleySays();
      else gridleyInput(GRID_PRESS);
      return;
    }
    if (screen == GRID_MAZE) {
      bootLastTapAt = 0;
      fireMazePulse();
      return;
    }
    if (screen == DISC_DEFENDER) {
      bootLastTapAt = 0;
      if (discPhase == DISC_PLAYING) boostDiscShield();
      else startDiscDefender();
      return;
    }
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
    case GRID_MAZE: drawMaze(); break;
    case SOUND_ART: drawSoundArt(); break;
    case GRIDLEY_SAYS: drawGridleySays(); break;
    case DISC_DEFENDER: drawDiscDefender(); break;
    case STATUS_SETTINGS: drawSettings(); break;
  }
}

void handleTap(int x, int y) {
  AppScreen previousScreen = screen;
  if (screen == LAUNCHER) {
    if (x >= 84 && x <= 382 && y >= 146 && y <= 194) {
      resetMaze();
      screen = GRID_MAZE;
    } else if (x >= 84 && x <= 382 && y >= 201 && y <= 249) {
      startSoundArt();
      screen = SOUND_ART;
    } else if (x >= 84 && x <= 382 && y >= 256 && y <= 304) {
      gridPhase = GRID_READY;
      gridTouchTracking = false;
      screen = GRIDLEY_SAYS;
    } else if (x >= 84 && x <= 382 && y >= 311 && y <= 359) {
      discPhase = DISC_READY;
      screen = DISC_DEFENDER;
    } else if (x >= 140 && x <= 326 && y >= 380 && y <= 435) {
      settingsBatterySnapshot = batteryText();
      settingsPowerSnapshot = powerStatusText();
      screen = STATUS_SETTINGS;
    }
  } else {
    if (x >= 91 && x <= 184 && y >= 91 && y <= 136) {
      if (screen == SOUND_ART) stopSoundArt();
      screen = LAUNCHER;
    } else if (screen == GRIDLEY_SAYS) {
      if ((gridPhase == GRID_READY || gridPhase == GRID_OVER) &&
          y >= 345 && y <= 420) startGridleySays();
    } else if (screen == DISC_DEFENDER) {
      if (discPhase != DISC_PLAYING &&
          x >= 101 && x <= 365 && y >= 352 && y <= 407)
        startDiscDefender();
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
  if (screen != previousScreen) screenDirty = true;
}

void pollTouch() {
  if (!touchReady) return;
  noInterrupts();
  bool pending = touchPending;
  touchPending = false;
  interrupts();
  if (screen == GRIDLEY_SAYS) {
    pollGridleyTouch(pending);
    return;
  }
  if (screen == DISC_DEFENDER && discPhase == DISC_PLAYING) {
    pollDiscTouch(pending);
    return;
  }
  if (!pending) return;
  int16_t x[1] = {}, y[1] = {};
  if (!touch.getPoint(x, y, 1)) return;
  if (millis() - lastTap < 260) return;
  Serial.printf("Touch: %d,%d\n", x[0], y[0]);
  AppScreen previousScreen = screen;
  handleTap(x[0], y[0]);
  // A launcher miss should not delay the next attempt to tap an app.
  if (previousScreen != LAUNCHER || screen != previousScreen) lastTap = millis();
}

void sendWifiSetupPage() {
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html",
           "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Gridley Wi-Fi setup</title><style>body{font:18px system-ui;max-width:32rem;"
           "margin:3rem auto;padding:0 1rem}label{display:block;margin:1rem 0}"
           "input{display:block;font:inherit;width:100%;padding:.6rem;box-sizing:border-box}"
           "button{font:inherit;padding:.7rem 1.2rem}</style><h1>Gridley Wi-Fi setup</h1>"
           "<p>Enter a 2.4 GHz Wi-Fi network.</p>"
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

void sendDevicePage() {
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html",
           "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Gridley</title><style>body{font:18px system-ui;max-width:32rem;"
           "margin:3rem auto;padding:0 1rem;background:#10131c;color:#eee}"
           "h1{color:#49dce9}</style><h1>Gridley is online</h1>"
           "<p>Pick a program on the puck to play.</p></html>");
}

void configureWeb() {
  web.on("/", HTTP_GET, []() {
    if (networkMode == SETUP) sendWifiSetupPage();
    else sendDevicePage();
  });
  web.on("/save", HTTP_POST, saveWifi);
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
  gridBest = settings.getUShort("gridBest", 0);
  discBest = settings.getUShort("discBest", 0);
  if (audioVolume > 100) audioVolume = 100;

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
    imuReady = imu.setAccelFullScaleRange(AccelFullScaleRange::FS_4G) &&
               imu.setAccelOutputDataRate(112.0f) && imu.enableAccel();
    if (imuReady) {
      gyroReady = imu.setGyroFullScaleRange(GyroFullScaleRange::FS_1000_DPS) &&
                  imu.setGyroOutputDataRate(112.0f) && imu.enableGyro();
    }
    Serial.println(imuReady ? "Motion accelerometer ready" :
                   "WARNING: Motion accelerometer setup failed");
    Serial.println(gyroReady ? "Gyroscope ready" :
                   "WARNING: Gyroscope unavailable");
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
  pumpAudio();
  if (screenDirty) {
    drawScreen();
    screenDirty = false;
  }
  updateSoundArt();
  updateMaze();
  updateGridleySays();
  updateDiscDefender();
  if (millis() - lastLog > 5000) {
    logStatus();
    lastLog = millis();
  }
  delay(10);
}

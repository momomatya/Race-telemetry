/**
- 38Racing - M5Stack Tab5 Racing Dash & BLE Telemetry Bridge
- - Screen Rotated 180 deg (setRotation(3))
- - Speaker Amp (IOE P1) & ESP32-C6 BLE Enable
- - CAN: TWAI_MODE_NORMAL @ 1 Mbit/s (AiM Solo 2 DL 5-pin EXP)
-   * 0x420: RPM (0-1), Speed x10 (2-3), WaterTemp x10 (6-7)
-   * 0x421: Fuel CC05 (0-1), LAP CC06 (2-3), RollingTime CC07 (4-5), +-RefTime CC08 (6-7)
-   * 0x422: Throttle CC10 (2-3), Brake CC11 (4-5), LapTime CC12 (6-7)
-   * 0x423: Steer (0-1), Lat G x100 (4-5), Inline G x100 (6-7) [Auto Zero-Calibrated]
-   * 0x424: Vert G x100 (4-5)
- - 8-Circuit Official Sector Presets & NVS Best Persistence
-   * Sector Board Tap: Switch Circuit / 1.5s Long Press: Clear Current Circuit Best
- - Shift LEDs: Full All-LED Red/Off Flash at Rev Limit
- - Boot Splash: Loads /38.jpg & /boot.wav (up to 12s) from microSD
- - Controls: Page Tabs / Shift Limit / Brightness (30/60/85/100%) / BLE / Mode
- - Timer: Default OFF, Tap to cycle (OFF/10/15/20/30M) & Auto-Start, Flash + Alarm at 00:00
- - Right-Bottom Card: Top = Clock / Bottom = Fuel % (2-sec Median Filter)
 */

#include <M5Unified.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <driver/twai.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>
#include <string.h>

#if __has_include("boot_logo.h")
  #include "boot_logo.h"
  #define HAS_BOOT_LOGO 1
#else
  #define HAS_BOOT_LOGO 0
#endif

// --- 起動音メロディ選択 (microSDに /boot.wav が無い時に鳴る内蔵メロディ) ---
#define BOOT_MELODY_TYPE 2

// --- M5Stack Tab5 microSD (SD_MMC 4-bit SDIO) ピン設定 ---
#define TAB5_SD_CLK 43
#define TAB5_SD_CMD 44
#define TAB5_SD_D0  39
#define TAB5_SD_D1  40
#define TAB5_SD_D2  41
#define TAB5_SD_D3  42

// --- M5Stack Tab5 Grove Port.A (HY2.0-4P) CAN ピン設定 ---
#define CAN_TX_PIN GPIO_NUM_53
#define CAN_RX_PIN GPIO_NUM_54

// --- BLE Nordic UART Service (NUS) UUID ---
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_CHAR_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_CHAR_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// --- カラーパレット (RGB565) ---
static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_PANEL    = 0x0862;
static const uint16_t COL_BORDER   = 0x39E7;
static const uint16_t COL_MUTED    = 0x94B2;
static const uint16_t COL_WHITE    = 0xFFFF;
static const uint16_t COL_GOLD     = 0xFEA0;
static const uint16_t COL_AMBER    = 0xFD20;
static const uint16_t COL_RED      = 0xF800;
static const uint16_t COL_RED_DARK = 0x6000;
static const uint16_t COL_GREEN    = 0x07E0;
static const uint16_t COL_EMERALD  = 0x06EC;
static const uint16_t COL_BLUE     = 0x1C9F;
static const uint16_t COL_CYAN     = 0x07FF;

// --- 部分描画用 軽量スプライト群 ---
M5Canvas sprLed(&M5.Display);      // 1240x30 : 上部15連シフトLED
M5Canvas sprLapCount(&M5.Display); // 208x168 : 上段左 ラップ数 (タップで0リセット)
M5Canvas sprSpeed(&M5.Display);    // 402x168 : 上段右 スピード & RPM
M5Canvas sprLap(&M5.Display);      // 624x204 : 中段 特大LAPタイマー & BEST Δ
M5Canvas sprCard(&M5.Display);     // 198x210 : 下段 水温・タイマー・時計/燃料カード共用
M5Canvas sprSecRow(&M5.Display);   // 588x94  : 右側 6段セクターの1行分共用
M5Canvas sprGBall(&M5.Display);    // 208x210 : P2 Gボール (タップでゼロ点較正)
M5Canvas sprP3Lap(&M5.Display);    // 624x136 : P3 左上ラップタイマー
M5Canvas sprP3Info(&M5.Display);   // 608x136 : P3 右上ステータスバー
M5Canvas sprSteer(&M5.Display);    // 450x450 : P3 ステアリング＋内蔵Gボール
M5Canvas sprPedal(&M5.Display);    // 240x456 : P3 ブレーキ・アクセル共用

BLECharacteristic* pTxCharacteristic = nullptr;
bool bleClientConnected = false;
bool prevBleConnected   = false;
bool isDemoMode         = false;
bool prevDemoMode       = false;
int  currentPage        = 0;     // 0: P1 RACE, 1: P2 G-BALL, 2: P3 PEDAL
bool needFullRedraw     = true;

// --- 輝度プリセット (5% / 15% / 30% / 60% / 85% / 100%) ---
const uint8_t BRIGHT_LEVELS[6] = { 13, 38, 75, 150, 215, 255 };
const int     BRIGHT_PCT[6]    = { 5, 15, 30, 60, 85, 100 };
int brightIdx = 4; // デフォルト 85%

// --- セッションタイマー (デフォルト OFF) ---
const int TIMER_PRESETS_MIN[5] = { 0, 10, 15, 20, 30 };
int      timerPresetIdx   = 0;
float    sessionRemainSec = 0.0f;
bool     timerRunning     = false;
bool     timerAlarmActive = false;
bool     prevAlarmFlash   = false;
uint32_t lastAlarmBeepMs  = 0;

// --- 燃料 2秒間中央値フィルタ (100ms × 20個 = 2.0秒) ---
static const int FUEL_BUF_LEN = 20;
int      fuelBuf[FUEL_BUF_LEN];
int      fuelBufIdx     = 0;
bool     fuelBufInit    = false;
int      rawFuelPct     = 68;
int      prevFuelDrawn  = -1;
uint32_t lastFuelSampMs = 0;

// --- 時計フォールバック用 ---
uint32_t bootEpochSec = 0;

// --- CANラップ数の0リセット用オフセット ---
int rawCanLap = 0;
int lapOffset = 0;

// --- 8サーキット 公式セクター距離プリセット (単位: m) & NVS保存 ---
struct CircuitPreset {
  const char* name;
  float s1Dist;    // スタート〜S1終了までの距離
  float s2Dist;    // スタート〜S2終了までの距離
  float totalDist; // 1周全長
};

const CircuitPreset CIRCUITS[8] = {
  { "FSW",       1550.0f, 3150.0f, 4563.0f }, // 富士スピードウェイ
  { "TC2000",     650.0f, 1350.0f, 2045.0f }, // 筑波サーキット
  { "MOTEGI",    1600.0f, 3200.0f, 4801.0f }, // モビリティリゾートもてぎ
  { "SUGO",      1150.0f, 2350.0f, 3586.0f }, // スポーツランドSUGO
  { "NIKKO",      340.0f,  680.0f, 1031.0f }, // 日光サーキット
  { "SODEGAURA",  800.0f, 1620.0f, 2436.0f }, // 袖ヶ浦フォレストレースウェイ
  { "MOBARA",     390.0f,  780.0f, 1170.0f }, // 茂原ツインサーキット(東)
  { "HONJO",      370.0f,  740.0f, 1112.0f }  // 本庄サーキット
};

Preferences prefs;
int   circuitIdx  = 0;
float bestS1      = 0.0f;
float bestS2      = 0.0f;
float bestS3      = 0.0f;
bool  s1Recorded  = false;
bool  s2Recorded  = false;

// --- セクターボード長押し検知用 ---
bool     secTouchActive    = false;
bool     secLongPressFired = false;
uint32_t secTouchStartMs   = 0;

// --- G軌跡 & ブレーキピーク ---
static const int G_HIST_LEN = 16;
float gHistX[G_HIST_LEN] = {0};
float gHistY[G_HIST_LEN] = {0};
int   gHistIdx = 0;
int   brkPeak = 0;
uint32_t brkPeakHoldMs = 0;

// --- Solo 2 DL Gセンサー受信時刻 & 自動ゼロ点較正 ---
uint32_t lastCanGMs  = 0;
float    rawCanGx    = 0.0f;
float    rawCanGy    = 0.0f;
float    gOffsetX    = 0.0f;
float    gOffsetY    = 0.0f;
bool     gCalibrated = false;

void calibrateGZero() {
  gOffsetX    = rawCanGx;
  gOffsetY    = rawCanGy;
  gCalibrated = true;
}

// --- テレメトリデータ ---
struct TelemetryData {
  float  spd       = 0.0f;
  int    rpm       = 850;
  int    gear      = 1;
  int    thr       = 0;
  int    brk       = 0;
  float  gx        = 0.0f;
  float  gy        = 0.0f;
  float  steer     = 0.0f;
  int    wtemp     = 88;
  int    fuel      = 68; // ガソリン残量 (%)・2秒中央値
  int    revLimit  = 7000;
  int    lap       = 1;
  float  distM     = 0.0f;
  float  lapTime   = 0.0f;
  float  lastLap   = 0.0f;
  float  bestLap   = 0.0f;
  float  optLap    = 0.0f;
  float  liveDelta = 0.0f;
  float  s1        = 0.0f;
  float  s2        = 0.0f;
  float  s3        = 0.0f;
  float  dS1       = 0.0f;
  float  dS2       = 0.0f;
  float  dS3       = 0.0f;
  float  dLast     = 0.0f;
  double lat       = 0.0;
  double lon       = 0.0;
  float  lonG      = 0.0f;
  float  latG      = 0.0f;
  float  vertG     = 0.0f;
} telem;

// --- 変更検知キャッシュ ---
int   prevLedLitCount   = -1;
bool  prevLedFlash      = false;
bool  prevAtRevLimit    = false;
int   prevLapCountDrawn = -1;
int   prevSpdDrawn      = -1;
int   prevRpmDrawn      = -1;
int   prevWtempDrawn    = -999;
int   prevTimerSec      = -1;
int   prevClockSec      = -1;
int   prevP3Spd         = -1;
int   prevP3Sec         = -1;
int   prevP3Lap         = -1;
bool  sectorsDirty      = true;
bool  headerDirty       = true;

uint32_t lastFrameMs   = 0;
uint32_t lastBleMs     = 0;
uint32_t lastLoopMs    = 0;
uint32_t lastGHistMs   = 0;
uint32_t lastCanStatMs = 0;
float    demoPhase     = 0.0f;

// --- NVS サーキット別ベスト読込・保存・個別消去 ---
void loadCircuitBests(int idx) {
  char key[16];
  prefs.begin("38dash", true);
  snprintf(key, sizeof(key), "bl_%d", idx);  telem.bestLap = prefs.getFloat(key, 0.0f);
  snprintf(key, sizeof(key), "bs1_%d", idx); bestS1        = prefs.getFloat(key, 0.0f);
  snprintf(key, sizeof(key), "bs2_%d", idx); bestS2        = prefs.getFloat(key, 0.0f);
  snprintf(key, sizeof(key), "bs3_%d", idx); bestS3        = prefs.getFloat(key, 0.0f);
  prefs.end();

  telem.optLap = (bestS1 > 0 && bestS2 > 0 && bestS3 > 0) ? (bestS1 + bestS2 + bestS3) : 0.0f;
  telem.s1 = telem.s2 = telem.s3 = 0.0f;
  telem.dS1 = telem.dS2 = telem.dS3 = 0.0f;
  telem.lastLap = 0.0f;
  telem.dLast   = 0.0f;
  s1Recorded    = false;
  s2Recorded    = false;
  sectorsDirty  = true;
}

void saveCircuitBests(int idx) {
  char key[16];
  prefs.begin("38dash", false);
  prefs.putInt("trkIdx", idx);
  snprintf(key, sizeof(key), "bl_%d", idx);  prefs.putFloat(key, telem.bestLap);
  snprintf(key, sizeof(key), "bs1_%d", idx); prefs.putFloat(key, bestS1);
  snprintf(key, sizeof(key), "bs2_%d", idx); prefs.putFloat(key, bestS2);
  snprintf(key, sizeof(key), "bs3_%d", idx); prefs.putFloat(key, bestS3);
  prefs.end();
}

void clearCircuitBests(int idx) {
  char key[16];
  prefs.begin("38dash", false);
  snprintf(key, sizeof(key), "bl_%d", idx);  prefs.remove(key);
  snprintf(key, sizeof(key), "bs1_%d", idx); prefs.remove(key);
  snprintf(key, sizeof(key), "bs2_%d", idx); prefs.remove(key);
  snprintf(key, sizeof(key), "bs3_%d", idx); prefs.remove(key);
  prefs.end();

  telem.bestLap = 0.0f;
  telem.optLap  = 0.0f;
  bestS1 = bestS2 = bestS3 = 0.0f;
  telem.s1 = telem.s2 = telem.s3 = 0.0f;
  telem.dS1 = telem.dS2 = telem.dS3 = 0.0f;
  telem.dLast = 0.0f;
  s1Recorded  = false;
  s2Recorded  = false;
  sectorsDirty = true;

  // 消去完了ビープ (2音)
  M5.Speaker.tone(1800, 90);
  delay(110);
  M5.Speaker.tone(2600, 140);
}

// --- 燃料 2秒間中央値計算 ---
int calcFuelMedian() {
  int tmp[FUEL_BUF_LEN];
  memcpy(tmp, fuelBuf, sizeof(tmp));
  for (int i = 0; i < FUEL_BUF_LEN - 1; i++) {
    for (int j = i + 1; j < FUEL_BUF_LEN; j++) {
      if (tmp[j] < tmp[i]) {
        int t = tmp[i];
        tmp[i] = tmp[j];
        tmp[j] = t;
      }
    }
  }
  return tmp[FUEL_BUF_LEN / 2];
}

// --- BLE コールバック ---
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleClientConnected = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    bleClientConnected = false;
    pServer->startAdvertising();
  }
};

void setupBLE() {
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
  delay(200);

  BLEDevice::init("M5-38RACING-TELEM");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(NUS_SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(
    NUS_TX_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  pService->createCharacteristic(
    NUS_RX_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );

  pService->start();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(NUS_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
}

void setupCAN() {
  // Solo 2 DL (1 Mbit/s EXPポート) と1対1接続でACKを返すため TWAI_MODE_NORMAL を使用
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t res = twai_driver_install(&g_config, &t_config, &f_config);
  Serial.printf("[CAN] driver install: %s\n", esp_err_to_name(res));
  if (res == ESP_OK) {
    esp_err_t st = twai_start();
    Serial.printf("[CAN] start: %s (TX=%d, RX=%d)\n", esp_err_to_name(st), CAN_TX_PIN, CAN_RX_PIN);
  }
}

// 無効値(0xCFFE)じゃなければ out に値を入れて true を返す
bool readValidInt16LE(const uint8_t* d, int offset, int16_t& out) {
  uint16_t raw = (uint16_t)((d[offset + 1] << 8) | d[offset]);
  if (raw == 0xCFFE) return false;
  out = (int16_t)raw;
  return true;
}

// --- JPEG / PNG の元解像度を読み取って画面いっぱいに拡大縮小するヘルパー ---
bool getJpegSize(const uint8_t* data, size_t len, int& w, int& h) {
  if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < len) {
    if (data[i] != 0xFF) { i++; continue; }
    while (i < len && data[i] == 0xFF) i++;
    if (i >= len) break;
    uint8_t marker = data[i++];
    if (marker == 0xD9 || marker == 0xDA) break;
    if (i + 2 > len) break;
    uint16_t segLen = ((uint16_t)data[i] << 8) | data[i + 1];
    if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
        (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF)) {
      if (i + 7 < len) {
        h = ((int)data[i + 3] << 8) | data[i + 4];
        w = ((int)data[i + 5] << 8) | data[i + 6];
        return (w > 0 && h > 0);
      }
    }
    if (segLen < 2) break;
    i += segLen;
  }
  return false;
}

bool getPngSize(const uint8_t* data, size_t len, int& w, int& h) {
  if (len < 24 || data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G') return false;
  w = ((int)data[16] << 24) | ((int)data[17] << 16) | ((int)data[18] << 8) | data[19];
  h = ((int)data[20] << 24) | ((int)data[21] << 16) | ((int)data[22] << 8) | data[23];
  return (w > 0 && h > 0);
}

bool drawImageFitScreen(const uint8_t* data, size_t len, bool isPng) {
  int imgW = 0, imgH = 0;
  bool gotSize = isPng ? getPngSize(data, len, imgW, imgH) : getJpegSize(data, len, imgW, imgH);
  float scale = 1.0f;
  int drawX = 0, drawY = 0;

  if (gotSize) {
    float sx = 1280.0f / (float)imgW;
    float sy = 720.0f  / (float)imgH;
    scale = (sx < sy) ? sx : sy;
    drawX = (1280 - (int)(imgW * scale)) / 2;
    drawY = (720  - (int)(imgH * scale)) / 2;
    if (drawX < 0) drawX = 0;
    if (drawY < 0) drawY = 0;
  }

  if (isPng) {
    return M5.Display.drawPng(data, len, drawX, drawY, 0, 0, 0, 0, scale, scale);
  } else {
    return M5.Display.drawJpg(data, len, drawX, drawY, 0, 0, 0, 0, scale, scale);
  }
}

// --- 内蔵メロディ再生 ---
struct NoteStep {
  float freqHz;
  int   durMs;
  int   gapMs;
  int   ch;
};

void playBuiltInBootMelody() {
#if BOOT_MELODY_TYPE == 1
  const NoteStep melody[] = {
    { 880.0f,  110, 220, 0 },
    { 880.0f,  110, 220, 0 },
    { 880.0f,  110, 220, 0 },
    { 1760.0f, 450, 0,   0 },
    { 1318.5f, 450, 0,   1 },
  };
#elif BOOT_MELODY_TYPE == 2
  const NoteStep melody[] = {
    { 523.25f, 110, 125, 0 },
    { 659.25f, 110, 125, 0 },
    { 783.99f, 110, 125, 0 },
    { 1046.5f, 480, 0,   0 },
    { 783.99f, 480, 0,   1 },
    { 659.25f, 480, 0,   2 },
  };
#else
  const NoteStep melody[] = {
    { 659.25f,  90, 100, 0 },
    { 783.99f,  90, 100, 1 },
    { 987.77f,  90, 100, 2 },
    { 1318.51f, 420, 0,  3 },
  };
#endif

  int count = sizeof(melody) / sizeof(melody[0]);
  for (int i = 0; i < count; i++) {
    if (melody[i].freqHz > 0.0f) {
      M5.Speaker.tone(melody[i].freqHz, melody[i].durMs, melody[i].ch, true);
    }
    if (melody[i].gapMs > 0) {
      delay(melody[i].gapMs);
    }
  }
}

// --- 起動画面 ---
void showBootSplash() {
  M5.Display.fillScreen(COL_BG);
  delay(250);

  bool logoDrawn = false;
  uint8_t* wavBuf = nullptr;
  size_t   wavLen = 0;
  const char* sdDiagMsg = "SD MOUNT FAILED (Format as FAT32)";

  SD_MMC.setPins(TAB5_SD_CLK, TAB5_SD_CMD, TAB5_SD_D0, TAB5_SD_D1, TAB5_SD_D2, TAB5_SD_D3);
  bool sdReady = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT);
  if (!sdReady) {
    sdReady = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
  }

  if (sdReady) {
    sdDiagMsg = "38.jpg / 38.png NOT FOUND in SD root";
    const char* imgCandidates[6] = { "/38.jpg", "/38.JPG", "/38.jpeg", "/38.png", "/38.PNG", "/38.jpg.jpg" };
    for (int i = 0; i < 6; i++) {
      if (SD_MMC.exists(imgCandidates[i])) {
        File f = SD_MMC.open(imgCandidates[i], FILE_READ);
        if (f && f.size() > 0 && f.size() < 4 * 1024 * 1024) {
          size_t sz = f.size();
          uint8_t* buf = (uint8_t*)ps_malloc(sz);
          if (buf) {
            size_t rd = f.read(buf, sz);
            bool isPng = (i == 3 || i == 4);
            logoDrawn = drawImageFitScreen(buf, rd, isPng);
            if (!logoDrawn) {
              sdDiagMsg = "JPG DECODE ERR (Resave 38.jpg in MS Paint)";
            }
            free(buf);
          }
          f.close();
          if (logoDrawn) break;
        }
      }
    }

    const char* wavCandidates[2] = { "/boot.wav", "/BOOT.WAV" };
    for (int i = 0; i < 2; i++) {
      if (SD_MMC.exists(wavCandidates[i])) {
        File wf = SD_MMC.open(wavCandidates[i], FILE_READ);
        if (wf && wf.size() > 44 && wf.size() < 8 * 1024 * 1024) {
          wavLen = wf.size();
          wavBuf = (uint8_t*)ps_malloc(wavLen);
          if (wavBuf) {
            wf.read(wavBuf, wavLen);
          }
          wf.close();
          break;
        }
      }
    }
    SD_MMC.end();
  }

#if HAS_BOOT_LOGO
  if (!logoDrawn) {
    logoDrawn = drawImageFitScreen(boot_logo_jpg, boot_logo_jpg_len, false);
  }
#endif

  if (!logoDrawn) {
    M5.Display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(2.6f);
    M5.Display.setTextColor(COL_RED);
    M5.Display.drawString("38 Racing", 640, 290);

    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextSize(1.0f);
    M5.Display.setTextColor(COL_GOLD);
    M5.Display.drawString(sdDiagMsg, 640, 420);
  }

  uint32_t startMs = millis();
  if (wavBuf && wavLen > 44) {
    M5.Speaker.playWav(wavBuf, wavLen, 1, 0, true);
    while (millis() - startMs < 12000) {
      M5.update();
      if (M5.Touch.getDetail().wasPressed()) {
        M5.Speaker.stop();
        M5.Speaker.tone(2400, 35);
        break;
      }
      if (!M5.Speaker.isPlaying() && (millis() - startMs > 1800)) {
        break;
      }
      delay(15);
    }
    M5.Speaker.stop();
    free(wavBuf);
  } else {
    playBuiltInBootMelody();
    while (millis() - startMs < 2400) {
      M5.update();
      if (M5.Touch.getDetail().wasPressed()) {
        M5.Speaker.tone(2400, 35);
        break;
      }
      delay(15);
    }
  }
}

// --- Tab5 内蔵RTC 初期合わせ ---
void initClockFromCompileTimeIfNeeded() {
  int hh = 12, mm = 0, ss = 0;
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  bootEpochSec = (uint32_t)(hh * 3600 + mm * 60 + ss);

  if (!M5.Rtc.isEnabled()) return;
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year < 2025 || dt.date.year > 2050) {
    char monStr[4] = {0};
    int day = 1, year = 2026;
    sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
    const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* p = strstr(months, monStr);
    int month = p ? (int)((p - months) / 3) + 1 : 1;

    m5::rtc_datetime_t setDt;
    setDt.date.year    = year;
    setDt.date.month   = month;
    setDt.date.date    = day;
    setDt.date.weekDay = 0;
    setDt.time.hours   = hh;
    setDt.time.minutes = mm;
    setDt.time.seconds = ss;
    M5.Rtc.setDateTime(setDt);
  }
}

void getCurrentClockHMS(int& h, int& m, int& s) {
  if (M5.Rtc.isEnabled()) {
    auto dt = M5.Rtc.getDateTime();
    h = dt.time.hours;
    m = dt.time.minutes;
    s = dt.time.seconds;
  } else {
    uint32_t total = (bootEpochSec + millis() / 1000) % 86400;
    h = total / 3600;
    m = (total % 3600) / 60;
    s = total % 60;
  }
}

// --- CAN受信 (0x420〜0x424 AiM Solo 2 DL 実車確定版) ---
void pollCANMessages() {
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) {
    if (isDemoMode) continue;

    switch (msg.identifier) {
      case 0x420:
        if (msg.data_length_code >= 8) {
          int16_t val;
          // Ch0 (Byte 0-1): REV / エンジン回転数 (rpm)
          if (readValidInt16LE(msg.data, 0, val)) {
            telem.rpm = (val > 0) ? val : 0;
          }
          // Ch1 (Byte 2-3): Speed / 車速 (km/h × 10 → 0.1倍)
          if (readValidInt16LE(msg.data, 2, val)) {
            telem.spd = (val > 0) ? (val * 0.1f) : 0.0f;
          }
          // Ch3 (Byte 6-7): 水温 (℃ × 10 → 0.1倍)
          if (readValidInt16LE(msg.data, 6, val)) {
            telem.wtemp = (int)roundf(val * 0.1f);
          }
        }
        break;

      case 0x421:
        if (msg.data_length_code >= 8) {
          int16_t val;
          // Ch0 (CC05): FuelLevel → 2秒間中央値フィルタへ
          if (readValidInt16LE(msg.data, 0, val)) {
            rawFuelPct = constrain((val > 100) ? (int)roundf(val * 0.1f) : val, 0, 100);
          }
          // Ch1 (CC06): LAP (ラップ数)
          if (readValidInt16LE(msg.data, 2, val)) {
            int lapVal = (val >= 100) ? (val / 10) : val;
            if (lapVal >= 0 && lapVal < 500) {
              if (rawCanLap > 0 && lapVal > rawCanLap) {
                telem.distM  = 0.0f; // ラップ切替で走行距離リセット
                s1Recorded   = false;
                s2Recorded   = false;
                sectorsDirty = true;
              }
              rawCanLap = lapVal;
              int adjLap = rawCanLap - lapOffset;
              telem.lap = (adjLap >= 0) ? adjLap : 0;
            }
          }
          // Ch2 (CC07): Rolling Time (走行中ラップタイム)
          if (readValidInt16LE(msg.data, 4, val)) {
            telem.lapTime = (val > 0) ? (val * 0.1f) : 0.0f;
          }
          // Ch3 (CC08): +- Ref Time (BEST DELTA)
          if (readValidInt16LE(msg.data, 6, val)) {
            telem.liveDelta = val * 0.01f;
          }
        }
        break;

      case 0x422:
        if (msg.data_length_code >= 8) {
          int16_t val;
          // Ch1 (CC10): アクセル開度 (%)
          if (readValidInt16LE(msg.data, 2, val)) {
            telem.thr = constrain(val, 0, 100);
          }
          // Ch2 (CC11): ブレーキ (%)
          if (readValidInt16LE(msg.data, 4, val)) {
            telem.brk = constrain(val, 0, 100);
          }
          // Ch3 (CC12): Lap Time (直前のラップタイム) → S3計算＆NVSベスト保存
          if (readValidInt16LE(msg.data, 6, val)) {
            if (val > 0) {
              float lastSec = val * 0.1f;
              if (fabsf(lastSec - telem.lastLap) > 0.05f) {
                telem.lastLap = lastSec;
                if (s1Recorded && s2Recorded && lastSec > (telem.s1 + telem.s2)) {
                  telem.s3 = lastSec - telem.s1 - telem.s2;
                  if (bestS3 <= 0.0f || telem.s3 < bestS3) {
                    bestS3 = telem.s3;
                  }
                  telem.dS3 = telem.s3 - bestS3;
                }
                if (telem.bestLap <= 0.0f || lastSec < telem.bestLap) {
                  telem.bestLap = lastSec;
                }
                telem.dLast = telem.lastLap - telem.bestLap;
                if (bestS1 > 0 && bestS2 > 0 && bestS3 > 0) {
                  telem.optLap = bestS1 + bestS2 + bestS3;
                }
                saveCircuitBests(circuitIdx);
                s1Recorded   = false;
                s2Recorded   = false;
                sectorsDirty = true;
              }
            }
          }
        }
        break;

      case 0x423:
        if (msg.data_length_code >= 8) {
          int16_t val;
          // Ch0 (Byte 0-1): ステアリング舵角 (右がマイナスなので符号反転)
          if (readValidInt16LE(msg.data, 0, val)) {
            telem.steer = -(float)val;
          }
          // Ch2 (Byte 4-5): 横G (-0.01G刻みで左右反転)
          if (readValidInt16LE(msg.data, 4, val)) {
            rawCanGy = val * -0.01f;
            if (!gCalibrated) gOffsetY = rawCanGy;
            telem.gy   = rawCanGy - gOffsetY;
            telem.latG = telem.gy;
            lastCanGMs = millis();
          }
          // Ch3 (Byte 6-7): 前後G (-0.01G刻みで前後反転)
          if (readValidInt16LE(msg.data, 6, val)) {
            rawCanGx = val * -0.01f;
            if (!gCalibrated) {
              gOffsetX    = rawCanGx;
              gCalibrated = true;
            }
            telem.gx   = rawCanGx - gOffsetX;
            telem.lonG = telem.gx;
            lastCanGMs = millis();
          }
        }
        break;

      case 0x424:
        if (msg.data_length_code >= 8) {
          int16_t val;
          // Ch2 (Byte 4-5): 上下G（立てて設置時 -1.0G）
          if (readValidInt16LE(msg.data, 4, val)) {
            telem.vertG = val * 0.01f;
          }
        }
        break;

      default:
        break;
    }
  }
}

void updateIMUGForce() {
  // CANから直近500ms以内にG値を受信していれば、Solo 2 DLの値を優先して上書きしない
  if (!isDemoMode && (millis() - lastCanGMs > 500) && M5.Imu.isEnabled()) {
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    telem.gx = telem.gx * 0.8f + ax * 0.2f;
    telem.gy = telem.gy * 0.8f + ay * 0.2f;
  }
}

void formatLapStr(float sec, char* out, size_t len) {
  if (sec <= 0.001f) {
    snprintf(out, len, "-'--.--");
    return;
  }
  int m = (int)(sec / 60.0f);
  float r = sec - (m * 60.0f);
  snprintf(out, len, "%d'%05.2f", m, r);
}

// --- 1. 上部15連シフトLED ---
void updateShiftLeds(bool force) {
  const int ledCount = 15;
  float rpmRatio = (float)telem.rpm / (float)telem.revLimit;
  bool atRevLimit = (telem.rpm >= telem.revLimit);
  bool flashOn = ((millis() / 65) % 2 == 0);

  int litCount = 0;
  for (int i = 0; i < ledCount; i++) {
    float thresh = 0.50f + (0.50f * i / (ledCount - 1));
    if (rpmRatio >= thresh) litCount = i + 1;
  }

  if (!force) {
    if (atRevLimit && prevAtRevLimit && flashOn == prevLedFlash) return;
    if (!atRevLimit && !prevAtRevLimit && litCount == prevLedLitCount) return;
  }

  prevAtRevLimit  = atRevLimit;
  prevLedLitCount = litCount;
  prevLedFlash    = flashOn;

  sprLed.fillSprite(COL_BG);
  int gap = 8;
  int ledW = ((int)sprLed.width() - gap * (ledCount - 1)) / ledCount;
  for (int i = 0; i < ledCount; i++) {
    uint16_t col = 0x18E3;
    if (atRevLimit) {
      col = flashOn ? COL_RED : 0x18E3;
    } else if (i < litCount) {
      col = (i < 5) ? COL_GREEN : (i < 10) ? COL_AMBER : COL_RED;
    }
    sprLed.fillRoundRect(i * (ledW + gap), 2, ledW, 26, 6, col);
  }
  sprLed.pushSprite(20, 6);
}

// --- 2. 上部タッチ操作バー ---
void drawControlBar() {
  auto& d = M5.Display;
  d.fillRect(0, 42, 1280, 44, COL_PANEL);
  d.drawFastHLine(0, 86, 1280, COL_BORDER);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextSize(1.0f);
  d.setTextDatum(middle_center);

  const char* tabs[3] = { "P1: RACE", "P2: G-BALL", "P3: PEDAL" };
  for (int i = 0; i < 3; i++) {
    int tx = 16 + i * 158;
    d.fillRoundRect(tx, 46, 150, 36, 6, (currentPage == i) ? 0xC800 : 0x18E3);
    d.setTextColor(COL_WHITE);
    d.drawString(tabs[i], tx + 75, 64);
  }

  d.fillRoundRect(498, 46, 46, 36, 6, 0x2124);
  d.setTextColor(COL_WHITE);
  d.drawString("-", 521, 64);

  char revBuf[24];
  snprintf(revBuf, sizeof(revBuf), "SHIFT %d", telem.revLimit);
  d.setTextColor(COL_CYAN);
  d.drawString(revBuf, 620, 64);

  d.fillRoundRect(696, 46, 46, 36, 6, 0x2124);
  d.setTextColor(COL_WHITE);
  d.drawString("+", 719, 64);

  char brtBuf[16];
  snprintf(brtBuf, sizeof(brtBuf), "BRT %d%%", BRIGHT_PCT[brightIdx]);
  d.fillRoundRect(758, 46, 128, 36, 6, 0x2945);
  d.setTextColor(COL_GOLD);
  d.drawString(brtBuf, 822, 64);

  d.fillRoundRect(900, 46, 170, 36, 6, bleClientConnected ? 0x03A0 : 0x2124);
  d.setTextColor(bleClientConnected ? COL_GREEN : COL_MUTED);
  d.drawString(bleClientConnected ? "BLE LINK" : "BLE WAIT", 985, 64);

  d.fillRoundRect(1086, 46, 178, 36, 6, isDemoMode ? 0x8200 : 0x0326);
  d.setTextColor(COL_WHITE);
  d.drawString(isDemoMode ? "MODE:DEMO" : "MODE:CAN", 1175, 64);

  headerDirty = false;
  prevBleConnected = bleClientConnected;
  prevDemoMode = isDemoMode;
}

// --- 3. 6段セクターボード (タップ:コース切替 / 1.5秒長押し:現コースBEST消去) ---
void drawSectorBoardArea(int x, int y, int bw, int bh) {
  auto& d = M5.Display;
  d.fillRoundRect(x, y, bw, bh, 12, COL_PANEL);
  d.drawRoundRect(x, y, bw, bh, 12, COL_BORDER);

  struct RowItem { const char* lbl; float sec; float delta; bool hasBadge; uint16_t lblCol; };
  RowItem rows[6] = {
    { "S3",   telem.s3,      telem.dS3,   true,  COL_WHITE },
    { "S2",   telem.s2,      telem.dS2,   true,  COL_WHITE },
    { "S1",   telem.s1,      telem.dS1,   true,  COL_CYAN  },
    { "LAST", telem.lastLap, telem.dLast, true,  COL_MUTED },
    { "BEST", telem.bestLap, 0.0f,        false, COL_GREEN },
    { "OPT.", telem.optLap,  0.0f,        false, COL_GOLD  }
  };

  int pad = 14;
  int rowH = (bh - pad * 2) / 6;
  int innerW = bw - 20;
  int sprH = (int)sprSecRow.height();
  int h = (rowH < sprH) ? rowH : sprH;
  int midY = h / 2;

  for (int i = 0; i < 6; i++) {
    int ry = y + pad + i * rowH;
    sprSecRow.fillSprite(COL_PANEL);

    if (i == 3) {
      sprSecRow.drawFastHLine(4, 2, innerW - 8, COL_BORDER);
    }

    sprSecRow.setFont(&fonts::FreeSansBold24pt7b);
    sprSecRow.setTextSize((i < 3) ? 1.05f : 0.80f);
    sprSecRow.setTextDatum(middle_left);
    sprSecRow.setTextColor(rows[i].lblCol);
    sprSecRow.drawString(rows[i].lbl, 8, midY);

    char tBuf[20];
    formatLapStr(rows[i].sec, tBuf, sizeof(tBuf));
    sprSecRow.setFont(&fonts::FreeSansBoldOblique24pt7b);
    sprSecRow.setTextSize(1.05f);
    sprSecRow.setTextDatum(middle_right);
    sprSecRow.setTextColor((i >= 4) ? rows[i].lblCol : COL_WHITE);
    sprSecRow.drawString(tBuf, (int)(innerW * 0.62f), midY);

    int bx = (int)(innerW * 0.645f);
    int bW = innerW - bx - 4;
    if (rows[i].hasBadge) {
      uint16_t badgeCol = (rows[i].delta <= 0.0f) ? COL_BLUE : COL_RED;
      sprSecRow.fillRoundRect(bx, 7, bW, h - 14, 10, badgeCol);
      char dBuf[16];
      snprintf(dBuf, sizeof(dBuf), "%+.2f", rows[i].delta);
      sprSecRow.setFont(&fonts::FreeSansBoldOblique24pt7b);
      sprSecRow.setTextSize(1.05f);
      sprSecRow.setTextDatum(middle_center);
      sprSecRow.setTextColor(COL_WHITE);
      sprSecRow.drawString(dBuf, bx + bW / 2 - 2, midY);
    } else {
      sprSecRow.setFont(&fonts::FreeSansBold18pt7b);
      sprSecRow.setTextSize(i == 4 ? 0.72f : 0.85f);
      sprSecRow.setTextDatum(middle_center);
      sprSecRow.setTextColor(i == 4 ? COL_CYAN : COL_MUTED);
      sprSecRow.drawString(i == 4 ? CIRCUITS[circuitIdx].name : "IDEAL", bx + bW / 2, midY);
    }

    sprSecRow.pushSprite(x + 10, ry);
  }
  sectorsDirty = false;
}

// --- 4. 上段左：LAP数枠 ---
void updateLapCountBox(int x, int y, bool force) {
  if (!force && telem.lap == prevLapCountDrawn) return;
  prevLapCountDrawn = telem.lap;

  int w = (int)sprLapCount.width();
  int h = (int)sprLapCount.height();
  sprLapCount.fillSprite(0x1860);
  sprLapCount.drawRoundRect(0, 0, w, h, 12, COL_AMBER);
  sprLapCount.drawRoundRect(1, 1, w - 2, h - 2, 11, COL_AMBER);

  sprLapCount.setFont(&fonts::FreeSansBold12pt7b);
  sprLapCount.setTextSize(0.95f);
  sprLapCount.setTextDatum(top_center);
  sprLapCount.setTextColor(COL_AMBER);
  sprLapCount.drawString("LAP", w / 2, 10);

  float numScale = (telem.lap >= 10) ? 2.0f : 2.55f;
  sprLapCount.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprLapCount.setTextSize(numScale);
  sprLapCount.setTextDatum(middle_center);
  sprLapCount.setTextColor(COL_AMBER);
  sprLapCount.drawNumber(telem.lap, w / 2 - 4, 88);

  sprLapCount.setFont(&fonts::FreeSansBold9pt7b);
  sprLapCount.setTextSize(0.95f);
  sprLapCount.setTextDatum(bottom_center);
  sprLapCount.setTextColor(COL_MUTED);
  sprLapCount.drawString("TAP: RESET 0", w / 2, h - 8);

  sprLapCount.pushSprite(x, y);
}

// --- 5. 上段右：スピード & RPM枠 ---
void updateSpeedBox(int x, int y, bool force) {
  int spdInt = (int)roundf(telem.spd);
  int rpmRound = (telem.rpm / 50) * 50;
  if (!force && spdInt == prevSpdDrawn && rpmRound == prevRpmDrawn) return;
  prevSpdDrawn = spdInt;
  prevRpmDrawn = rpmRound;

  int w = (int)sprSpeed.width();
  int h = (int)sprSpeed.height();
  sprSpeed.fillSprite(COL_PANEL);
  sprSpeed.drawRoundRect(0, 0, w, h, 12, COL_BORDER);

  sprSpeed.setFont(&fonts::FreeSansBold12pt7b);
  sprSpeed.setTextSize(0.95f);
  sprSpeed.setTextDatum(top_left);
  sprSpeed.setTextColor(COL_MUTED);
  sprSpeed.drawString("SPEED", 16, 10);

  char rpmBuf[24];
  snprintf(rpmBuf, sizeof(rpmBuf), "%d rpm", rpmRound);
  sprSpeed.setTextDatum(top_right);
  sprSpeed.setTextColor(telem.rpm >= telem.revLimit ? COL_RED : COL_GOLD);
  sprSpeed.drawString(rpmBuf, w - 16, 10);

  sprSpeed.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprSpeed.setTextSize(2.35f);
  sprSpeed.setTextDatum(middle_right);
  sprSpeed.setTextColor(COL_WHITE);
  sprSpeed.drawNumber(spdInt, w - 92, 100);

  sprSpeed.setFont(&fonts::FreeSansBold12pt7b);
  sprSpeed.setTextSize(0.95f);
  sprSpeed.setTextDatum(bottom_right);
  sprSpeed.setTextColor(COL_MUTED);
  sprSpeed.drawString("km/h", w - 16, h - 16);

  sprSpeed.pushSprite(x, y);
}

// --- 6. 中段：LAPタイマー & BEST Δ枠 ---
void updateLapDeltaBox(int x, int y) {
  int w = (int)sprLap.width();
  int h = (int)sprLap.height();
  sprLap.fillSprite(COL_PANEL);
  sprLap.drawRoundRect(0, 0, w, h, 12, COL_BORDER);

  char headBuf[40];
  snprintf(headBuf, sizeof(headBuf), "LAP TIMER    //    %4dm", (int)telem.distM);
  sprLap.setFont(&fonts::FreeSansBold12pt7b);
  sprLap.setTextSize(1.05f);
  sprLap.setTextDatum(top_left);
  sprLap.setTextColor(COL_CYAN);
  sprLap.drawString(headBuf, 20, 14);

  char curBuf[20];
  formatLapStr(telem.lapTime, curBuf, sizeof(curBuf));
  sprLap.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprLap.setTextSize(2.15f);
  sprLap.setTextDatum(middle_left);
  sprLap.setTextColor(COL_WHITE);
  sprLap.drawString(curBuf, 18, 124);

  uint16_t dCol = (telem.liveDelta <= 0.0f) ? COL_BLUE : COL_RED;
  sprLap.fillRoundRect(410, 52, 198, 132, 12, dCol);
  sprLap.setFont(&fonts::FreeSansBold12pt7b);
  sprLap.setTextSize(0.9f);
  sprLap.setTextDatum(top_center);
  sprLap.setTextColor(COL_WHITE);
  sprLap.drawString("BEST DELTA", 509, 64);

  char dBuf[16];
  snprintf(dBuf, sizeof(dBuf), "%+.2f", telem.liveDelta);
  sprLap.setFont(&fonts::FreeSansBold24pt7b);
  sprLap.setTextSize(1.3f);
  sprLap.setTextDatum(middle_center);
  sprLap.setTextColor(COL_WHITE);
  sprLap.drawString(dBuf, 509, 132);

  sprLap.pushSprite(x, y);
}

// --- 7. 下段3連装カード (水温 / タイマー / 時計＆燃料) ---
void updateWaterTempCard(int x, int y, bool force) {
  if (!force && telem.wtemp == prevWtempDrawn) return;
  prevWtempDrawn = telem.wtemp;

  int w = (int)sprCard.width();
  int h = (int)sprCard.height();
  bool isHot  = (telem.wtemp >= 100);
  bool isWarm = (telem.wtemp >= 95);
  uint16_t bgCol  = isHot ? COL_RED_DARK : COL_PANEL;
  uint16_t brdCol = isHot ? COL_RED : (isWarm ? COL_AMBER : COL_BORDER);
  uint16_t valCol = isHot ? COL_WHITE : (isWarm ? COL_AMBER : COL_EMERALD);

  sprCard.fillSprite(bgCol);
  sprCard.drawRoundRect(0, 0, w, h, 12, brdCol);

  sprCard.setFont(&fonts::FreeSansBold12pt7b);
  sprCard.setTextSize(0.95f);
  sprCard.setTextDatum(top_center);
  sprCard.setTextColor(COL_MUTED);
  sprCard.drawString("WATER TEMP", w / 2, 14);

  char wBuf[16];
  snprintf(wBuf, sizeof(wBuf), "%d", telem.wtemp);
  sprCard.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprCard.setTextSize(1.9f);
  sprCard.setTextDatum(middle_center);
  sprCard.setTextColor(valCol);
  sprCard.drawString(wBuf, w / 2 - 4, 112);

  sprCard.setFont(&fonts::FreeSansBold12pt7b);
  sprCard.setTextSize(0.95f);
  sprCard.setTextDatum(bottom_center);
  sprCard.setTextColor(isHot ? COL_GOLD : COL_MUTED);
  sprCard.drawString(isHot ? "HIGH TEMP!" : "deg C", w / 2, h - 12);

  sprCard.pushSprite(x, y);
}

void updateTimerCard(int x, int y, bool force) {
  int w = (int)sprCard.width();
  int h = (int)sprCard.height();

  if (timerAlarmActive) {
    bool flash = ((millis() / 300) % 2 == 0);
    if (!force && prevTimerSec == -2 && flash == prevAlarmFlash) return;
    prevTimerSec   = -2;
    prevAlarmFlash = flash;

    sprCard.fillSprite(flash ? COL_RED : COL_BG);
    sprCard.drawRoundRect(0, 0, w, h, 12, COL_RED);

    sprCard.setFont(&fonts::FreeSansBold12pt7b);
    sprCard.setTextSize(0.95f);
    sprCard.setTextDatum(top_center);
    sprCard.setTextColor(flash ? COL_WHITE : COL_RED);
    sprCard.drawString("TIME UP!", w / 2, 14);

    sprCard.setFont(&fonts::FreeSansBold24pt7b);
    sprCard.setTextSize(1.15f);
    sprCard.setTextDatum(middle_center);
    sprCard.setTextColor(flash ? COL_WHITE : COL_RED);
    sprCard.drawString("00:00", w / 2, 108);

    sprCard.setFont(&fonts::FreeSansBold9pt7b);
    sprCard.setTextSize(1.0f);
    sprCard.setTextDatum(bottom_center);
    sprCard.setTextColor(COL_GOLD);
    sprCard.drawString("TAP TO STOP!", w / 2, h - 12);

    sprCard.pushSprite(x, y);
    return;
  }

  if (!timerRunning && timerPresetIdx == 0) {
    if (!force && prevTimerSec == -3) return;
    prevTimerSec = -3;

    sprCard.fillSprite(COL_PANEL);
    sprCard.drawRoundRect(0, 0, w, h, 12, COL_BORDER);

    sprCard.setFont(&fonts::FreeSansBold12pt7b);
    sprCard.setTextSize(0.95f);
    sprCard.setTextDatum(top_center);
    sprCard.setTextColor(COL_MUTED);
    sprCard.drawString("TIMER", w / 2, 14);

    sprCard.setFont(&fonts::FreeSansBold24pt7b);
    sprCard.setTextSize(1.15f);
    sprCard.setTextDatum(middle_center);
    sprCard.setTextColor(COL_MUTED);
    sprCard.drawString("OFF", w / 2, 108);

    sprCard.setFont(&fonts::FreeSansBold9pt7b);
    sprCard.setTextSize(1.0f);
    sprCard.setTextDatum(bottom_center);
    sprCard.setTextColor(COL_MUTED);
    sprCard.drawString("TAP: 10M START", w / 2, h - 12);

    sprCard.pushSprite(x, y);
    return;
  }

  int remTotal = (int)ceilf(sessionRemainSec);
  if (!force && remTotal == prevTimerSec) return;
  prevTimerSec = remTotal;

  int remM = remTotal / 60;
  int remS = remTotal % 60;
  bool isLow = (remTotal <= 120);

  sprCard.fillSprite(isLow ? 0x3800 : COL_PANEL);
  sprCard.drawRoundRect(0, 0, w, h, 12, isLow ? COL_RED : COL_BORDER);

  sprCard.setFont(&fonts::FreeSansBold12pt7b);
  sprCard.setTextSize(0.95f);
  sprCard.setTextDatum(top_center);
  sprCard.setTextColor(COL_GOLD);
  sprCard.drawString("TIMER", w / 2, 14);

  char tBuf[16];
  snprintf(tBuf, sizeof(tBuf), "%02d:%02d", remM, remS);
  sprCard.setFont(&fonts::FreeSansBold24pt7b);
  sprCard.setTextSize(1.15f);
  sprCard.setTextDatum(middle_center);
  sprCard.setTextColor(isLow ? COL_RED : COL_GOLD);
  sprCard.drawString(tBuf, w / 2, 108);

  char subBuf[24];
  snprintf(subBuf, sizeof(subBuf), "%dM (TAP:NEXT)", TIMER_PRESETS_MIN[timerPresetIdx]);
  sprCard.setFont(&fonts::FreeSansBold9pt7b);
  sprCard.setTextSize(1.0f);
  sprCard.setTextDatum(bottom_center);
  sprCard.setTextColor(COL_MUTED);
  sprCard.drawString(subBuf, w / 2, h - 12);

  sprCard.pushSprite(x, y);
}

void updateClockCard(int x, int y, bool force) {
  int hh, mm, ss;
  getCurrentClockHMS(hh, mm, ss);
  if (!force && ss == prevClockSec && telem.fuel == prevFuelDrawn) return;
  prevClockSec  = ss;
  prevFuelDrawn = telem.fuel;

  int w = (int)sprCard.width();
  int h = (int)sprCard.height();
  sprCard.fillSprite(COL_BG);

  sprCard.fillRoundRect(0, 0, w, 100, 10, COL_PANEL);
  sprCard.drawRoundRect(0, 0, w, 100, 10, COL_BORDER);

  sprCard.setFont(&fonts::FreeSansBold9pt7b);
  sprCard.setTextSize(1.0f);
  sprCard.setTextDatum(top_left);
  sprCard.setTextColor(COL_CYAN);
  sprCard.drawString("CLOCK", 12, 8);

  char sBuf[12];
  snprintf(sBuf, sizeof(sBuf), ":%02d", ss);
  sprCard.setTextDatum(top_right);
  sprCard.setTextColor(COL_MUTED);
  sprCard.drawString(sBuf, w - 12, 8);

  char hmBuf[12];
  snprintf(hmBuf, sizeof(hmBuf), "%02d:%02d", hh, mm);
  sprCard.setFont(&fonts::FreeSansBold24pt7b);
  sprCard.setTextSize(1.05f);
  sprCard.setTextDatum(middle_center);
  sprCard.setTextColor(COL_WHITE);
  sprCard.drawString(hmBuf, w / 2, 62);

  int fy = 108;
  int fh = h - fy;
  bool isLowFuel = (telem.fuel <= 20);
  uint16_t fBg  = isLowFuel ? COL_RED_DARK : COL_PANEL;
  uint16_t fBrd = isLowFuel ? COL_RED : COL_BORDER;
  uint16_t fCol = isLowFuel ? COL_WHITE : (telem.fuel <= 35 ? COL_AMBER : COL_EMERALD);

  sprCard.fillRoundRect(0, fy, w, fh, 10, fBg);
  sprCard.drawRoundRect(0, fy, w, fh, 10, fBrd);

  sprCard.setFont(&fonts::FreeSansBold9pt7b);
  sprCard.setTextSize(1.0f);
  sprCard.setTextDatum(top_left);
  sprCard.setTextColor(isLowFuel ? COL_GOLD : COL_MUTED);
  sprCard.drawString(isLowFuel ? "LOW FUEL!" : "FUEL", 12, fy + 8);

  char fBuf[12];
  snprintf(fBuf, sizeof(fBuf), "%d%%", telem.fuel);
  sprCard.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprCard.setTextSize(1.05f);
  sprCard.setTextDatum(middle_center);
  sprCard.setTextColor(fCol);
  sprCard.drawString(fBuf, w / 2 - 2, fy + 52);

  int barX = 14, barY = fy + fh - 16, barW = w - 28, barH = 8;
  sprCard.fillRoundRect(barX, barY, barW, barH, 4, 0x18E3);
  int fillW = (barW * constrain(telem.fuel, 0, 100)) / 100;
  if (fillW > 0) {
    sprCard.fillRoundRect(barX, barY, fillW, barH, 4, isLowFuel ? COL_RED : fCol);
  }

  sprCard.pushSprite(x, y);
}

// --- 8. Gボール描画共通ヘルパー ---
void drawGBallToCanvas(M5Canvas& g, int cx, int cy, int radius, bool showSteer) {
  g.fillCircle(cx, cy, radius, 0x0841);
  g.drawCircle(cx, cy, radius, COL_BORDER);
  g.drawCircle(cx, cy, (int)(radius * 0.66f), 0x2965);
  g.drawCircle(cx, cy, (int)(radius * 0.33f), 0x2104);
  g.drawFastHLine(cx - radius, cy, radius * 2, 0x2965);
  g.drawFastVLine(cx, cy - radius, radius * 2, 0x2965);

  for (int i = 0; i < G_HIST_LEN; i++) {
    int idx = (gHistIdx + i) % G_HIST_LEN;
    int px = cx + (int)(constrain(gHistY[idx] / 1.5f, -1.0f, 1.0f) * (radius - 10));
    int py = cy - (int)(constrain(gHistX[idx] / 1.5f, -1.0f, 1.0f) * (radius - 10));
    g.fillCircle(px, py, (i > G_HIST_LEN - 5) ? 4 : 2, 0x7BEF);
  }

  int bx = cx + (int)(constrain(telem.gy / 1.5f, -1.0f, 1.0f) * (radius - 10));
  int by = cy - (int)(constrain(telem.gx / 1.5f, -1.0f, 1.0f) * (radius - 10));
  g.fillCircle(bx, by, 9, COL_GOLD);
  g.drawCircle(bx, by, 10, COL_WHITE);

  float gTot = hypotf(telem.gx, telem.gy);
  char gBuf[28];
  if (showSteer) snprintf(gBuf, sizeof(gBuf), "%.2fG / %+.0f", gTot, telem.steer);
  else           snprintf(gBuf, sizeof(gBuf), "%.2f G", gTot);

  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextSize(1.0f);
  g.setTextDatum(bottom_center);
  g.setTextColor(COL_GOLD);
  g.drawString(gBuf, cx, cy + radius - 6);
}

// --- 9. P2：G-BALL ページ ---
void updatePage2Dynamic(bool force) {
  if (force || sectorsDirty) {
    drawSectorBoardArea(16, 96, 608, 610);
  }

  updateLapCountBox(640, 96, force);
  updateSpeedBox(862, 96, force);
  updateLapDeltaBox(640, 278);
  updateWaterTempCard(640, 496, force);
  updateTimerCard(848, 496, force);

  int gw = (int)sprGBall.width();
  int gh = (int)sprGBall.height();
  sprGBall.fillSprite(COL_PANEL);
  sprGBall.drawRoundRect(0, 0, gw, gh, 12, COL_BORDER);
  drawGBallToCanvas(sprGBall, gw / 2, 96, 82, false);

  char gAxis[28];
  snprintf(gAxis, sizeof(gAxis), "X%+.1f Y%+.1f", telem.gx, telem.gy);
  sprGBall.setFont(&fonts::FreeSansBold9pt7b);
  sprGBall.setTextSize(1.0f);
  sprGBall.setTextDatum(bottom_center);
  sprGBall.setTextColor(COL_CYAN);
  sprGBall.drawString(gAxis, gw / 2, gh - 10);
  sprGBall.pushSprite(1056, 496);
}

// --- 10. P3：STEER & PEDAL ページ ---
void updatePage3Dynamic(bool force) {
  sprP3Lap.fillSprite(COL_PANEL);
  sprP3Lap.drawRoundRect(0, 0, sprP3Lap.width(), sprP3Lap.height(), 12, COL_BORDER);
  char headBuf[36];
  snprintf(headBuf, sizeof(headBuf), "LAP TIMER  //  %4dm", (int)telem.distM);
  sprP3Lap.setFont(&fonts::FreeSansBold12pt7b);
  sprP3Lap.setTextSize(1.0f);
  sprP3Lap.setTextDatum(top_left);
  sprP3Lap.setTextColor(COL_CYAN);
  sprP3Lap.drawString(headBuf, 18, 10);

  char curBuf[20];
  formatLapStr(telem.lapTime, curBuf, sizeof(curBuf));
  sprP3Lap.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprP3Lap.setTextSize(1.7f);
  sprP3Lap.setTextDatum(middle_left);
  sprP3Lap.setTextColor(COL_WHITE);
  sprP3Lap.drawString(curBuf, 18, 86);

  uint16_t dCol = (telem.liveDelta <= 0.0f) ? COL_BLUE : COL_RED;
  sprP3Lap.fillRoundRect(402, 22, 206, 92, 10, dCol);
  char dBuf[16];
  snprintf(dBuf, sizeof(dBuf), "%+.2fs", telem.liveDelta);
  sprP3Lap.setFont(&fonts::FreeSansBold24pt7b);
  sprP3Lap.setTextSize(1.05f);
  sprP3Lap.setTextDatum(middle_center);
  sprP3Lap.setTextColor(COL_WHITE);
  sprP3Lap.drawString(dBuf, 505, 68);
  sprP3Lap.pushSprite(16, 96);

  int hh, mm, ss;
  getCurrentClockHMS(hh, mm, ss);
  int spdInt = (int)roundf(telem.spd);
  if (force || spdInt != prevP3Spd || ss != prevP3Sec || telem.lap != prevP3Lap) {
    prevP3Spd = spdInt;
    prevP3Sec = ss;
    prevP3Lap = telem.lap;

    sprP3Info.fillSprite(COL_PANEL);
    sprP3Info.drawRoundRect(0, 0, sprP3Info.width(), sprP3Info.height(), 12, COL_BORDER);

    char b1[32], b2[40];
    snprintf(b1, sizeof(b1), "LAP %d    %3d km/h", telem.lap, spdInt);
    snprintf(b2, sizeof(b2), "WT %dC | FUEL %d%% | %02d:%02d", telem.wtemp, telem.fuel, hh, mm);

    sprP3Info.setFont(&fonts::FreeSansBoldOblique24pt7b);
    sprP3Info.setTextSize(1.15f);
    sprP3Info.setTextDatum(top_center);
    sprP3Info.setTextColor(COL_AMBER);
    sprP3Info.drawString(b1, 304, 16);

    sprP3Info.setFont(&fonts::FreeSansBold18pt7b);
    sprP3Info.setTextSize(0.85f);
    sprP3Info.setTextDatum(bottom_center);
    sprP3Info.setTextColor(COL_WHITE);
    sprP3Info.drawString(b2, 304, 122);

    sprP3Info.pushSprite(656, 96);
  }

  sprSteer.fillSprite(COL_BG);
  int cx = 225, cy = 225, stR = 200;
  float rad = telem.steer * (M_PI / 180.0f);

  for (int dr = -10; dr <= 10; dr += 4) {
    sprSteer.drawCircle(cx, cy, stR + dr, 0x4A49);
  }
  float topAng = rad - M_PI / 2.0f;
  sprSteer.fillCircle(cx + (int)(cosf(topAng) * stR), cy + (int)(sinf(topAng) * stR), 15, COL_GOLD);

  float spokes[3] = { rad, rad + M_PI, rad + M_PI / 2.0f };
  for (int i = 0; i < 3; i++) {
    int x1 = cx + (int)(cosf(spokes[i]) * 128);
    int y1 = cy + (int)(sinf(spokes[i]) * 128);
    int x2 = cx + (int)(cosf(spokes[i]) * (stR - 10));
    int y2 = cy + (int)(sinf(spokes[i]) * (stR - 10));
    sprSteer.drawWideLine(x1, y1, x2, y2, 15, 0x5ACB);
  }
  drawGBallToCanvas(sprSteer, cx, cy, 128, true);
  sprSteer.pushSprite(110, 250);

  int pW = (int)sprPedal.width();
  int pH = (int)sprPedal.height();
  sprPedal.fillSprite(COL_PANEL);
  sprPedal.drawRoundRect(0, 0, pW, pH, 12, COL_RED);
  int brkH = (int)((pH - 12) * (constrain(telem.brk, 0, 100) / 100.0f));
  if (brkH > 0) sprPedal.fillRoundRect(6, pH - 6 - brkH, pW - 12, brkH, 8, COL_RED);
  int peakY = pH - 6 - (int)((pH - 12) * (brkPeak / 100.0f));
  sprPedal.fillRect(6, peakY, pW - 12, 5, COL_GOLD);

  sprPedal.setFont(&fonts::FreeSansBold18pt7b);
  sprPedal.setTextSize(1.0f);
  sprPedal.setTextDatum(top_center);
  sprPedal.setTextColor(COL_WHITE);
  sprPedal.drawString("BRAKE", pW / 2, 18);

  char bPct[16], bMpa[20];
  snprintf(bPct, sizeof(bPct), "%d%%", telem.brk);
  snprintf(bMpa, sizeof(bMpa), "%.1f MPa", telem.brk * 0.085f);
  sprPedal.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprPedal.setTextSize(1.5f);
  sprPedal.setTextDatum(middle_center);
  sprPedal.drawString(bPct, pW / 2, pH / 2 - 10);
  sprPedal.setFont(&fonts::FreeSansBold12pt7b);
  sprPedal.setTextSize(1.1f);
  sprPedal.setTextColor(COL_GOLD);
  sprPedal.drawString(bMpa, pW / 2, pH / 2 + 52);
  sprPedal.pushSprite(710, 248);

  sprPedal.fillSprite(COL_PANEL);
  sprPedal.drawRoundRect(0, 0, pW, pH, 12, COL_EMERALD);
  int thrH = (int)((pH - 12) * (constrain(telem.thr, 0, 100) / 100.0f));
  if (thrH > 0) sprPedal.fillRoundRect(6, pH - 6 - thrH, pW - 12, thrH, 8, COL_EMERALD);

  sprPedal.setFont(&fonts::FreeSansBold18pt7b);
  sprPedal.setTextSize(1.0f);
  sprPedal.setTextDatum(top_center);
  sprPedal.setTextColor(COL_WHITE);
  sprPedal.drawString("THROTTLE", pW / 2, 18);

  char tPct[16];
  snprintf(tPct, sizeof(tPct), "%d%%", telem.thr);
  sprPedal.setFont(&fonts::FreeSansBoldOblique24pt7b);
  sprPedal.setTextSize(1.5f);
  sprPedal.setTextDatum(middle_center);
  sprPedal.drawString(tPct, pW / 2, pH / 2);
  sprPedal.pushSprite(990, 248);
}

// --- メイン画面更新ディスパッチャ ---
void renderFrame() {
  M5.Display.startWrite();

  if (needFullRedraw) {
    M5.Display.fillScreen(COL_BG);
    drawControlBar();
    updateShiftLeds(true);
    prevLapCountDrawn = -1;
    prevSpdDrawn      = -1;
    prevRpmDrawn      = -1;
    prevWtempDrawn    = -999;
    prevTimerSec      = -1;
    prevClockSec      = -1;
    prevFuelDrawn     = -1;
    prevP3Spd         = -1;
    prevP3Sec         = -1;
    prevP3Lap         = -1;
    sectorsDirty      = true;
  } else {
    if (headerDirty || bleClientConnected != prevBleConnected || isDemoMode != prevDemoMode) {
      drawControlBar();
    }
    updateShiftLeds(false);
  }

  if (currentPage == 0) {
    updateLapCountBox(16, 96, needFullRedraw);
    updateSpeedBox(238, 96, needFullRedraw);
    updateLapDeltaBox(16, 278);
    updateWaterTempCard(16, 496, needFullRedraw);
    updateTimerCard(229, 496, needFullRedraw);
    updateClockCard(442, 496, needFullRedraw);
    if (needFullRedraw || sectorsDirty) {
      drawSectorBoardArea(656, 96, 608, 610);
    }
  } else if (currentPage == 1) {
    updatePage2Dynamic(needFullRedraw);
  } else {
    updatePage3Dynamic(needFullRedraw);
  }

  needFullRedraw = false;
  M5.Display.endWrite();
}

void resetLapCountToZero() {
  if (!isDemoMode) {
    lapOffset = rawCanLap;
  }
  telem.lap = 0;
  prevLapCountDrawn = -1;
  prevP3Lap = -1;
}

bool isInsideSectorBoard(int x, int y) {
  if (currentPage == 0 && x >= 656 && x <= 1264 && y >= 96 && y <= 706) return true;
  if (currentPage == 1 && x >= 16  && x <= 624  && y >= 96 && y <= 706) return true;
  return false;
}

// --- タッチ操作 ---
void handleTouchInput() {
  auto t = M5.Touch.getDetail();

  // セクターボード長押し継続チェック (1.5秒で現コースのBEST消去)
  if (secTouchActive) {
    if (t.isPressed()) {
      if (!secLongPressFired && (millis() - secTouchStartMs >= 1500)) {
        secLongPressFired = true;
        clearCircuitBests(circuitIdx);
      }
      return;
    } else {
      // 指を離したとき：長押し未発動ならコース切替
      secTouchActive = false;
      if (!secLongPressFired) {
        circuitIdx = (circuitIdx + 1) % 8;
        prefs.begin("38dash", false);
        prefs.putInt("trkIdx", circuitIdx);
        prefs.end();
        loadCircuitBests(circuitIdx);
        M5.Speaker.tone(2400, 35);
      }
      return;
    }
  }

  if (!t.wasPressed()) return;

  if (timerAlarmActive) {
    timerAlarmActive = false;
    timerRunning     = false;
    timerPresetIdx   = 0;
    sessionRemainSec = 0.0f;
    prevTimerSec     = -1;
    M5.Speaker.stop();
    M5.Speaker.tone(2400, 35);
    return;
  }

  int x = t.x, y = t.y;

  // セクターボードに触れたら長押し/短押し判定モードへ
  if (isInsideSectorBoard(x, y)) {
    secTouchActive    = true;
    secLongPressFired = false;
    secTouchStartMs   = millis();
    return;
  }

  bool isResetAction = false;

  auto cycleTimer = [&]() {
    timerPresetIdx = (timerPresetIdx + 1) % 5;
    if (timerPresetIdx == 0) {
      timerRunning     = false;
      sessionRemainSec = 0.0f;
    } else {
      sessionRemainSec = TIMER_PRESETS_MIN[timerPresetIdx] * 60.0f;
      timerRunning     = true;
    }
    timerAlarmActive = false;
    prevTimerSec     = -1;
  };

  if (y >= 36 && y <= 92) {
    if (x >= 16 && x < 166)       { currentPage = 0; needFullRedraw = true; }
    else if (x >= 174 && x < 324) { currentPage = 1; needFullRedraw = true; }
    else if (x >= 332 && x < 482) { currentPage = 2; needFullRedraw = true; }
    else if (x >= 494 && x < 548) {
      telem.revLimit -= 250;
      if (telem.revLimit < 5000) telem.revLimit = 5000;
      headerDirty = true;
    }
    else if (x >= 692 && x < 746) {
      telem.revLimit += 250;
      if (telem.revLimit > 8000) telem.revLimit = 8000;
      headerDirty = true;
    }
    else if (x >= 754 && x < 890) {
      brightIdx = (brightIdx + 1) % 6;
      M5.Display.setBrightness(BRIGHT_LEVELS[brightIdx]);
      headerDirty = true;
    }
    else if (x >= 1080) {
      isDemoMode = !isDemoMode;
      headerDirty = true;
    }
  }
  else if (currentPage == 0 && x >= 16 && x <= 224 && y >= 96 && y <= 264) {
    resetLapCountToZero();
    isResetAction = true;
  }
  else if (currentPage == 0 && x >= 229 && x <= 427 && y >= 496 && y <= 706) {
    cycleTimer();
  }
  else if (currentPage == 1 && x >= 640 && x <= 848 && y >= 96 && y <= 264) {
    resetLapCountToZero();
    isResetAction = true;
  }
  else if (currentPage == 1 && x >= 848 && x <= 1046 && y >= 496 && y <= 706) {
    cycleTimer();
  }
  else if (currentPage == 1 && x >= 1056 && x <= 1264 && y >= 496 && y <= 706) {
    calibrateGZero();
    isResetAction = true;
  }

  if (isResetAction) {
    M5.Speaker.tone(3000, 55);
  } else {
    M5.Speaker.tone(2400, 35);
  }
}

void sendTelemetryBLE25Hz() {
  if (!bleClientConnected || pTxCharacteristic == nullptr) return;
  char jsonBuf[384];
  snprintf(jsonBuf, sizeof(jsonBuf),
           "{\"spd\":%.1f,\"rpm\":%d,\"gear\":%d,\"thr\":%d,\"brk\":%d,"
           "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"steer\":%.1f,\"wtemp\":%d,\"fuel\":%d,"
           "\"lap\":%d,\"distM\":%.1f,\"lapTime\":%.2f,\"lastLap\":%.2f,"
           "\"lat\":%.5f,\"lon\":%.5f,\"trk\":\"%s\"}\n",
           telem.spd, telem.rpm, telem.gear, telem.thr, telem.brk,
           telem.gx, telem.gy, telem.vertG, telem.steer, telem.wtemp, telem.fuel,
           telem.lap, telem.distM, telem.lapTime, telem.lastLap,
           telem.lat, telem.lon, CIRCUITS[circuitIdx].name);
  pTxCharacteristic->setValue((uint8_t*)jsonBuf, strlen(jsonBuf));
  pTxCharacteristic->notify();
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  cfg.internal_spk = true;
  cfg.output_power = true;
  M5.begin(cfg);

  M5.Display.setRotation(3);
  M5.Display.setBrightness(BRIGHT_LEVELS[brightIdx]);

  auto& ioe = M5.getIOExpander(0);
  ioe.setDirection(1, true);
  ioe.digitalWrite(1, true);
  M5.Speaker.begin();
  M5.Speaker.setVolume(255);
  delay(150);

  showBootSplash();

  auto initSpr = [](M5Canvas& s, int w, int h) {
    s.setPsram(true);
    s.setColorDepth(16);
    s.createSprite(w, h);
  };
  initSpr(sprLed,      1240, 30);
  initSpr(sprLapCount, 208,  168);
  initSpr(sprSpeed,    402,  168);
  initSpr(sprLap,      624,  204);
  initSpr(sprCard,     198,  210);
  initSpr(sprSecRow,   588,  94);
  initSpr(sprGBall,    208,  210);
  initSpr(sprP3Lap,    624,  136);
  initSpr(sprP3Info,   608,  136);
  initSpr(sprSteer,    450,  450);
  initSpr(sprPedal,    240,  456);

  initClockFromCompileTimeIfNeeded();
  setupCAN();
  setupBLE();

  // 前回選択していたサーキットとベストを復元
  prefs.begin("38dash", true);
  circuitIdx = constrain(prefs.getInt("trkIdx", 0), 0, 7);
  prefs.end();
  loadCircuitBests(circuitIdx);

  lastLoopMs = millis();
}

void loop() {
  M5.update();
  uint32_t now = millis();
  float dtSec = (now - lastLoopMs) * 0.001f;
  if (dtSec > 0.5f) dtSec = 0.5f;
  lastLoopMs = now;

  handleTouchInput();
  pollCANMessages();
  updateIMUGForce();

  // 2秒ごとにCANステータスをシリアル出力
  if (now - lastCanStatMs >= 2000) {
    lastCanStatMs = now;
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
      Serial.printf("[CAN STAT] state:%d rx_q:%lu rx_err:%lu bus_err:%lu\n",
                    st.state, (unsigned long)st.msgs_to_rx,
                    (unsigned long)st.rx_error_counter, (unsigned long)st.bus_error_count);
    }
  }

  // 燃料残量の2秒間中央値フィルタ更新 (100msごと)
  if (!fuelBufInit) {
    for (int i = 0; i < FUEL_BUF_LEN; i++) fuelBuf[i] = rawFuelPct;
    fuelBufInit = true;
  }
  if (now - lastFuelSampMs >= 100) {
    lastFuelSampMs = now;
    if (isDemoMode) {
      rawFuelPct = 68 + (int)(sinf(demoPhase * 2.0f) * 3.0f);
    }
    fuelBuf[fuelBufIdx] = rawFuelPct;
    fuelBufIdx = (fuelBufIdx + 1) % FUEL_BUF_LEN;
    telem.fuel = calcFuelMedian();
  }

  // セッションタイマー更新 & 00:00アラーム
  if (timerRunning && sessionRemainSec > 0.0f) {
    sessionRemainSec -= dtSec;
    if (sessionRemainSec <= 0.0f) {
      sessionRemainSec = 0.0f;
      timerRunning     = false;
      timerAlarmActive = true;
      lastAlarmBeepMs  = 0;
      prevTimerSec     = -1;
    }
  }
  if (timerAlarmActive && (now - lastAlarmBeepMs >= 600)) {
    lastAlarmBeepMs = now;
    M5.Speaker.tone(2800, 220);
  }

  if (isDemoMode) {
    demoPhase += dtSec * 1.15f;
    float w = sinf(demoPhase);
    telem.brk   = (w < -0.25f) ? constrain((int)((-w - 0.25f) * 145), 0, 100) : 0;
    telem.thr   = (telem.brk == 0) ? constrain((int)((w + 0.30f) * 90), 0, 100) : 0;
    telem.spd   = 138.0f + w * 62.0f;
    telem.rpm   = 3600 + (int)(fabsf(sinf(demoPhase * 1.5f)) * 3900.0f);
    telem.gear  = (telem.spd > 172) ? 5 : (telem.spd > 132) ? 4 : (telem.spd > 92) ? 3 : 2;
    telem.gx    = (telem.brk > 0) ? -telem.brk * 0.012f : telem.thr * 0.0045f;
    telem.gy    = cosf(demoPhase * 0.7f) * 1.25f;
    telem.steer = cosf(demoPhase * 0.7f) * 110.0f;
    telem.liveDelta = sinf(demoPhase * 0.35f) * 0.45f;

    telem.distM   += (telem.spd / 3.6f) * dtSec;
    telem.lapTime += dtSec;
    if (telem.distM >= CIRCUITS[circuitIdx].totalDist) {
      telem.lastLap = telem.lapTime;
      if (telem.bestLap <= 0.0f || telem.lastLap < telem.bestLap) telem.bestLap = telem.lastLap;
      telem.dLast = telem.lastLap - telem.bestLap;
      telem.lap++;
      telem.distM = 0.0f;
      telem.lapTime = 0.0f;
      sectorsDirty = true;
    }
  } else if (telem.spd > 1.0f) {
    // CAN実車モード：車速から走行距離を自動積分し、公式セクター距離でS1/S2を計測
    telem.distM += (telem.spd / 3.6f) * dtSec;
    const auto& trk = CIRCUITS[circuitIdx];

    if (!s1Recorded && telem.distM >= trk.s1Dist && telem.lapTime > 1.0f) {
      telem.s1 = telem.lapTime;
      if (bestS1 <= 0.0f || telem.s1 < bestS1) {
        bestS1 = telem.s1;
        saveCircuitBests(circuitIdx);
      }
      telem.dS1 = telem.s1 - bestS1;
      if (bestS1 > 0 && bestS2 > 0 && bestS3 > 0) telem.optLap = bestS1 + bestS2 + bestS3;
      s1Recorded   = true;
      sectorsDirty = true;
    }

    if (!s2Recorded && telem.distM >= trk.s2Dist && s1Recorded) {
      telem.s2 = telem.lapTime - telem.s1;
      if (bestS2 <= 0.0f || telem.s2 < bestS2) {
        bestS2 = telem.s2;
        saveCircuitBests(circuitIdx);
      }
      telem.dS2 = telem.s2 - bestS2;
      if (bestS1 > 0 && bestS2 > 0 && bestS3 > 0) telem.optLap = bestS1 + bestS2 + bestS3;
      s2Recorded   = true;
      sectorsDirty = true;
    }
  }

  if (telem.brk >= brkPeak) {
    brkPeak = telem.brk;
    brkPeakHoldMs = now;
  } else if (now - brkPeakHoldMs > 1200) {
    int decayed = brkPeak - 2;
    brkPeak = (telem.brk > decayed) ? telem.brk : decayed;
  }

  if (now - lastGHistMs >= 66) {
    lastGHistMs = now;
    gHistX[gHistIdx] = telem.gx;
    gHistY[gHistIdx] = telem.gy;
    gHistIdx = (gHistIdx + 1) % G_HIST_LEN;
  }

  if (now - lastFrameMs >= 20) {
    lastFrameMs = now;
    renderFrame();
  }

  if (now - lastBleMs >= 40) {
    lastBleMs = now;
    sendTelemetryBLE25Hz();
  }
}

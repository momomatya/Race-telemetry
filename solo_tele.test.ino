/**
 * 38Racing - M5Stack Tab5 Racing & Street Dashboard (HTML Preview Full Reproduction)
 * ----------------------------------------------------------------------------------
 * [Target Hardware]
 * - M5Stack Tab5 (ESP32-P4, 1280x720 IPS, PSRAM Double-Buffered)
 * - Rotation : setRotation(3) -> 1280x720 Landscape
 * - SD_MMC   : 4-bit SDIO (CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42)
 *              Optional VLW fonts in /fonts/ (auto-fallback to built-in Italic + efontJA)
 * - CAN Bus  : TWAI 1Mbps (TX=53, RX=54 / AiM Solo 2 DL EXP)
 * - BLE NUS  : "M5-38RACING-TELEM" (4-Stage Status Lamp + Track Sync)
 *
 * [UI Features - 1:1 Match with HTML Preview]
 * - Full-width 15-LED Shift Bar + Sliding Pill Tabs + Collapsible P4 Header
 * - Radial Explode / Converge Page Transitions (Cards + Contents move together!)
 * - Left/Right Swipe + Tap Controls
 * - P1: RACE   (108px LAP, 134px SPEED + RPM-only, 76px TIMER, 68px 3D BEST Δ, 6-Row Sector Board)
 * - P2: G-BALL (Timing Summary + 400px Smooth G-Force Monitor w/ Trail & Tap Zero-Cal)
 * - P3: PEDAL  (Compact Top Bar w/ 74px SPEED, Thin Steering Ring + Large G-Ball, Extra-Wide Brake/Throttle)
 * - P4: STREET (Frameless Left Metallic Menu, Concentric Water/Fuel Arcs [R=312/334],
 *               Right 620px White Tachometer w/ Dynamic REV Redline [3000-8500 RPM] & 112px Digital Speed)
 */

#include <M5Unified.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <driver/twai.h>
#include <math.h>
#include <string.h>

// ESP32-P4 loopTaskのスタック容量を 8KB -> 32KB へ拡張（特大VLWフォント描画時のスタック保護エラー防止）
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// --- SD_MMC 4-bit SDIO ピン設定 (M5Stack Tab5) ---
#define TAB5_SD_CLK 43
#define TAB5_SD_CMD 44
#define TAB5_SD_D0  39
#define TAB5_SD_D1  40
#define TAB5_SD_D2  41
#define TAB5_SD_D3  42

// --- CAN (TWAI) ピン設定 (Grove Port.A) ---
#define CAN_TX_PIN GPIO_NUM_53
#define CAN_RX_PIN GPIO_NUM_54

// --- カラーパレット (RGB565: HTMLプレビュー準拠) ---
static const uint16_t COL_BLACK       = 0x0000;
static const uint16_t COL_BG          = 0x0862; // #0b0e14
static const uint16_t COL_CARBON_A    = 0x10A3; // 斜めカーボン線
static const uint16_t COL_CARD_TOP    = 0x18E5; // #161d29
static const uint16_t COL_CARD_BOT    = 0x0883; // #0c1018
static const uint16_t COL_CARD_DARK   = 0x0842; // #020617
static const uint16_t COL_BORDER      = 0x39E9; // メタリック枠
static const uint16_t COL_SLATE_700   = 0x31C9;
static const uint16_t COL_SLATE_400   = 0x9517; // #94a3b8
static const uint16_t COL_SLATE_300   = 0xCE79; // #cbd5e1
static const uint16_t COL_WHITE       = 0xFFFF;
static const uint16_t COL_OFFWHITE    = 0xF7DF; // #f8fafc
static const uint16_t COL_CYAN        = 0x269D; // #22d3ee
static const uint16_t COL_CYAN_DARK   = 0x09C9; // #083344
static const uint16_t COL_AMBER       = 0xFDE4; // #fbbf24
static const uint16_t COL_AMBER_LIGHT = 0xFEEA; // #fcd34d
static const uint16_t COL_AMBER_BG    = 0x28E1; // #2d1c0a
static const uint16_t COL_AMBER_BRD   = 0xF4E1; // #f59e0b
static const uint16_t COL_EMERALD     = 0x3693; // #34d399
static const uint16_t COL_EMERALD_BG  = 0x0964; // #064e3b
static const uint16_t COL_EMERALD_BRD = 0x154A; // #10b981
static const uint16_t COL_BLUE        = 0x231D; // #2563eb
static const uint16_t COL_BLUE_DARK   = 0x1A7B; // #1d4ed8
static const uint16_t COL_BLUE_LIGHT  = 0x653F; // #60a5fa
static const uint16_t COL_ROSE        = 0xE0E9; // #e11d48
static const uint16_t COL_ROSE_LIGHT  = 0xFB90; // #fb7185
static const uint16_t COL_GREEN       = 0x262B; // #22c55e
static const uint16_t COL_YELLOW      = 0xED81; // #eab308
static const uint16_t COL_RED         = 0xEA28; // #ef4444
static const uint16_t COL_RED_BRIGHT  = 0xF806; // #ff0033
static const uint16_t COL_LED_OFF     = 0x1906; // #1a2230
static const uint16_t COL_TRANS_KEY   = 0xF81F; // スプライト透過キー

// --- PSRAM スプライト（ダブルバッファ＆静的キャッシュ） ---
M5Canvas sprFrame(&M5.Display);
M5Canvas sprCarbonBg(&M5.Display);
M5Canvas sprTachoBg(&M5.Display);
M5Canvas sprLedBar(&M5.Display);       // 1280x42 シフトLED専用高速バッファ
M5Canvas sprWidget(&M5.Display);       // 620x620 部分更新用高速バッファ
M5Canvas sprPageCache[4] = {           // 4ページ分の事前レンダリングキャッシュ（遷移フリーズ0ms化）
  M5Canvas(&M5.Display),
  M5Canvas(&M5.Display),
  M5Canvas(&M5.Display),
  M5Canvas(&M5.Display)
};
bool pageCacheDirty[4] = {true, true, true, true};

// --- 部分更新・ダーティ管理 ---
bool staticDirty     = true;
bool prevRevFlash    = false;
int  lastDrawnSec    = -1;
int  lastDrawnLedCnt = -99;
int  lastP1Speed     = -1;
int  lastP1Rpm       = -1;
int  lastP1Lap10     = -1;
int  lastP1Delta100  = -9999;
int  lastP1DeltaFast = -1; // 画面上のBEST Δカード色 (-1:未描画, 0:赤, 1:青)
int  lastP4Speed     = -1;
int  lastP4Gear      = -99;
int  lastP4RpmStep   = -1;
float lastP4NeedleDeg = -999.0f;
int  lastP3BrakeFill = -1;
int  lastP3BrakePeak = -1;
int  lastP3ThrFill   = -1;
int  lastP3BrakePct  = -1;
int  lastP3ThrPct    = -1;
int  lastP3Speed     = -1;
int  lastP3Gear      = -99;
float lastP3SteerDeg = -999.0f;
uint32_t lastRenderMs = 0;

// --- SDカード VLWフォント状態（PSRAMに一括ロードして爆速描画） ---
bool sdMounted = false;
bool hasVlwNumBig   = false; // /fonts/BarlowBI_120.vlw
bool hasVlwNumMed   = false; // /fonts/BarlowBI_68.vlw
bool hasVlwNumSm    = false; // /fonts/BarlowBI_44.vlw
bool hasVlwUi       = false; // /fonts/BarlowBI_28.vlw

uint8_t* vlwBuf120 = nullptr;
uint8_t* vlwBuf68  = nullptr;
uint8_t* vlwBuf44  = nullptr;
uint8_t* vlwBuf28  = nullptr;

// 全キャンバス共通のVLWロード状態キャッシュ（不要なunload/loadの嵐を完全防止）
struct CanvasFontSlot {
  M5Canvas*      canvas;
  const uint8_t* vlw;
};
static CanvasFontSlot gFontSlots[12] = {};

static void ensureCanvasVlw(M5Canvas& c, const uint8_t* vlw) {
  int freeIdx = -1;
  for (int i = 0; i < 12; i++) {
    if (gFontSlots[i].canvas == &c) {
      if (gFontSlots[i].vlw == vlw) return; // 既に同じVLWがロード済みなら何もしない
      c.unloadFont();
      if (vlw) c.loadFont(vlw);
      gFontSlots[i].vlw = vlw;
      return;
    }
    if (freeIdx < 0 && gFontSlots[i].canvas == nullptr) freeIdx = i;
  }
  c.unloadFont();
  if (vlw) c.loadFont(vlw);
  if (freeIdx >= 0) {
    gFontSlots[freeIdx].canvas = &c;
    gFontSlots[freeIdx].vlw    = vlw;
  }
}

// SDカードのVLWファイルをPSRAMへ丸ごと読み込むヘルパー
static uint8_t* loadVlwFromPath(const char* fullPath) {
  File f = SD_MMC.open(fullPath, FILE_READ);
  if (!f) return nullptr;
  size_t sz = f.size();
  if (sz < 32) { f.close(); return nullptr; }
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) {
    Serial.printf("[FONT] ps_malloc failed (%u bytes): %s\n", (unsigned)sz, fullPath);
    f.close();
    return nullptr;
  }
  size_t readLen = f.read(buf, sz);
  f.close();
  if (readLen != sz) {
    free(buf);
    return nullptr;
  }
  Serial.printf("[FONT] Loaded to PSRAM: %s (%u bytes)\n", fullPath, (unsigned)sz);
  return buf;
}

// 大文字小文字を無視して拡張子 .vlw か判定
static bool endsWithVlw(const char* name) {
  size_t n = strlen(name);
  if (n < 4) return false;
  const char* ext = name + (n - 4);
  return (ext[0] == '.') &&
         (ext[1] == 'v' || ext[1] == 'V') &&
         (ext[2] == 'l' || ext[2] == 'L') &&
         (ext[3] == 'w' || ext[3] == 'W');
}

// /fonts, /font, / を自動走査して .vlw ファイルを名前・サイズ順で自動ロード
static void autoLoadVlwFonts() {
  static const char* dirs[3] = {"/fonts", "/font", "/"};
  static char   foundPaths[16][128];
  static size_t foundSizes[16];
  int    foundCnt = 0;

  for (int d = 0; d < 3; d++) {
    File dir = SD_MMC.open(dirs[d]);
    if (!dir || !dir.isDirectory()) continue;
    Serial.printf("[SD LS] %s:\n", dirs[d]);
    File entry = dir.openNextFile();
    int listed = 0;
    while (entry && listed < 30) {
      const char* ename = entry.name();
      size_t esz = entry.size();
      Serial.printf("  - %s (%u bytes)\n", ename, (unsigned)esz);
      if (!entry.isDirectory() && endsWithVlw(ename) && foundCnt < 16) {
        if (ename[0] == '/') {
          snprintf(foundPaths[foundCnt], sizeof(foundPaths[0]), "%s", ename);
        } else if (strcmp(dirs[d], "/") == 0) {
          snprintf(foundPaths[foundCnt], sizeof(foundPaths[0]), "/%s", ename);
        } else {
          snprintf(foundPaths[foundCnt], sizeof(foundPaths[0]), "%s/%s", dirs[d], ename);
        }
        foundSizes[foundCnt] = esz;
        foundCnt++;
      }
      entry = dir.openNextFile();
      listed++;
    }
    dir.close();
  }

  if (foundCnt == 0) {
    Serial.println("[FONT] No .vlw files found in /fonts, /font, or /");
    return;
  }

  // ファイルサイズ降順にソート（大きいフォントほどファイルサイズが大きい）
  for (int i = 0; i < foundCnt - 1; i++) {
    for (int j = i + 1; j < foundCnt; j++) {
      if (foundSizes[j] > foundSizes[i]) {
        size_t ts = foundSizes[i]; foundSizes[i] = foundSizes[j]; foundSizes[j] = ts;
        static char tp[128]; memcpy(tp, foundPaths[i], 128); memcpy(foundPaths[i], foundPaths[j], 128); memcpy(foundPaths[j], tp, 128);
      }
    }
  }

  // 1) ファイル名に含まれるサイズ数字 (120 / 68 / 44 / 28) で優先マッチ
  auto pickByToken = [&](const char* tok) -> int {
    for (int i = 0; i < foundCnt; i++) {
      if (strstr(foundPaths[i], tok) != nullptr) return i;
    }
    return -1;
  };
  int idx120 = pickByToken("120");
  int idx68  = pickByToken("68");
  int idx44  = pickByToken("44");
  int idx28  = pickByToken("28");

  // 2) 数字が一致しなかったスロットはサイズ順のファイルから自動割り当て
  if (idx120 < 0) idx120 = 0;
  if (idx68  < 0) idx68  = (foundCnt > 1) ? 1 : 0;
  if (idx44  < 0) idx44  = (foundCnt > 2) ? 2 : idx68;
  if (idx28  < 0) idx28  = foundCnt - 1;

  vlwBuf120 = loadVlwFromPath(foundPaths[idx120]);
  vlwBuf68  = (idx68 == idx120) ? vlwBuf120 : loadVlwFromPath(foundPaths[idx68]);
  vlwBuf44  = (idx44 == idx68)  ? vlwBuf68  : loadVlwFromPath(foundPaths[idx44]);
  vlwBuf28  = (idx28 == idx44)  ? vlwBuf44  : loadVlwFromPath(foundPaths[idx28]);

  hasVlwNumBig = (vlwBuf120 != nullptr);
  hasVlwNumMed = (vlwBuf68  != nullptr);
  hasVlwNumSm  = (vlwBuf44  != nullptr);
  hasVlwUi     = (vlwBuf28  != nullptr);
}

// --- 画面＆モード状態 ---
bool isDemoMode = false; // デフォルトは実車CAN待機モード（右上MODEボタンでDEMO切替可）
bool canOk      = false;
int  currentPage = 0;    // 0:P1 RACE, 1:P2 G-BALL, 2:P3 PEDAL, 3:P4 STREET

// --- 4段階 安心ランプ状態 (0:BLE WAIT, 1:PC WAIT, 2:ALL OK, 3:● REC ON) ---
int lampState = 2; // 初期 ALL OK

// --- ヘッダー格納アニメーション (P4時に0.0へ、P1-P3時に1.0へ) ---
float headerVis = 1.0f;

// --- タブ スライドピル アニメーション ---
float tabPillX = 14.0f;
float tabPillW = 112.0f;

// --- P4 左メニュー状態 ---
bool  p4MenuOpen        = true;
float p4MenuVis         = 1.0f;
int   p4SelectedRow     = 0;

// --- 分解・収束トランジション ---
bool     isTransitioning   = false;
int      transFromPage     = 0;
int      transToPage       = 0;
uint32_t transStartMs      = 0;
uint32_t transPhaseStartMs = 0;
int      transFrameCnt     = 0;
int      transLeaveFrames  = 0;
int      transEnterFrames  = 0;
int      transPhase        = 0; // 0:退場(中央→四方へ分散), 1:入場(四方→中央へ収束)
static const uint32_t TRANS_LEAVE_MS = 240;
static const uint32_t TRANS_ENTER_MS = 300;
bool     touchTracking = false;
bool     touchConsumed = false;
int      touchStartX   = 0;
int      touchStartY   = 0;
int      touchLastX    = 0;
int      touchLastY    = 0;
bool     touchMoved    = false;

// --- 設定 (NVS保存) ---
Preferences prefs;
int shiftLimitRpm = 7000;
int brightnessPct = 85;
int circuitIdx    = 0;

struct CircuitInfo {
  const char* name;
  float lengthM;
};
static const CircuitInfo CIRCUITS[] = {
  {"SODEGAURA", 2436.0f},
  {"FSW",       4563.0f},
  {"TC2000",    2045.0f},
  {"MOTEGI",    4801.0f},
  {"SUGO",      3704.0f},
  {"NIKKO",     1030.0f}
};
static const int CIRCUIT_COUNT = sizeof(CIRCUITS) / sizeof(CIRCUITS[0]);
static const int REV_PRESETS[] = {3000, 6000, 6500, 7000, 7500, 8000, 8500};
static const int REV_PRESET_COUNT = sizeof(REV_PRESETS) / sizeof(REV_PRESETS[0]);

// --- テレメトリデータ（実車CAN待機時は0クリア状態で起動） ---
struct TelemetryData {
  int   rpm          = 0;
  float speedKmh     = 0.0f;
  int   gear         = 0;
  float waterTempC   = 0.0f;
  float oilTempC     = 0.0f;
  float ambientTempC = 25.0f;
  float oilPressBar  = 0.0f;
  float fuelPct      = 0.0f;
  float throttlePct  = 0.0f;
  float brakePct     = 0.0f;
  float brakePeakPct = 0.0f;
  float brakeMpa     = 0.0f;
  float steerDeg     = 0.0f;
  float latG         = 0.0f;
  float lonG         = 0.0f;
  int   lapCount     = 0;
  float lapTimeSec   = 0.0f;
  float lastLapSec   = 0.0f;
  float bestLapSec   = 0.0f;
  float optLapSec    = 0.0f;
  float targetLapSec = 0.0f;
  float deltaSec     = 0.0f;
  float s1Sec        = 0.0f;
  float s2Sec        = 0.0f;
  float s3Sec        = 0.0f;
  float ds1          = 0.0f;
  float ds2          = 0.0f;
  float ds3          = 0.0f;
  int   lapDistM     = 0;
  int   sessionSec   = 900;
  float tripKm       = 0.0f;
  int   clockHour    = 14;
  int   clockMin     = 25;
  int   clockSec     = 0;
} telem;

struct GPoint { float x; float y; };
static const int G_TRAIL_MAX = 20;
GPoint gTrail[G_TRAIL_MAX];
int    gTrailCount = 0;
float  gZeroLat = 0.0f;
float  gZeroLon = 0.0f;

// --- 数値・イージングヘルパー ---
static float clampf(float v, float lo, float hi) {
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}
static float lerpf(float a, float b, float t) {
  return a + (b - a) * t;
}
static float easeOutCubic(float t) {
  t = clampf(t, 0.0f, 1.0f);
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}
static float easeInCubic(float t) {
  t = clampf(t, 0.0f, 1.0f);
  return t * t * t;
}

static void formatLapTime(char* buf, size_t len, float sec) {
  if (sec <= 0.0f || sec > 3599.0f) { snprintf(buf, len, "--'--.--"); return; }
  int m = (int)(sec / 60.0f);
  float r = sec - m * 60.0f;
  int s = (int)r;
  int h = (int)((r - s) * 100.0f + 0.5f);
  if (h >= 100) { h -= 100; s++; }
  if (s >= 60)  { s -= 60; m++; }
  snprintf(buf, len, "%d'%02d.%02d", m, s, h);
}

static void formatDelta(char* buf, size_t len, float d) {
  float ad = fabsf(d);
  if (ad > 99.99f) ad = 99.99f;
  snprintf(buf, len, "%s%.2f", (d < 0.0f) ? "-" : "+", ad);
}

static void beepClick(int freq = 1046, int ms = 35) {
  M5.Speaker.tone(freq, ms);
}

// --- 斜体フォント描画ヘルパー（PSRAM上のBarlow VLW優先、未配置時は内蔵斜体へ自動フォールバック） ---
static void drawItalicText(M5Canvas& c, const char* txt, int x, int y,
                           float targetPx, uint16_t fg, textdatum_t datum = MC_DATUM) {
  // 一番小さい文字が読みづらくならないよう最小サイズを底上げ
  if (targetPx < 17.5f) targetPx = 17.5f;

  c.setTextDatum(datum);
  c.setTextColor(fg);

  const uint8_t* chosenVlw = nullptr;
  float basePx = 28.0f;

  if (targetPx >= 86.0f && vlwBuf120) {
    chosenVlw = vlwBuf120; basePx = 120.0f;
  } else if (targetPx >= 50.0f && vlwBuf68) {
    chosenVlw = vlwBuf68;  basePx = 68.0f;
  } else if (targetPx >= 32.0f && vlwBuf44) {
    chosenVlw = vlwBuf44;  basePx = 44.0f;
  } else if (vlwBuf28) {
    chosenVlw = vlwBuf28;  basePx = 28.0f;
  }

  if (chosenVlw) {
    ensureCanvasVlw(c, chosenVlw);
    float sc = clampf(targetPx / basePx, 0.62f, 1.35f);
    c.setTextSize(sc, sc);
    c.drawString(txt, x, y);
    c.setTextSize(1.0f, 1.0f);
    return;
  }

  // VLW未配置時のフォールバック（高解像度DejaVuフォントを等倍ベースで使いギザギザを解消）
  ensureCanvasVlw(c, nullptr);
  if (targetPx >= 95.0f) {
    c.setFont(&fonts::DejaVu72);
    c.setTextSize(1.45f, 1.45f);
  } else if (targetPx >= 64.0f) {
    c.setFont(&fonts::DejaVu72);
    c.setTextSize(1.0f, 1.0f);
  } else if (targetPx >= 48.0f) {
    c.setFont(&fonts::DejaVu56);
    c.setTextSize(1.0f, 1.0f);
  } else if (targetPx >= 34.0f) {
    c.setFont(&fonts::DejaVu40);
    c.setTextSize(1.0f, 1.0f);
  } else if (targetPx >= 22.0f) {
    c.setFont(&fonts::DejaVu24);
    c.setTextSize(1.0f, 1.0f);
  } else {
    c.setFont(&fonts::DejaVu18);
    c.setTextSize(1.0f, 1.0f);
  }
  c.drawString(txt, x, y);
  c.setTextSize(1.0f, 1.0f);
}

// 日本語ラベル用ヘルパー (efontJA_24_b)
static void drawJpText(M5Canvas& c, const char* txt, int x, int y,
                       float scale, uint16_t fg, textdatum_t datum = ML_DATUM) {
  ensureCanvasVlw(c, nullptr);
  c.setTextDatum(datum);
  c.setTextColor(fg);
  c.setFont(&fonts::efontJA_24_b);
  c.setTextSize(scale, scale);
  c.drawString(txt, x, y);
  c.setTextSize(1.0f, 1.0f);
}

// --- パーツごとの分解・収束オフセット計算（Arduino IDE自動プロトタイプ対策済み） ---
static bool calcPartOffset(int pageId, float ex, float ey, int staggerMs, int& outDx, int& outDy) {
  outDx = 0;
  outDy = 0;
  if (!isTransitioning) {
    return (pageId == currentPage);
  }
  if (transPhase == 0 && pageId == transFromPage && transFromPage != transToPage) {
    // 前半 (0 .. TRANS_LEAVE_MS): 中央から四方 (ex, ey) へ放射状に分散して退場
    // 初回フレームから即座に動きが見えるよう +35ms の初速を与える
    uint32_t pe = (millis() - transPhaseStartMs) + 35;
    float start = staggerMs * 0.35f;
    float dur   = max(80.0f, (float)TRANS_LEAVE_MS - start);
    float t     = clampf(((float)pe - start) / dur, 0.0f, 1.0f);
    if (t >= 1.0f) return false;
    float e = t * t; // 加速しながら四方へ飛び散る
    outDx = (int)(ex * e);
    outDy = (int)(ey * e);
    return true;
  }
  if (transPhase == 1 && pageId == transToPage) {
    // 後半 (0 .. TRANS_ENTER_MS): 四方 (ex, ey) → 中央 (0, 0) へ収束して合体
    uint32_t pe = millis() - transPhaseStartMs;
    float start = staggerMs * 0.35f;
    if ((float)pe < start) return false;
    float dur   = max(90.0f, (float)TRANS_ENTER_MS - start);
    float t     = clampf(((float)pe - start) / dur, 0.0f, 1.0f);
    float e     = easeOutCubic(t); // 外から勢いよく飛び込んで中央でピタッと減速着地
    outDx = (int)(ex * (1.0f - e));
    outDy = (int)(ey * (1.0f - e));
    return true;
  }
  return false;
}

// --- NVS 設定保存 / 読み込み ---
static void saveSettings() {
  prefs.begin("dash38", false);
  prefs.putInt("shift", shiftLimitRpm);
  prefs.putInt("brt", brightnessPct);
  prefs.putInt("trk", circuitIdx);
  prefs.putInt("page", currentPage);
  prefs.end();
}

static void loadSettings() {
  prefs.begin("dash38", true);
  shiftLimitRpm = prefs.getInt("shift", 7000);
  brightnessPct = prefs.getInt("brt", 85);
  circuitIdx    = prefs.getInt("trk", 0);
  currentPage   = prefs.getInt("page", 0);
  prefs.end();
  shiftLimitRpm = (int)clampf(shiftLimitRpm, 3000, 9000);
  brightnessPct = (int)clampf(brightnessPct, 15, 100);
  if (circuitIdx < 0 || circuitIdx >= CIRCUIT_COUNT) circuitIdx = 0;
  if (currentPage < 0 || currentPage > 3) currentPage = 0;
}

// --- サーキット背景透かし＆カーボンキャッシュ再構築 ---
static void drawCircuitWatermark(M5Canvas& c, int idx, uint16_t col) {
  switch (idx % CIRCUIT_COUNT) {
    case 0: // SODEGAURA
      c.drawWideLine(200, 520, 920, 520, 9, col);
      c.drawWideLine(920, 520, 1040, 390, 9, col);
      c.drawWideLine(1040, 390, 930, 275, 9, col);
      c.drawWideLine(930, 275, 650, 240, 9, col);
      c.drawWideLine(650, 240, 520, 165, 9, col);
      c.drawWideLine(520, 165, 250, 210, 9, col);
      c.drawWideLine(250, 210, 200, 520, 9, col);
      break;
    case 1: // FSW
      c.drawWideLine(160, 220, 1060, 220, 9, col);
      c.drawWideLine(1060, 220, 1110, 340, 9, col);
      c.drawWideLine(1110, 340, 760, 355, 9, col);
      c.drawWideLine(760, 355, 780, 540, 9, col);
      c.drawWideLine(780, 540, 480, 570, 9, col);
      c.drawWideLine(480, 570, 220, 480, 9, col);
      c.drawWideLine(220, 480, 160, 220, 9, col);
      break;
    case 2: // TC2000
      c.drawWideLine(240, 240, 920, 200, 9, col);
      c.drawWideLine(920, 200, 1030, 380, 9, col);
      c.drawWideLine(1030, 380, 720, 405, 9, col);
      c.drawWideLine(720, 405, 980, 535, 9, col);
      c.drawWideLine(980, 535, 270, 540, 9, col);
      c.drawWideLine(270, 540, 240, 240, 9, col);
      break;
    default:
      c.drawWideLine(230, 200, 960, 190, 9, col);
      c.drawWideLine(960, 190, 1050, 350, 9, col);
      c.drawWideLine(1050, 350, 880, 530, 9, col);
      c.drawWideLine(880, 530, 250, 510, 9, col);
      c.drawWideLine(250, 510, 230, 200, 9, col);
      break;
  }
}

static void rebuildCarbonCache() {
  sprCarbonBg.fillSprite(COL_BG);
  for (int x = -720; x < 1280; x += 8) {
    sprCarbonBg.drawLine(x, 0, x + 720, 720, COL_CARBON_A);
  }
  drawCircuitWatermark(sprCarbonBg, circuitIdx, 0x0947);
}

// --- P4 ホワイトタコメーター角度計算 (HTMLと完全一致) ---
// 0=90°(真下6時), 5=180°(9時), 8=270°(真上12時), 10=330°(2時)
static float rpmToLfaDeg(float rpmK) {
  rpmK = clampf(rpmK, 0.0f, 10.0f);
  if (rpmK <= 5.0f) return 90.0f + (rpmK / 5.0f) * 90.0f;
  if (rpmK <= 8.0f) return 180.0f + ((rpmK - 5.0f) / 3.0f) * 90.0f;
  return 270.0f + ((rpmK - 8.0f) / 2.0f) * 60.0f;
}

// --- P4 ホワイトタコメーター静的文字盤キャッシュ (620x620・アンチエイリアス円) ---
static void rebuildTachoCache() {
  const int cx = 310;
  const int cy = 310;
  const int radius = 279;

  sprTachoBg.fillSprite(COL_BLACK);

  // 3Dメタリックベゼルリング
  sprTachoBg.fillSmoothCircle(cx, cy, radius, 0x0842);
  sprTachoBg.fillArc(cx, cy, radius - 6,  radius - 1,  0, 360, 0xE73C);
  sprTachoBg.fillArc(cx, cy, radius - 14, radius - 6,  0, 360, 0x6B8E);
  sprTachoBg.fillArc(cx, cy, radius - 22, radius - 14, 0, 360, 0x18E5);

  // 内側ホワイト文字盤
  sprTachoBg.fillSmoothCircle(cx, cy, radius - 22, COL_OFFWHITE);

  // 設定REV連動のレッドゾーン（3.0k〜9.5kから10.0kまで）
  float redStartK = clampf(shiftLimitRpm / 1000.0f, 3.0f, 9.5f);
  float angRed0 = rpmToLfaDeg(redStartK);
  float angRed1 = rpmToLfaDeg(10.0f);
  sprTachoBg.fillArc(cx, cy, 174, radius - 22, angRed0, angRed1, COL_RED);

  // 目盛り＆38px数字 (0〜10, 0.5刻み)
  for (int half = 0; half <= 20; half++) {
    float r = half * 0.5f;
    float deg = rpmToLfaDeg(r);
    float rad = deg * DEG_TO_RAD;
    bool isMajor = (half % 2 == 0);
    float outerR = radius - 24;
    float innerR = outerR - (isMajor ? 22 : 12);
    uint16_t tCol = (r >= redStartK) ? COL_WHITE : 0x0863;

    int x1 = cx + (int)(cosf(rad) * outerR);
    int y1 = cy + (int)(sinf(rad) * outerR);
    int x2 = cx + (int)(cosf(rad) * innerR);
    int y2 = cy + (int)(sinf(rad) * innerR);
    sprTachoBg.drawWideLine(x1, y1, x2, y2, isMajor ? 4 : 2, tCol);

    if (isMajor) {
      float textR = innerR - 30;
      int tx = cx + (int)(cosf(rad) * textR);
      int ty = cy + (int)(sinf(rad) * textR);
      char nb[4];
      snprintf(nb, sizeof(nb), "%d", (int)r);

      // レッドゾーン及び赤白境界の数字には2pxの黒縁取りを付けてくっきり読めるようにする
      if (r >= redStartK - 0.5f) {
        static const int8_t ox[8] = {-2, 2, 0, 0, -2, 2, -2, 2};
        static const int8_t oy[8] = {0, 0, -2, 2, -2, -2, 2, 2};
        for (int k = 0; k < 8; k++) {
          drawItalicText(sprTachoBg, nb, tx + ox[k], ty + oy[k], 38.0f, COL_BLACK, MC_DATUM);
        }
        drawItalicText(sprTachoBg, nb, tx, ty, 38.0f, COL_WHITE, MC_DATUM);
      } else {
        drawItalicText(sprTachoBg, nb, tx, ty, 38.0f, tCol, MC_DATUM);
      }
    }
  }

  // 中央ブラックLCDコア枠 (半径176px)
  sprTachoBg.fillSmoothCircle(cx, cy, 176, COL_BLACK);
  sprTachoBg.drawCircle(cx, cy, 176, 0x296A);
  sprTachoBg.drawCircle(cx, cy, 175, 0x18E5);
}

// --- ページ切り替え開始（PSRAMキャッシュを使った爆速分解＆収束） ---
static void drawPageP1(M5Canvas& c, int topY);
static void drawPageP2(M5Canvas& c, int topY);
static void drawPageP3(M5Canvas& c, int topY);
static void drawPageP4(M5Canvas& c, bool isRevLimit);
static void drawShiftAndCtrlHeader(M5Canvas& c, bool isRevLimit);

static void renderPageToCanvas(M5Canvas& dst, int pid, bool isRevLimit) {
  int topY = (pid == 3) ? 0 : 86;
  if (pid == 3) {
    dst.fillSprite(COL_BLACK);
  } else {
    memcpy(dst.getBuffer(), sprCarbonBg.getBuffer(), 1280 * 720 * 2);
  }
  bool savedTrans = isTransitioning;
  int  savedPage  = currentPage;
  float savedPillX = tabPillX;
  float savedPillW = tabPillW;
  static const int tabX[4] = {14, 134, 256, 376};
  static const int tabW[4] = {116, 118, 116, 118};

  isTransitioning = false;
  currentPage     = pid;
  tabPillX        = (float)tabX[pid];
  tabPillW        = (float)tabW[pid];
  if (pid == 0)      drawPageP1(dst, topY);
  else if (pid == 1) drawPageP2(dst, topY);
  else if (pid == 2) drawPageP3(dst, topY);
  else if (pid == 3) drawPageP4(dst, isRevLimit);
  if (pid != 3) {
    float savedHdr = headerVis;
    headerVis = 1.0f;
    drawShiftAndCtrlHeader(dst, isRevLimit);
    headerVis = savedHdr;
  }
  isTransitioning = savedTrans;
  currentPage     = savedPage;
  tabPillX        = savedPillX;
  tabPillW        = savedPillW;
}

static void markAllPageCachesDirty() {
  for (int i = 0; i < 4; i++) pageCacheDirty[i] = true;
}

static void ensurePageCache(int pid, bool isRevLimit) {
  if (pid < 0 || pid > 3) return;
  if (pageCacheDirty[pid]) {
    renderPageToCanvas(sprPageCache[pid], pid, isRevLimit);
    pageCacheDirty[pid] = false;
  }
}

// 超高速（1フレーム数ms・PSRAMダブルバッファ合成）で中央から四方へ分散／四方から中央へ収束するパーツ定義
struct TransPartDef {
  int x, y, w, h;
  int ex, ey;
  int stagger;
};

// PSRAMキャッシュ(src)の指定矩形を、オフセット(dx, dy)付きで作業フレーム(dst)へ行単位memcpyで超高速合成
static void blitCacheRectToFrame(M5Canvas& src, M5Canvas& dst,
                                 int sx, int sy, int w, int h,
                                 int dx, int dy, int minY) {
  int dstX0 = sx + dx;
  int dstY0 = sy + dy;
  int dstX1 = dstX0 + w;
  int dstY1 = dstY0 + h;

  int cX0 = max(0, dstX0);
  int cY0 = max(minY, dstY0);
  int cX1 = min(1280, dstX1);
  int cY1 = min(720, dstY1);
  if (cX1 <= cX0 || cY1 <= cY0) return;

  int copyW = cX1 - cX0;
  int copyH = cY1 - cY0;
  int srcX0 = sx + (cX0 - dstX0);
  int srcY0 = sy + (cY0 - dstY0);

  const uint16_t* sBuf = (const uint16_t*)src.getBuffer();
  uint16_t*       dBuf = (uint16_t*)dst.getBuffer();
  if (!sBuf || !dBuf) return;

  size_t rowBytes = (size_t)copyW * 2;
  for (int row = 0; row < copyH; row++) {
    memcpy(dBuf + (cY0 + row) * 1280 + cX0,
           sBuf + (srcY0 + row) * 1280 + srcX0,
           rowBytes);
  }
}

// 90度回転(setRotation(3))時のPSRAMキャッシュミスを劇的に抑える128x32タイル分割高速転送
// （1タイル＝8KBでESP32-P4のL1キャッシュに完全収容され、全画面転送が大幅に高速化）
static void pushFrameTiled(M5Canvas& src, int minY) {
  M5.Display.startWrite();
  const int TW = 128;
  const int TH = 32;
  for (int ty = minY; ty < 720; ty += TH) {
    int th = min(TH, 720 - ty);
    for (int tx = 0; tx < 1280; tx += TW) {
      int tw = min(TW, 1280 - tx);
      M5.Display.setClipRect(tx, ty, tw, th);
      src.pushSprite(0, 0);
    }
  }
  M5.Display.clearClipRect();
  M5.Display.endWrite();
}

// 中身入りのまま全パーツを画面中心(640,360)から四方八方へ大きく分散／四方八方から中央へ収束合成
static void drawTransitionAnimatedFrame(int pid, uint16_t bgCol, bool forceLand) {
  // P1: 10パーツ（右のセクターボードも上・中・下の3ブロックに分けて右上・右・右下へ放射状に飛ばす）
  static const TransPartDef p1Defs[10] = {
    { 10,  96, 202, 190, -460, -280,  0}, // LAP (左上)
    {222,  96, 413, 190, -160, -340, 12}, // SPEED (真上やや左)
    { 10, 296, 360, 262, -500,   10, 20}, // LAP TIMER (真左)
    {380, 296, 255, 262, -140,  110, 28}, // BEST DELTA (左下中央)
    { 10, 568, 201, 142, -440,  300, 16}, // WATER (左下)
    {221, 568, 201, 142, -180,  330, 24}, // SESSION (真下やや左)
    {432, 568, 203, 142,  -20,  340, 32}, // CLOCK/FUEL (真下)
    {645,  96, 625, 234,  460, -280,  8}, // SECTOR 上段: HEADER+S3+S2 (右上)
    {645, 330, 625, 182,  540,    0, 20}, // SECTOR 中段: S1+LAST (真右)
    {645, 512, 625, 198,  460,  280, 30}  // SECTOR 下段: BEST+OPT (右下)
  };
  // P2: 6パーツ（左タイミング3分割＋右G-BALL3分割で四方八方へ放射状分散）
  static const TransPartDef p2Defs[6] = {
    { 10,  96, 625, 192, -440, -280,  0}, // TIMING 上 (左上)
    { 10, 288, 625, 260, -520,    0, 18}, // TIMING 中 (真左)
    { 10, 548, 625, 162, -440,  280, 32}, // TIMING 下 (左下)
    {645,  96, 625,  60,  400, -300,  8}, // G-BALL ヘッダー (右上)
    {645, 156, 625, 474,  520,    0, 18}, // G-BALL 本体 (真右)
    {645, 630, 625,  80,  400,  300, 32}  // G-BALL 下部バー (右下)
  };
  // P3: 6パーツ（上段3枚は左上・真上・右上、下段はステアリング左下・BRAKE真下・THROTTLE右下）
  static const TransPartDef p3Defs[6] = {
    { 10,  96, 413,  92, -440, -290,  0}, // TOP LEFT (左上)
    {433,  96, 520,  92,    0, -340, 12}, // TOP MID (真上)
    {963,  96, 307,  92,  440, -290, 24}, // TOP RIGHT (右上)
    { 10, 198, 518, 512, -480,  260, 16}, // STEER & G-BALL (左下)
    {538, 198, 366, 512,  100,  360, 26}, // BRAKE (真下)
    {904, 198, 366, 512,  480,  260, 36}  // THROTTLE (右下)
  };
  // P4: 4象限（左上・左下・右上・右下の4方向へパッと割れて、四方から中央へピタッと合体）
  static const TransPartDef p4Defs[4] = {
    {  0,   0, 556, 360, -460, -260,  8}, // 左上: OUTSIDE + メニュー上
    {  0, 360, 556, 360, -460,  260, 22}, // 左下: メニュー下 + 時計
    {556,   0, 724, 360,  460, -260,  0}, // 右上: 水温アーク + タコ上半分
    {556, 360, 724, 360,  460,  260, 16}  // 右下: 燃料アーク + タコ下半分
  };

  const TransPartDef* defs = p1Defs;
  int cnt = 10;
  if (pid == 1)      { defs = p2Defs; cnt = 6; }
  else if (pid == 2) { defs = p3Defs; cnt = 6; }
  else if (pid == 3) { defs = p4Defs; cnt = 4; }

  bool keepHeader = (transFromPage != 3 && transToPage != 3);
  int  minY       = keepHeader ? 86 : 0;

  // 1) 最終着地フレームはキャッシュをそのままピタッとタイル高速転送して完了（後追い更新ゼロ）
  if (forceLand) {
    pushFrameTiled(sprPageCache[pid], minY);
    return;
  }

  // 2) PSRAM上の sprFrame へ背景クリア＋全パーツをmemcpyで超高速合成（チラつき・残像ゼロ）
  uint16_t* fBuf = (uint16_t*)sprFrame.getBuffer();
  if (fBuf) {
    if (bgCol == 0) {
      memset(fBuf + minY * 1280, 0, (size_t)(720 - minY) * 1280 * 2);
    } else {
      uint32_t pair = ((uint32_t)bgCol << 16) | bgCol;
      uint32_t* p32 = (uint32_t*)(fBuf + minY * 1280);
      size_t cnt32  = (size_t)(720 - minY) * 640;
      for (size_t i = 0; i < cnt32; i++) p32[i] = pair;
    }
  }
  for (int i = 0; i < cnt; i++) {
    int dx = 0, dy = 0;
    if (calcPartOffset(pid, (float)defs[i].ex, (float)defs[i].ey, defs[i].stagger, dx, dy)) {
      blitCacheRectToFrame(sprPageCache[pid], sprFrame,
                           defs[i].x, defs[i].y, defs[i].w, defs[i].h,
                           dx, dy, minY);
    }
  }

  // 3) 合成済みフレームをキャッシュ最適タイル転送で高速表示
  pushFrameTiled(sprFrame, minY);
}

static void resetPartialCaches() {
  lastDrawnLedCnt = -99;
  lastP1Speed = lastP1Rpm = lastP1Lap10 = -1;
  lastP1Delta100 = -9999;
  lastP1DeltaFast = -1;
  lastP4Speed = lastP4RpmStep = -1;
  lastP4Gear = -99;
  lastP4NeedleDeg = -999.0f;
  lastP3BrakeFill = lastP3BrakePeak = lastP3ThrFill = -1;
  lastP3BrakePct = lastP3ThrPct = lastP3Speed = -1;
  lastP3Gear = -99;
  lastP3SteerDeg = -999.0f;
}

static void switchPage(int nextPage) {
  nextPage = (nextPage + 4) % 4;
  if (nextPage == currentPage && !isTransitioning) return;
  int fromPage = isTransitioning ? transToPage : currentPage;
  bool isRevLimit = (telem.rpm >= shiftLimitRpm);

  // 事前キャッシュ済みなら待ち時間0ms
  ensurePageCache(fromPage, isRevLimit);
  ensurePageCache(nextPage, isRevLimit);

  transFromPage     = fromPage;
  transToPage       = nextPage;
  transFrameCnt     = 0;
  transLeaveFrames  = 0;
  transEnterFrames  = 0;
  transPhase        = 0;
  isTransitioning   = true;
  currentPage       = nextPage;
  headerVis         = (currentPage == 3) ? 0.0f : 1.0f;
  if (currentPage == 3) { p4MenuOpen = true; p4MenuVis = 1.0f; }
  staticDirty       = false;
  resetPartialCaches();

  // P1〜P3間の遷移は上部コントロールバー(86px)だけ1回即更新
  if (fromPage != 3 && nextPage != 3) {
    M5.Display.startWrite();
    M5.Display.setClipRect(0, 0, 1280, 86);
    sprPageCache[nextPage].pushSprite(0, 0);
    M5.Display.clearClipRect();
    M5.Display.endWrite();
  }

  beepClick(1174, 30);
  transStartMs      = millis();
  transPhaseStartMs = transStartMs;
}

// --- 共通カード枠描画（グラデーション＋上端ハイライト） ---
static void drawCardSolid(M5Canvas& c, int x, int y, int w, int h,
                          uint16_t bg = COL_CARD_TOP, uint16_t brd = COL_BORDER, int r = 10) {
  c.fillRoundRect(x, y, w, h, r, bg);
  c.drawRoundRect(x, y, w, h, r, brd);
  c.drawFastHLine(x + r, y + 1, w - r * 2, 0x424B);
}

// --- 最上段 15連シフトインジケーター（画面幅いっぱい・格納アニメーション連動） ---
static void drawShiftAndCtrlHeader(M5Canvas& c, bool isRevLimit) {
  int totalH = (int)(86.0f * headerVis + 0.5f);
  if (totalH <= 2) return;
  int yOff = totalH - 86; // 上からスライドイン/アウト

  // 1) 15連シフトLED (y = yOff .. yOff+42)
  c.fillRect(0, yOff, 1280, 42, COL_CARD_DARK);
  c.drawFastHLine(0, yOff + 41, 1280, COL_SLATE_700);

  int ledStartRpm = (shiftLimitRpm <= 3500) ? 1000 : 3000;
  float ratio = clampf((float)(telem.rpm - ledStartRpm) / (float)max(1000, shiftLimitRpm - ledStartRpm), 0.0f, 1.0f);
  int activeLeds = (int)(ratio * 15.0f);
  bool blinkOn = ((millis() / 85) % 2) == 0;

  const int gap = 6;
  const int padX = 6;
  const int ledW = (1280 - padX * 2 - gap * 14) / 15;
  for (int i = 0; i < 15; i++) {
    int lx = padX + i * (ledW + gap);
    int ly = yOff + 5;
    uint16_t col = COL_LED_OFF;
    if (isRevLimit) {
      col = blinkOn ? COL_RED_BRIGHT : 0x5002;
    } else if (i < activeLeds) {
      if (i < 5)       col = COL_GREEN;
      else if (i < 10) col = COL_YELLOW;
      else             col = COL_RED;
    }
    c.fillRoundRect(lx, ly, ledW, 32, 4, col);
    if (col != COL_LED_OFF) {
      c.drawFastHLine(lx + 3, ly + 2, ledW - 6, COL_WHITE);
    }
  }

  // 2) コントロールバー (y = yOff+42 .. yOff+86)
  int cy0 = yOff + 42;
  c.fillRect(0, cy0, 1280, 44, 0x10A4);
  c.drawFastHLine(0, cy0 + 43, 1280, COL_SLATE_700);

  // タブコンテナ＋スライドピル
  c.fillRoundRect(10, cy0 + 5, 488, 34, 8, COL_CARD_DARK);
  c.drawRoundRect(10, cy0 + 5, 488, 34, 8, COL_SLATE_700);

  static const char* tabLabels[4] = {"P1: RACE", "P2: G-BALL", "P3: PEDAL", "P4: STREET"};
  static const int   tabX[4]      = {14, 134, 256, 376};
  static const int   tabW[4]      = {116, 118, 116, 118};

  // スライドピル補間
  tabPillX = lerpf(tabPillX, (float)tabX[currentPage], 0.35f);
  tabPillW = lerpf(tabPillW, (float)tabW[currentPage], 0.35f);
  c.fillRoundRect((int)tabPillX, cy0 + 8, (int)tabPillW, 28, 6, COL_ROSE);

  for (int i = 0; i < 4; i++) {
    uint16_t col = (i == currentPage) ? COL_WHITE : COL_SLATE_400;
    drawItalicText(c, tabLabels[i], tabX[i] + tabW[i] / 2, cy0 + 22, 16.0f, col, MC_DATUM);
  }

  // SHIFT [- 7000 +] & BRT 85%
  c.fillRoundRect(548, cy0 + 6, 178, 32, 6, COL_BLACK);
  c.drawRoundRect(548, cy0 + 6, 178, 32, 6, COL_SLATE_700);
  drawItalicText(c, "SHIFT", 580, cy0 + 22, 13.0f, COL_SLATE_400, MC_DATUM);
  c.fillRoundRect(610, cy0 + 10, 24, 24, 4, COL_SLATE_700);
  drawItalicText(c, "-", 622, cy0 + 22, 16.0f, COL_WHITE, MC_DATUM);
  char sbuf[12];
  snprintf(sbuf, sizeof(sbuf), "%d", shiftLimitRpm);
  drawItalicText(c, sbuf, 666, cy0 + 22, 17.0f, COL_CYAN, MC_DATUM);
  c.fillRoundRect(696, cy0 + 10, 24, 24, 4, COL_SLATE_700);
  drawItalicText(c, "+", 708, cy0 + 22, 16.0f, COL_WHITE, MC_DATUM);

  c.fillRoundRect(736, cy0 + 6, 94, 32, 6, COL_BLACK);
  c.drawRoundRect(736, cy0 + 6, 94, 32, 6, COL_SLATE_700);
  drawItalicText(c, "BRT", 760, cy0 + 22, 13.0f, COL_SLATE_400, MC_DATUM);
  snprintf(sbuf, sizeof(sbuf), "%d%%", brightnessPct);
  drawItalicText(c, sbuf, 804, cy0 + 22, 17.0f, COL_AMBER, MC_DATUM);

  // 右側：4段階 安心ステータスランプ＋MODE
  uint16_t lCol = COL_EMERALD_BRD;
  const char* lTxt = "ALL OK";
  if (lampState == 0)      { lCol = COL_SLATE_700; lTxt = "BLE WAIT"; }
  else if (lampState == 1) { lCol = COL_AMBER_BRD; lTxt = "PC WAIT";  }
  else if (lampState == 3) { lCol = COL_ROSE;      lTxt = "REC ON";   }

  c.fillRoundRect(1004, cy0 + 6, 128, 32, 6, lCol);
  c.fillCircle(1020, cy0 + 22, 5, COL_WHITE);
  drawItalicText(c, lTxt, 1076, cy0 + 22, 15.0f, COL_WHITE, MC_DATUM);

  uint16_t mBg = isDemoMode ? COL_CYAN_DARK : 0x18E5;
  c.fillRoundRect(1140, cy0 + 6, 130, 32, 6, mBg);
  c.drawRoundRect(1140, cy0 + 6, 130, 32, 6, isDemoMode ? COL_CYAN : COL_SLATE_700);
  drawItalicText(c, isDemoMode ? "MODE: DEMO" : "MODE: CAN", 1205, cy0 + 22, 15.0f,
                 isDemoMode ? COL_CYAN : COL_SLATE_300, MC_DATUM);
}

// --- G-BALL 共通描画（P2 / P3共用） ---
// --- G-BALL 共通描画（P2 / P3共用・アンチエイリアス円） ---
static void drawGBallWidget(M5Canvas& c, int cx, int cy, int maxR) {
  c.fillSmoothCircle(cx, cy, maxR, 0x0842);
  c.drawCircle(cx, cy, maxR, 0x31C9);
  c.drawCircle(cx, cy, (int)(maxR * 0.66f), 0x2127);
  c.drawCircle(cx, cy, (int)(maxR * 0.33f), 0x2127);
  c.drawFastHLine(cx - maxR, cy, maxR * 2, 0x2127);
  c.drawFastVLine(cx, cy - maxR, maxR * 2, 0x2127);

  // トレイル
  for (int i = 0; i < gTrailCount; i++) {
    int tx = cx + (int)(clampf(gTrail[i].x / 1.5f, -1.0f, 1.0f) * maxR);
    int ty = cy - (int)(clampf(gTrail[i].y / 1.5f, -1.0f, 1.0f) * maxR);
    c.fillSmoothCircle(tx, ty, (i > gTrailCount - 6) ? 4 : 2, COL_CYAN);
  }

  float lat = telem.latG - gZeroLat;
  float lon = telem.lonG - gZeroLon;
  int bx = cx + (int)(clampf(lat / 1.5f, -1.0f, 1.0f) * maxR);
  int by = cy - (int)(clampf(lon / 1.5f, -1.0f, 1.0f) * maxR);

  c.drawWideLine(cx, cy, bx, by, 3, COL_CYAN);
  c.fillSmoothCircle(bx, by, 14, COL_WHITE);
  c.fillSmoothCircle(bx, by, 12, COL_AMBER);
}

// =========================================================================
// PAGE 1: RACE (HTMLプレビュー 1:1 再現)
// =========================================================================
static void drawPageP1(M5Canvas& c, int topY) {
  char buf[32];
  int dx = 0, dy = 0;

  // 1) LAPカード (x=10, y=topY+10, w=202, h=190)
  if (calcPartOffset(0, -150, -115, 0, dx, dy)) {
    int x = 10 + dx, y = topY + 10 + dy, w = 202, h = 190;
    c.fillRoundRect(x, y, w, h, 10, COL_AMBER_BG);
    c.drawRoundRect(x, y, w, h, 10, COL_AMBER_BRD);
    c.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, COL_AMBER_BRD);
    drawItalicText(c, "LAP", x + 14, y + 18, 15.0f, COL_AMBER, ML_DATUM);
    drawItalicText(c, "TAP: 0", x + w - 12, y + 18, 12.0f, COL_AMBER_LIGHT, MR_DATUM);
    snprintf(buf, sizeof(buf), "%d", telem.lapCount);
    drawItalicText(c, buf, x + w / 2, y + 112, 104.0f, COL_AMBER, MC_DATUM);
  }

  // 2) SPEEDカード (x=222, y=topY+10, w=413, h=190) — バーなし・右上RPM数字のみ
  if (calcPartOffset(0, -55, -135, 25, dx, dy)) {
    int x = 222 + dx, y = topY + 10 + dy, w = 413, h = 190;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "SPEED", x + 14, y + 20, 16.0f, COL_SLATE_400, ML_DATUM);
    drawItalicText(c, "km/h", x + 84, y + 21, 13.0f, 0x6B8E, ML_DATUM);
    snprintf(buf, sizeof(buf), "%d", telem.rpm);
    drawItalicText(c, buf, x + w - 16, y + 24, 30.0f, COL_CYAN, MR_DATUM);
    snprintf(buf, sizeof(buf), "%d", (int)(telem.speedKmh + 0.5f));
    drawItalicText(c, buf, x + w / 2, y + 116, 126.0f, COL_WHITE, MC_DATUM);
  }

  // 3) LAP TIMERカード (x=10, y=topY+210, w=360, h=262)
  if (calcPartOffset(0, -165, 10, 40, dx, dy)) {
    int x = 10 + dx, y = topY + 210 + dy, w = 360, h = 262;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "LAP TIMER", x + 14, y + 22, 16.0f, COL_CYAN, ML_DATUM);
    c.fillRoundRect(x + w - 92, y + 10, 80, 24, 5, COL_CARD_DARK);
    c.drawRoundRect(x + w - 92, y + 10, 80, 24, 5, COL_SLATE_700);
    snprintf(buf, sizeof(buf), "%dm", telem.lapDistM);
    drawItalicText(c, buf, x + w - 52, y + 22, 14.0f, COL_SLATE_300, MC_DATUM);

    formatLapTime(buf, sizeof(buf), telem.lapTimeSec);
    drawItalicText(c, buf, x + w / 2, y + 132, 72.0f, COL_WHITE, MC_DATUM);

    drawItalicText(c, "TARGET: 1'13.50", x + 14, y + h - 18, 14.0f, COL_SLATE_400, ML_DATUM);
    drawItalicText(c, "SECTOR 3", x + w - 14, y + h - 18, 14.0f, COL_CYAN, MR_DATUM);
  }

  // 4) BEST Δ ソリッド3Dカード (x=380, y=topY+210, w=255, h=262)
  if (calcPartOffset(0, -45, 35, 55, dx, dy)) {
    int x = 380 + dx, y = topY + 210 + dy, w = 255, h = 262;
    bool fast = (telem.deltaSec <= 0.0f);
    uint16_t bg  = fast ? COL_BLUE : COL_ROSE;
    uint16_t brd = fast ? COL_BLUE_LIGHT : COL_ROSE_LIGHT;
    c.fillRoundRect(x, y, w, h, 12, bg);
    c.drawRoundRect(x, y, w, h, 12, brd);
    c.drawFastHLine(x + 12, y + 2, w - 24, COL_WHITE);

    drawItalicText(c, "BEST D", x + 14, y + 22, 18.0f, COL_WHITE, ML_DATUM);
    c.fillRoundRect(x + w - 98, y + 10, 88, 24, 4, fast ? COL_BLUE_DARK : 0xA007);
    drawItalicText(c, "REF: BEST", x + w - 54, y + 22, 15.0f, COL_WHITE, MC_DATUM);

    formatDelta(buf, sizeof(buf), telem.deltaSec);
    drawItalicText(c, buf, x + w / 2, y + 134, 66.0f, COL_WHITE, MC_DATUM);
    drawItalicText(c, fast ? "FASTER TIME" : "SLOWER TIME", x + w / 2, y + h - 18, 15.0f, COL_WHITE, MC_DATUM);
  }

  // 5) 下段3枚 (y=topY+482, h=142, w=201ずつ)
  if (calcPartOffset(0, -145, 125, 30, dx, dy)) {
    int x = 10 + dx, y = topY + 482 + dy, w = 201, h = 142;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "WATER TEMP", x + 12, y + 18, 13.0f, COL_SLATE_400, ML_DATUM);
    snprintf(buf, sizeof(buf), "%d C", (int)(telem.waterTempC + 0.5f));
    drawItalicText(c, buf, x + w / 2, y + 74, 54.0f, COL_EMERALD, MC_DATUM);
    c.fillRoundRect(x + 12, y + h - 20, w - 24, 9, 4, 0x18E5);
    int fw = (int)((w - 24) * clampf((telem.waterTempC - 40.0f) / 90.0f, 0.0f, 1.0f));
    c.fillRoundRect(x + 12, y + h - 20, fw, 9, 4, COL_EMERALD);
  }
  if (calcPartOffset(0, -65, 140, 50, dx, dy)) {
    int x = 221 + dx, y = topY + 482 + dy, w = 201, h = 142;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "SESSION TIMER", x + 12, y + 18, 13.0f, COL_AMBER, ML_DATUM);
    snprintf(buf, sizeof(buf), "%02d:%02d", telem.sessionSec / 60, telem.sessionSec % 60);
    drawItalicText(c, buf, x + w / 2, y + 74, 48.0f, COL_AMBER_LIGHT, MC_DATUM);
    drawItalicText(c, "RUNNING", x + w / 2, y + h - 16, 13.0f, COL_SLATE_400, MC_DATUM);
  }
  if (calcPartOffset(0, -15, 140, 70, dx, dy)) {
    int x = 432 + dx, y = topY + 482 + dy, w = 203, h = 142;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "CLOCK", x + 12, y + 22, 13.0f, COL_CYAN, ML_DATUM);
    snprintf(buf, sizeof(buf), "%02d:%02d", telem.clockHour, telem.clockMin);
    drawItalicText(c, buf, x + w - 12, y + 24, 26.0f, COL_WHITE, MR_DATUM);
    c.drawFastHLine(x + 12, y + 44, w - 24, 0x2127);
    drawItalicText(c, "FUEL", x + 12, y + 84, 14.0f, COL_SLATE_400, ML_DATUM);
    snprintf(buf, sizeof(buf), "%d%%", (int)(telem.fuelPct + 0.5f));
    drawItalicText(c, buf, x + w - 12, y + 84, 38.0f, COL_EMERALD, MR_DATUM);
    c.fillRoundRect(x + 12, y + h - 20, w - 24, 9, 4, 0x18E5);
    int fw = (int)((w - 24) * clampf(telem.fuelPct / 100.0f, 0.0f, 1.0f));
    c.fillRoundRect(x + 12, y + h - 20, fw, 9, 4, COL_EMERALD);
  }

  // 6) 右カラム: SECTOR BOARD (x=645, y=topY+10, w=625, h=614)
  if (calcPartOffset(0, 175, 0, 20, dx, dy)) {
    int x = 645 + dx, y = topY + 10 + dy, w = 625, h = 614;
    drawCardSolid(c, x, y, w, h, COL_CARD_BOT);

    c.fillRoundRect(x + 14, y + 12, 8, 30, 2, COL_CYAN);
    drawItalicText(c, "SECTOR BOARD", x + 30, y + 27, 22.0f, COL_WHITE, ML_DATUM);

    // 光るCIRCUITバッジ（文字と枠を大きくして視認性向上）
    c.fillRoundRect(x + w - 364, y + 6, 350, 40, 6, COL_CYAN_DARK);
    c.drawRoundRect(x + w - 364, y + 6, 350, 40, 6, COL_CYAN);
    c.drawRoundRect(x + w - 363, y + 7, 348, 38, 5, COL_CYAN);
    snprintf(buf, sizeof(buf), "CIRCUIT: %s", CIRCUITS[circuitIdx].name);
    drawItalicText(c, buf, x + w - 189, y + 26, 26.0f, COL_CYAN, MC_DATUM);
    c.drawFastHLine(x + 14, y + 52, w - 28, 0x2127);

    struct RowDef {
      const char* lbl;
      float tVal;
      float dVal;
      uint16_t rowBg;
      uint16_t rowBrd;
      uint16_t lblCol;
      uint16_t valCol;
      bool hasDelta;
      const char* tag;
    };
    RowDef rows[6] = {
      {"S3",   telem.s3Sec,      telem.ds3,      0x10A4, 0x2127, COL_SLATE_300, COL_WHITE,       true,  ""},
      {"S2",   telem.s2Sec,      telem.ds2,      0x10A4, 0x2127, COL_SLATE_300, COL_WHITE,       true,  ""},
      {"S1",   telem.s1Sec,      telem.ds1,      0x10A4, 0x2127, COL_CYAN,      COL_WHITE,       true,  ""},
      {"LAST", telem.lastLapSec, telem.deltaSec, 0x10A4, 0x31C9, COL_SLATE_400, COL_AMBER_LIGHT, true,  ""},
      {"BEST", telem.bestLapSec, 0.0f,           COL_EMERALD_BG, COL_EMERALD_BRD, COL_EMERALD, 0x6759, false, "SESSION BEST"},
      {"OPT.", telem.optLapSec,  0.0f,           COL_CYAN_DARK,  COL_CYAN,        COL_CYAN,    0x779F, false, "IDEAL LAP"}
    };

    for (int i = 0; i < 6; i++) {
      int ry = y + 60 + i * 91;
      int rh = 82;
      c.fillRoundRect(x + 14, ry, w - 28, rh, 8, rows[i].rowBg);
      c.drawRoundRect(x + 14, ry, w - 28, rh, 8, rows[i].rowBrd);

      // ラベルをセクタタイムと同等サイズ(44px)に拡大し、重ならないようタイム位置を微調整
      drawItalicText(c, rows[i].lbl, x + 24, ry + rh / 2, 44.0f, rows[i].lblCol, ML_DATUM);
      formatLapTime(buf, sizeof(buf), rows[i].tVal);
      drawItalicText(c, buf, x + 312, ry + rh / 2, 46.0f, rows[i].valCol, MC_DATUM);

      if (rows[i].hasDelta) {
        bool fast = (rows[i].dVal <= 0.0f);
        uint16_t bBg  = fast ? COL_BLUE : COL_ROSE;
        uint16_t bBrd = fast ? COL_BLUE_LIGHT : COL_ROSE_LIGHT;
        int bx = x + w - 166;
        c.fillRoundRect(bx, ry + 13, 140, 56, 8, bBg);
        c.drawRoundRect(bx, ry + 13, 140, 56, 8, bBrd);
        formatDelta(buf, sizeof(buf), rows[i].dVal);
        drawItalicText(c, buf, bx + 70, ry + rh / 2, 30.0f, COL_WHITE, MC_DATUM);
      } else {
        int bx = x + w - 166;
        c.fillRoundRect(bx, ry + 22, 140, 38, 6, rows[i].rowBg);
        c.drawRoundRect(bx, ry + 22, 140, 38, 6, rows[i].rowBrd);
        drawItalicText(c, rows[i].tag, bx + 70, ry + rh / 2, 17.5f, rows[i].valCol, MC_DATUM);
      }
    }
  }
}

// =========================================================================
// PAGE 2: G-BALL (HTMLプレビュー 1:1 再現)
// =========================================================================
static void drawPageP2(M5Canvas& c, int topY) {
  char buf[32];
  int dx = 0, dy = 0;

  // 左カラム (x=10, w=625)
  if (calcPartOffset(1, -175, 0, 0, dx, dy)) {
    int x = 10 + dx, y = topY + 10 + dy, w = 625, h = 614;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "TIMING & SECTORS", x + 16, y + 24, 20.0f, COL_CYAN, ML_DATUM);
    drawItalicText(c, CIRCUITS[circuitIdx].name, x + w - 16, y + 24, 24.0f, COL_CYAN, MR_DATUM);
    c.drawFastHLine(x + 14, y + 44, w - 28, 0x2127);

    // SPEED
    c.fillRoundRect(x + 16, y + 60, w - 32, 122, 8, 0x10A4);
    c.drawRoundRect(x + 16, y + 60, w - 32, 122, 8, 0x2127);
    drawItalicText(c, "SPEED", x + 34, y + 121, 22.0f, COL_SLATE_400, ML_DATUM);
    snprintf(buf, sizeof(buf), "%d km/h", (int)(telem.speedKmh + 0.5f));
    drawItalicText(c, buf, x + w - 34, y + 121, 56.0f, COL_WHITE, MR_DATUM);

    // LAST LAP
    c.fillRoundRect(x + 16, y + 198, w - 32, 122, 8, 0x10A4);
    c.drawRoundRect(x + 16, y + 198, w - 32, 122, 8, 0x2127);
    drawItalicText(c, "LAST LAP", x + 34, y + 259, 22.0f, COL_SLATE_400, ML_DATUM);
    formatLapTime(buf, sizeof(buf), telem.lastLapSec);
    drawItalicText(c, buf, x + w - 34, y + 259, 50.0f, COL_AMBER_LIGHT, MR_DATUM);

    // BEST LAP
    c.fillRoundRect(x + 16, y + 336, w - 32, 122, 8, COL_EMERALD_BG);
    c.drawRoundRect(x + 16, y + 336, w - 32, 122, 8, COL_EMERALD_BRD);
    drawItalicText(c, "BEST LAP", x + 34, y + 397, 22.0f, COL_EMERALD, ML_DATUM);
    formatLapTime(buf, sizeof(buf), telem.bestLapSec);
    drawItalicText(c, buf, x + w - 34, y + 397, 52.0f, 0x6759, MR_DATUM);

    // BEST Δ
    bool fast = (telem.deltaSec <= 0.0f);
    c.fillRoundRect(x + 16, y + 474, w - 32, 122, 8, fast ? 0x0931 : 0x5002);
    c.drawRoundRect(x + 16, y + 474, w - 32, 122, 8, fast ? COL_BLUE_LIGHT : COL_ROSE_LIGHT);
    drawItalicText(c, "BEST D", x + 34, y + 535, 22.0f, COL_BLUE_LIGHT, ML_DATUM);
    formatDelta(buf, sizeof(buf), telem.deltaSec);
    drawItalicText(c, buf, x + w - 34, y + 535, 54.0f, COL_WHITE, MR_DATUM);
  }

  // 右カラム: 大画面 G-FORCE MONITOR (x=645, w=625)
  if (calcPartOffset(1, 175, 0, 25, dx, dy)) {
    int x = 645 + dx, y = topY + 10 + dy, w = 625, h = 614;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "G-FORCE MONITOR", x + 16, y + 24, 18.0f, COL_AMBER, ML_DATUM);
    float gTot = hypotf(telem.latG - gZeroLat, telem.lonG - gZeroLon);
    snprintf(buf, sizeof(buf), "%.2f G", gTot);
    drawItalicText(c, buf, x + w - 16, y + 24, 24.0f, COL_WHITE, MR_DATUM);
    c.drawFastHLine(x + 14, y + 44, w - 28, 0x2127);

    drawGBallWidget(c, x + w / 2, y + 298, 210);

    c.fillRoundRect(x + 16, y + h - 62, w - 32, 48, 8, COL_CARD_DARK);
    c.drawRoundRect(x + 16, y + h - 62, w - 32, 48, 8, 0x2127);
    drawItalicText(c, "LAT / LONG G", x + 30, y + h - 38, 15.0f, COL_SLATE_400, ML_DATUM);
    float lat = telem.latG - gZeroLat;
    float lon = telem.lonG - gZeroLon;
    snprintf(buf, sizeof(buf), "%s %.2f G / %s %.2f G",
             (lat >= 0 ? "R" : "L"), fabsf(lat), (lon >= 0 ? "ACC" : "BRK"), fabsf(lon));
    drawItalicText(c, buf, x + w - 30, y + h - 38, 24.0f, COL_CYAN, MR_DATUM);
  }
}

// =========================================================================
// PAGE 3: PEDAL (極太ペダル＆薄型ステアリング＋大型Gボール)
// =========================================================================
static void drawPageP3(M5Canvas& c, int topY) {
  char buf[32];
  int dx = 0, dy = 0;

  // 上段3カード (h=92, y=topY+10)
  if (calcPartOffset(2, -145, -120, 0, dx, dy)) {
    int x = 10 + dx, y = topY + 10 + dy, w = 413, h = 92;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "LAP TIMER", x + 16, y + 20, 13.0f, COL_CYAN, ML_DATUM);
    formatLapTime(buf, sizeof(buf), telem.lapTimeSec);
    drawItalicText(c, buf, x + 16, y + 58, 38.0f, COL_WHITE, ML_DATUM);
    bool fast = (telem.deltaSec <= 0.0f);
    c.fillRoundRect(x + w - 134, y + 20, 118, 52, 8, fast ? COL_BLUE : COL_ROSE);
    formatDelta(buf, sizeof(buf), telem.deltaSec);
    drawItalicText(c, buf, x + w - 75, y + 46, 26.0f, COL_WHITE, MC_DATUM);
  }
  if (calcPartOffset(2, 0, -140, 25, dx, dy)) {
    int x = 433 + dx, y = topY + 10 + dy, w = 520, h = 92;
    drawCardSolid(c, x, y, w, h, 0x0910, COL_BLUE_LIGHT);
    snprintf(buf, sizeof(buf), "%d", (int)(telem.speedKmh + 0.5f));
    drawItalicText(c, buf, x + 22, y + 48, 72.0f, COL_WHITE, ML_DATUM);
    drawItalicText(c, "km/h", x + 195, y + 62, 18.0f, COL_CYAN, ML_DATUM);
    drawItalicText(c, "GEAR", x + w - 34, y + 20, 13.0f, COL_SLATE_400, MR_DATUM);
    snprintf(buf, sizeof(buf), "%d", telem.gear);
    drawItalicText(c, buf, x + w - 30, y + 58, 50.0f, COL_AMBER, MR_DATUM);
  }
  if (calcPartOffset(2, 145, -120, 45, dx, dy)) {
    int x = 963 + dx, y = topY + 10 + dy, w = 307, h = 92;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "WATER", x + 76, y + 24, 13.0f, COL_SLATE_400, MC_DATUM);
    snprintf(buf, sizeof(buf), "%dC", (int)(telem.waterTempC + 0.5f));
    drawItalicText(c, buf, x + 76, y + 58, 32.0f, COL_EMERALD, MC_DATUM);
    c.drawFastVLine(x + w / 2, y + 18, h - 36, 0x2127);
    drawItalicText(c, "FUEL", x + 230, y + 24, 13.0f, COL_SLATE_400, MC_DATUM);
    snprintf(buf, sizeof(buf), "%d%%", (int)(telem.fuelPct + 0.5f));
    drawItalicText(c, buf, x + 230, y + 58, 32.0f, COL_AMBER_LIGHT, MC_DATUM);
  }

  // 下段左: 薄型ステアリングリング＋大型G-BALL (x=10, w=518, h=512)
  if (calcPartOffset(2, -165, 110, 30, dx, dy)) {
    int x = 10 + dx, y = topY + 112 + dy, w = 518, h = 512;
    drawCardSolid(c, x, y, w, h);
    drawItalicText(c, "STEERING & G-BALL", x + 16, y + 22, 16.0f, COL_SLATE_400, ML_DATUM);
    snprintf(buf, sizeof(buf), "%+.0f deg", telem.steerDeg);
    drawItalicText(c, buf, x + w - 16, y + 22, 24.0f, COL_CYAN, MR_DATUM);

    int scx = x + w / 2;
    int scy = y + 272;
    int sR  = 196;
    c.fillArc(scx, scy, sR - 9, sR, 0, 360, 0x6B8E);
    // 赤マーカーは部分更新側でのみ描画する（静的キャッシュに残ると遷移後に二重表示になるため）

    drawGBallWidget(c, scx, scy, 152);
  }

  // 下段右: 極太 BRAKE & THROTTLE (x=538, w=732, h=512)
  if (calcPartOffset(2, 165, 110, 55, dx, dy)) {
    int x = 538 + dx, y = topY + 112 + dy, w = 732, h = 512;
    drawCardSolid(c, x, y, w, h);

    int colW = (w - 30) / 2; // 351pxずつ
    int bx = x + 10;
    int tx = bx + colW + 10;
    int by = y + 10;
    int bh = h - 20;

    // BRAKE
    c.fillRoundRect(bx, by, colW, bh, 8, COL_CARD_DARK);
    c.drawRoundRect(bx, by, colW, bh, 8, COL_ROSE);
    c.drawRoundRect(bx + 1, by + 1, colW - 2, bh - 2, 7, COL_ROSE);
    c.fillRoundRect(bx + 10, by + 10, colW - 20, 44, 6, COL_BLACK);
    drawItalicText(c, "BRAKE", bx + 22, by + 32, 24.0f, COL_ROSE, ML_DATUM);
    snprintf(buf, sizeof(buf), "%.1f MPa", telem.brakeMpa);
    drawItalicText(c, buf, bx + colW - 20, by + 32, 24.0f, COL_AMBER, MR_DATUM);

    int barTop = by + 64, barH = bh - 144, barW = colW - 20;
    c.fillRoundRect(bx + 10, barTop, barW, barH, 6, 0x10A4);
    int bFill = (int)(clampf(telem.brakePct / 100.0f, 0.0f, 1.0f) * barH);
    if (bFill > 0) c.fillRoundRect(bx + 12, barTop + barH - bFill, barW - 4, bFill, 5, COL_ROSE);
    int pkY = barTop + barH - (int)(clampf(telem.brakePeakPct / 100.0f, 0.0f, 1.0f) * barH);
    c.fillRect(bx + 10, pkY - 4, barW, 8, COL_AMBER);

    snprintf(buf, sizeof(buf), "%d%%", (int)(telem.brakePct + 0.5f));
    drawItalicText(c, buf, bx + colW / 2, by + bh - 40, 62.0f, COL_ROSE_LIGHT, MC_DATUM);

    // THROTTLE
    c.fillRoundRect(tx, by, colW, bh, 8, COL_CARD_DARK);
    c.drawRoundRect(tx, by, colW, bh, 8, COL_EMERALD_BRD);
    c.drawRoundRect(tx + 1, by + 1, colW - 2, bh - 2, 7, COL_EMERALD_BRD);
    c.fillRoundRect(tx + 10, by + 10, colW - 20, 44, 6, COL_BLACK);
    drawItalicText(c, "THROTTLE", tx + 22, by + 32, 24.0f, COL_EMERALD, ML_DATUM);
    drawItalicText(c, "TPS", tx + colW - 20, by + 32, 18.0f, COL_SLATE_400, MR_DATUM);

    c.fillRoundRect(tx + 10, barTop, barW, barH, 6, 0x10A4);
    int tFill = (int)(clampf(telem.throttlePct / 100.0f, 0.0f, 1.0f) * barH);
    if (tFill > 0) c.fillRoundRect(tx + 12, barTop + barH - tFill, barW - 4, tFill, 5, COL_GREEN);

    snprintf(buf, sizeof(buf), "%d%%", (int)(telem.throttlePct + 0.5f));
    drawItalicText(c, buf, tx + colW / 2, by + bh - 40, 62.0f, COL_EMERALD, MC_DATUM);
  }
}

// =========================================================================
// PAGE 4: STREET (ホワイトメーター・同心円アーク・箱なしメニュー)
// =========================================================================
static void drawPageP4(M5Canvas& c, bool isRevLimit) {
  char buf[32];
  int dx = 0, dy = 0;
  c.fillRect(0, 0, 1280, 720, COL_BLACK);

  // タコメーター中心 (HTML完全準拠: cx=926, cy=360)
  const int tcx = 926;
  const int tcy = 360;

  // 1) 左側：箱感ゼロのシームレスメニュー＆外気温・時計
  if (calcPartOffset(3, -185, 0, 20, dx, dy)) {
    int mx = 26 + dx;
    int my = dy;

    // 左端の繊細な円弧ライン装飾
    c.drawArc(mx + 250, 360 + my, 270, 268, 130, 230, 0x6B8E);

    // 外気温（56px大文字）
    drawItalicText(c, "OUTSIDE", mx + 54, 52 + my, 16.0f, COL_SLATE_400, ML_DATUM);
    snprintf(buf, sizeof(buf), "%d C", (int)(telem.ambientTempC + 0.5f));
    drawItalicText(c, buf, mx + 148, 52 + my, 52.0f, COL_WHITE, ML_DATUM);

    // メニュー本体（タコメータータップで滑らかに開閉）
    if (p4MenuVis > 0.05f) {
      int slideX = (int)((1.0f - p4MenuVis) * -40.0f);
      int bx = mx + slideX;

      drawJpText(c, "メニュー", bx + 32, 124 + my, 0.85f, COL_SLATE_400, ML_DATUM);
      c.drawFastHLine(bx + 20, 144 + my, 495, 0x31C9);

      static const char* rowTitles[6] = {
        "RACE ダッシュ",
        "Revインジケータ",
        "車両状態",
        "G-BALL",
        "PEDAL",
        "ピット通信"
      };
      char badges[6][20];
      snprintf(badges[0], sizeof(badges[0]), "P1");
      snprintf(badges[1], sizeof(badges[1]), "%d", shiftLimitRpm);
      snprintf(badges[2], sizeof(badges[2]), "%dC / %.1f", (int)telem.oilTempC, telem.oilPressBar);
      snprintf(badges[3], sizeof(badges[3]), "P2");
      snprintf(badges[4], sizeof(badges[4]), "P3");
      snprintf(badges[5], sizeof(badges[5]), "%s",
               (lampState == 3) ? "REC ON" : (lampState == 2) ? "ALL OK" : (lampState == 1) ? "PC WAIT" : "BLE WAIT");

      for (int i = 0; i < 6; i++) {
        int ry = 148 + i * 70 + my;
        bool active = (i == p4SelectedRow);
        if (active) {
          c.fillRect(bx + 12, ry, 508, 34, 0xE73C);
          c.fillRect(bx + 12, ry + 34, 508, 34, 0xBDF7);
          c.drawFastHLine(bx + 12, ry, 508, COL_WHITE);
        } else {
          c.drawFastHLine(bx + 18, ry + 67, 498, 0x296A);
        }

        drawJpText(c, rowTitles[i], bx + 28, ry + 34, 1.15f, active ? COL_BLACK : COL_OFFWHITE, ML_DATUM);

        uint16_t bBg = (i == 5) ? ((lampState == 3) ? COL_ROSE : COL_EMERALD_BRD) : 0x10A4;
        uint16_t bFg = (i == 1) ? COL_CYAN : (i == 3) ? COL_AMBER : (i == 4) ? COL_EMERALD : COL_WHITE;
        int bw = (i == 2) ? 136 : (i == 5) ? 106 : (i == 1) ? 92 : 60;
        c.fillRoundRect(bx + 492 - bw, ry + 14, bw, 40, 6, bBg);
        drawItalicText(c, badges[i], bx + 492 - bw / 2, ry + 34, 22.0f, bFg, MC_DATUM);
        drawItalicText(c, ">", bx + 508, ry + 34, 24.0f, active ? COL_BLACK : COL_SLATE_400, MC_DATUM);
      }
    }

    // 下部：デジタル時計（56px大文字）
    snprintf(buf, sizeof(buf), "%02d:%02d", telem.clockHour, telem.clockMin);
    drawItalicText(c, buf, mx + 54, 668 + my, 54.0f, COL_WHITE, ML_DATUM);
  }

  // 2) 先に右側：特大ホワイトタコメーター (620x620, 中心 926, 360) を描画
  //    （※黒四隅が左側の水温・燃料アークを塗りつぶさないよう必ず先に描く）
  if (calcPartOffset(3, 195, 0, 0, dx, dy)) {
    int cx = tcx + dx;
    int cy = tcy + dy;

    sprTachoBg.pushSprite(&c, cx - 310, cy - 310);

    // 初期針は黒コア外側 (178..255) のみに描き、黒円内に青い根元が残らないようにする
    // （キャッシュ生成時は針なしで描き、実表示直後の部分更新で最初の針を描く）
    snprintf(buf, sizeof(buf), "%d", (int)(telem.speedKmh + 0.5f));
    drawItalicText(c, buf, cx, cy - 78, 108.0f, COL_WHITE, MC_DATUM);
    drawItalicText(c, "km/h", cx, cy - 18, 18.0f, COL_SLATE_400, MC_DATUM);

    c.drawFastHLine(cx - 84, cy - 2, 168, COL_SLATE_700);

    char gStr[4];
    if (telem.gear <= 0) snprintf(gStr, sizeof(gStr), "N");
    else snprintf(gStr, sizeof(gStr), "%d", telem.gear);
    drawItalicText(c, gStr, cx - 36, cy + 46, 82.0f, COL_WHITE, MC_DATUM);

    c.fillRoundRect(cx + 12, cy + 28, 86, 30, 4, COL_WHITE);
    drawItalicText(c, "SPORT", cx + 55, cy + 43, 17.0f, COL_BLACK, MC_DATUM);

    snprintf(buf, sizeof(buf), "TRIP A  %.1f km", telem.tripKm);
    drawItalicText(c, buf, cx, cy + 108, 18.0f, COL_SLATE_300, MC_DATUM);
    drawItalicText(c, "x1000 r/min", cx, cy + 136, 14.0f, 0x6B8E, MC_DATUM);
  }

  // 3) 後に中央左：右タコメーターと完全同心円の湾曲アークゲージ（水温上・燃料下）を描画
  if (calcPartOffset(3, -40, -110, 45, dx, dy)) {
    int ax = tcx + dx;
    int ay = tcy + dy;

    // 水温側（上：192°→242°, R=303..321, 外周ガイドR=333..335）
    c.drawArc(ax, ay, 335, 333, 192, 242, 0x52AA);
    c.fillArc(ax, ay, 321, 303, 192, 242, 0x296A);
    float wPct = clampf((telem.waterTempC - 40.0f) / 90.0f, 0.0f, 1.0f);
    c.fillArc(ax, ay, 321, 303, 192, 192 + 50.0f * wPct, COL_OFFWHITE);
    drawItalicText(c, "130C", ax - 175, 48 + dy, 26.0f, COL_OFFWHITE, MC_DATUM);
    drawItalicText(c, "40",   ax - 350, 324 + dy, 26.0f, COL_SLATE_300, MC_DATUM);

    // 燃料側（下：118°→168°, R=303..321, 外周ガイドR=333..335）
    c.drawArc(ax, ay, 335, 333, 118, 168, 0x52AA);
    c.fillArc(ax, ay, 321, 303, 118, 168, 0x296A);
    float fPct = clampf(telem.fuelPct / 100.0f, 0.0f, 1.0f);
    c.fillArc(ax, ay, 321, 303, 118, 118 + 50.0f * fPct, COL_OFFWHITE);
    drawItalicText(c, "1/1", ax - 352, 418 + dy, 26.0f, COL_SLATE_300, MC_DATUM);
    drawItalicText(c, "E",   ax - 146, 680 + dy, 28.0f, COL_OFFWHITE, MC_DATUM);
  }
}

// --- 小スプライト(sprWidget)の指定領域だけを画面へ安全・高速転送 ---
static void pollTouch(); // 前方宣言（描画の合間にもタッチを取りこぼさない）

static void blitWidget(int dstX, int dstY, int w, int h) {
  M5.Display.startWrite();
  M5.Display.setClipRect(dstX, dstY, w, h);
  sprWidget.pushSprite(dstX, dstY);
  M5.Display.clearClipRect();
  M5.Display.endWrite();
  pollTouch();
}

// --- フルフレーム描画（アニメ中は全体更新、通常時は差分部分更新で爆速動作） ---
static void renderPartialWidgets(bool isRevLimit, int topY) {
  char buf[32];

  // 1) 上部 15連シフトLEDバー (P1〜P3のみ・変化時または点滅時のみ転送)
  if (currentPage != 3) {
    int ledStartRpm = (shiftLimitRpm <= 3500) ? 1000 : 3000;
    float ratio = clampf((float)(telem.rpm - ledStartRpm) / (float)max(1000, shiftLimitRpm - ledStartRpm), 0.0f, 1.0f);
    int activeLeds = (int)(ratio * 15.0f);
    bool blinkOn = ((millis() / 85) % 2) == 0;
    int stateKey = isRevLimit ? (blinkOn ? 100 : 101) : activeLeds;
    if (stateKey != lastDrawnLedCnt) {
      lastDrawnLedCnt = stateKey;
      sprLedBar.fillSprite(COL_CARD_DARK);
      sprLedBar.drawFastHLine(0, 41, 1280, COL_SLATE_700);
      const int gap = 6, padX = 6;
      const int ledW = (1280 - padX * 2 - gap * 14) / 15;
      for (int i = 0; i < 15; i++) {
        int lx = padX + i * (ledW + gap);
        uint16_t col = COL_LED_OFF;
        if (isRevLimit) col = blinkOn ? COL_RED_BRIGHT : 0x5002;
        else if (i < activeLeds) col = (i < 5) ? COL_GREEN : (i < 10) ? COL_YELLOW : COL_RED;
        sprLedBar.fillRoundRect(lx, 5, ledW, 32, 4, col);
        if (col != COL_LED_OFF) sprLedBar.drawFastHLine(lx + 3, 7, ledW - 6, COL_WHITE);
      }
      sprLedBar.pushSprite(0, 0);
      pollTouch();
    }
  }

  // 2) 各ページの動的ウィジェットのみを最小領域で差分更新（全ページ高FPS化）
  if (currentPage == 0) {
    int spdInt = (int)(telem.speedKmh + 0.5f);
    int rpmDisp = (telem.rpm / 20) * 20;
    if (rpmDisp != lastP1Rpm) {
      lastP1Rpm = rpmDisp;
      sprWidget.setClipRect(0, 0, 140, 36);
      sprWidget.fillRect(0, 0, 140, 36, COL_CARD_TOP);
      snprintf(buf, sizeof(buf), "%d", telem.rpm);
      drawItalicText(sprWidget, buf, 136, 18, 30.0f, COL_CYAN, MR_DATUM);
      blitWidget(222 + 413 - 152, topY + 16, 140, 36);
    }
    if (spdInt != lastP1Speed) {
      lastP1Speed = spdInt;
      sprWidget.setClipRect(0, 0, 300, 122);
      sprWidget.fillRect(0, 0, 300, 122, COL_CARD_TOP);
      snprintf(buf, sizeof(buf), "%d", spdInt);
      drawItalicText(sprWidget, buf, 150, 61, 126.0f, COL_WHITE, MC_DATUM);
      blitWidget(222 + 56, topY + 65, 300, 122);
    }

    int lap10 = (int)(telem.lapTimeSec * 10.0f);
    if (lap10 != lastP1Lap10) {
      lastP1Lap10 = lap10;
      sprWidget.setClipRect(0, 0, 336, 86);
      sprWidget.fillRect(0, 0, 336, 86, COL_CARD_TOP);
      formatLapTime(buf, sizeof(buf), telem.lapTimeSec);
      drawItalicText(sprWidget, buf, 168, 43, 72.0f, COL_WHITE, MC_DATUM);
      blitWidget(22, topY + 210 + 89, 336, 86);
    }

    int delta100 = (int)(telem.deltaSec * 100.0f);
    bool fast = (telem.deltaSec <= 0.0f);
    bool signChanged = (lastP1DeltaFast != (int)fast);
    if (signChanged || abs(delta100 - lastP1Delta100) >= 2) {
      lastP1Delta100 = delta100;
      lastP1DeltaFast = (int)fast;
      if (signChanged) {
        sprWidget.setClipRect(0, 0, 255, 262);
        sprWidget.fillRoundRect(0, 0, 255, 262, 12, fast ? COL_BLUE : COL_ROSE);
        sprWidget.drawRoundRect(0, 0, 255, 262, 12, fast ? COL_BLUE_LIGHT : COL_ROSE_LIGHT);
        sprWidget.drawFastHLine(12, 2, 231, COL_WHITE);
        drawItalicText(sprWidget, "BEST D", 14, 22, 18.0f, COL_WHITE, ML_DATUM);
        sprWidget.fillRoundRect(255 - 98, 10, 88, 24, 4, fast ? COL_BLUE_DARK : 0xA007);
        drawItalicText(sprWidget, "REF: BEST", 255 - 54, 22, 15.0f, COL_WHITE, MC_DATUM);
        formatDelta(buf, sizeof(buf), telem.deltaSec);
        drawItalicText(sprWidget, buf, 127, 134, 66.0f, COL_WHITE, MC_DATUM);
        drawItalicText(sprWidget, fast ? "FASTER TIME" : "SLOWER TIME", 127, 262 - 18, 17.0f, COL_WHITE, MC_DATUM);
        blitWidget(380, topY + 210, 255, 262);
      } else {
        sprWidget.setClipRect(0, 0, 230, 82);
        sprWidget.fillRect(0, 0, 230, 82, fast ? COL_BLUE : COL_ROSE);
        formatDelta(buf, sizeof(buf), telem.deltaSec);
        drawItalicText(sprWidget, buf, 115, 41, 66.0f, COL_WHITE, MC_DATUM);
        blitWidget(380 + 12, topY + 210 + 93, 230, 82);
      }
    }

    // 1秒に1回だけセッションタイマー＆時計の2カードだけを小さく更新
    if (telem.clockSec != lastDrawnSec) {
      lastDrawnSec = telem.clockSec;
      sprWidget.setClipRect(0, 0, 175, 58);
      sprWidget.fillRect(0, 0, 175, 58, COL_CARD_TOP);
      snprintf(buf, sizeof(buf), "%02d:%02d", telem.sessionSec / 60, telem.sessionSec % 60);
      drawItalicText(sprWidget, buf, 87, 29, 48.0f, COL_AMBER_LIGHT, MC_DATUM);
      blitWidget(221 + 13, topY + 482 + 45, 175, 58);

      sprWidget.setClipRect(0, 0, 110, 32);
      sprWidget.fillRect(0, 0, 110, 32, COL_CARD_TOP);
      snprintf(buf, sizeof(buf), "%02d:%02d", telem.clockHour, telem.clockMin);
      drawItalicText(sprWidget, buf, 106, 16, 26.0f, COL_WHITE, MR_DATUM);
      blitWidget(432 + 85, topY + 482 + 8, 110, 32);
    }

    sprWidget.clearClipRect();
  } else if (currentPage == 1) {
    // P2: G-BALL本体 (428x428)
    sprWidget.setClipRect(0, 0, 428, 428);
    sprWidget.fillRect(0, 0, 428, 428, COL_CARD_TOP);
    drawGBallWidget(sprWidget, 214, 214, 208);
    blitWidget(645 + 312 - 214, topY + 10 + 298 - 214, 428, 428);

    // P2: 下部 LAT/LONG G 数値のみ
    sprWidget.setClipRect(0, 0, 380, 36);
    sprWidget.fillRect(0, 0, 380, 36, COL_CARD_DARK);
    float lat = telem.latG - gZeroLat, lon = telem.lonG - gZeroLon;
    snprintf(buf, sizeof(buf), "%s %.2f G / %s %.2f G",
             (lat >= 0 ? "R" : "L"), fabsf(lat), (lon >= 0 ? "ACC" : "BRK"), fabsf(lon));
    drawItalicText(sprWidget, buf, 376, 18, 24.0f, COL_CYAN, MR_DATUM);
    blitWidget(645 + 625 - 410, topY + 10 + 614 - 56, 380, 36);

    // P2: 左上 SPEED 数値のみ
    int spdInt = (int)(telem.speedKmh + 0.5f);
    if (spdInt != lastP1Speed) {
      lastP1Speed = spdInt;
      sprWidget.setClipRect(0, 0, 360, 74);
      sprWidget.fillRect(0, 0, 360, 74, 0x10A4);
      snprintf(buf, sizeof(buf), "%d km/h", spdInt);
      drawItalicText(sprWidget, buf, 354, 37, 56.0f, COL_WHITE, MR_DATUM);
      blitWidget(26 + 593 - 376, topY + 70 + 24, 360, 74);
    }

    sprWidget.clearClipRect();
  } else if (currentPage == 2) {
    // P3: SPEED & GEAR（変化時のみ最小領域更新）
    int spdInt = (int)(telem.speedKmh + 0.5f);
    if (spdInt != lastP3Speed) {
      lastP3Speed = spdInt;
      sprWidget.setClipRect(0, 0, 170, 76);
      sprWidget.fillRect(0, 0, 170, 76, 0x0910);
      snprintf(buf, sizeof(buf), "%d", spdInt);
      drawItalicText(sprWidget, buf, 4, 38, 72.0f, COL_WHITE, ML_DATUM);
      blitWidget(433 + 18, topY + 18, 170, 76);
    }
    if (telem.gear != lastP3Gear) {
      lastP3Gear = telem.gear;
      sprWidget.setClipRect(0, 0, 68, 56);
      sprWidget.fillRect(0, 0, 68, 56, 0x0910);
      snprintf(buf, sizeof(buf), "%d", telem.gear);
      drawItalicText(sprWidget, buf, 62, 28, 50.0f, COL_AMBER, MR_DATUM);
      blitWidget(433 + 520 - 94, topY + 40, 68, 56);
    }

    // P3: 内側G-BALL(316x316)転送（四隅がステアリングリングを削らないようリングの弧も同時描画！）
    int scx = 10 + 259, scy = topY + 112 + 272, sR = 196;
    sprWidget.setClipRect(0, 0, 316, 316);
    sprWidget.fillRect(0, 0, 316, 316, COL_CARD_TOP);
    sprWidget.fillArc(158, 158, sR, sR - 9, 0, 360, 0x6B8E); // 四隅にかかるリングの縁を保護
    drawGBallWidget(sprWidget, 158, 158, 152);
    blitWidget(scx - 158, scy - 158, 316, 316);

    // P3: ステアリング外周赤マーカー（G-BALL転送後に描くことで角付近でも絶対に欠けない）
    if (fabsf(telem.steerDeg - lastP3SteerDeg) >= 1.0f || lastP3SteerDeg < -900.0f) {
      M5.Display.startWrite();
      if (lastP3SteerDeg > -900.0f) {
        float oldRad = (-90.0f + lastP3SteerDeg) * DEG_TO_RAD;
        int ox = scx + (int)(cosf(oldRad) * (sR - 4));
        int oy = scy + (int)(sinf(oldRad) * (sR - 4));
        // 滲みも含めて余裕を持って完全消去
        M5.Display.fillCircle(ox, oy, 15, COL_CARD_TOP);
        // 0度跨ぎでも確実にリングを復元できるよう角度を正規化
        float oldDeg = fmodf(-90.0f + lastP3SteerDeg, 360.0f);
        if (oldDeg < 0.0f) oldDeg += 360.0f;
        float a0 = oldDeg - 14.0f;
        float a1 = oldDeg + 14.0f;
        if (a0 < 0.0f) {
          M5.Display.fillArc(scx, scy, sR, sR - 9, a0 + 360.0f, 360.0f, 0x6B8E);
          M5.Display.fillArc(scx, scy, sR, sR - 9, 0.0f, a1, 0x6B8E);
        } else if (a1 > 360.0f) {
          M5.Display.fillArc(scx, scy, sR, sR - 9, a0, 360.0f, 0x6B8E);
          M5.Display.fillArc(scx, scy, sR, sR - 9, 0.0f, a1 - 360.0f, 0x6B8E);
        } else {
          M5.Display.fillArc(scx, scy, sR, sR - 9, a0, a1, 0x6B8E);
        }
      }
      float aRad = (-90.0f + telem.steerDeg) * DEG_TO_RAD;
      int mx = scx + (int)(cosf(aRad) * (sR - 4));
      int my = scy + (int)(sinf(aRad) * (sR - 4));
      M5.Display.fillSmoothCircle(mx, my, 12, COL_WHITE);
      M5.Display.fillSmoothCircle(mx, my, 10, COL_ROSE);
      M5.Display.endWrite();
      lastP3SteerDeg = telem.steerDeg;
      pollTouch();
    }

    // P3: BRAKE & THROTTLE — バーは前回高さとの差分バンドだけM5.Displayへ直接塗る（爆速1ms！）
    int colW = 351, bh = 492, barTop = 64, barH = bh - 144, barW = colW - 20;
    int bx0 = 548, tx0 = 548 + colW + 10, by0 = topY + 122;
    int bFill = (int)(clampf(telem.brakePct / 100.0f, 0.0f, 1.0f) * barH);
    int pkY   = max(0, min(barH - 8, barH - (int)(clampf(telem.brakePeakPct / 100.0f, 0.0f, 1.0f) * barH) - 4));
    int tFill = (int)(clampf(telem.throttlePct / 100.0f, 0.0f, 1.0f) * barH);

    M5.Display.startWrite();
    // BRAKE バー差分描画
    if (lastP3BrakeFill < 0) {
      M5.Display.fillRect(bx0 + 10, by0 + barTop, barW, barH, 0x10A4);
      if (bFill > 0) M5.Display.fillRect(bx0 + 12, by0 + barTop + barH - bFill, barW - 4, bFill, COL_ROSE);
    } else if (bFill != lastP3BrakeFill) {
      if (bFill > lastP3BrakeFill) {
        int dh = bFill - lastP3BrakeFill;
        M5.Display.fillRect(bx0 + 12, by0 + barTop + barH - bFill, barW - 4, dh, COL_ROSE);
      } else {
        int dh = lastP3BrakeFill - bFill;
        M5.Display.fillRect(bx0 + 12, by0 + barTop + barH - lastP3BrakeFill, barW - 4, dh, 0x10A4);
      }
    }
    if (lastP3BrakePeak >= 0 && lastP3BrakePeak != pkY) {
      // 旧ピークバー消去（バー内部か背景色で塗り戻し）
      int fillTop = barH - bFill;
      uint16_t restCol = (lastP3BrakePeak >= fillTop) ? COL_ROSE : 0x10A4;
      M5.Display.fillRect(bx0 + 10, by0 + barTop + lastP3BrakePeak, barW, 8, restCol);
    }
    M5.Display.fillRect(bx0 + 10, by0 + barTop + pkY, barW, 8, COL_AMBER);
    lastP3BrakeFill = bFill;
    lastP3BrakePeak = pkY;

    // THROTTLE バー差分描画
    if (lastP3ThrFill < 0) {
      M5.Display.fillRect(tx0 + 10, by0 + barTop, barW, barH, 0x10A4);
      if (tFill > 0) M5.Display.fillRect(tx0 + 12, by0 + barTop + barH - tFill, barW - 4, tFill, COL_GREEN);
    } else if (tFill != lastP3ThrFill) {
      if (tFill > lastP3ThrFill) {
        int dh = tFill - lastP3ThrFill;
        M5.Display.fillRect(tx0 + 12, by0 + barTop + barH - tFill, barW - 4, dh, COL_GREEN);
      } else {
        int dh = lastP3ThrFill - tFill;
        M5.Display.fillRect(tx0 + 12, by0 + barTop + barH - lastP3ThrFill, barW - 4, dh, 0x10A4);
      }
    }
    lastP3ThrFill = tFill;
    M5.Display.endWrite();
    pollTouch();

    int bPctInt = (int)(telem.brakePct + 0.5f);
    if (bPctInt != lastP3BrakePct) {
      lastP3BrakePct = bPctInt;
      sprWidget.setClipRect(0, 0, 170, 36);
      sprWidget.fillRect(0, 0, 170, 36, COL_BLACK);
      snprintf(buf, sizeof(buf), "%.1f MPa", telem.brakeMpa);
      drawItalicText(sprWidget, buf, 166, 18, 24.0f, COL_AMBER, MR_DATUM);
      blitWidget(bx0 + colW - 186, by0 + 14, 170, 36);

      sprWidget.setClipRect(0, 0, 210, 68);
      sprWidget.fillRect(0, 0, 210, 68, COL_CARD_DARK);
      snprintf(buf, sizeof(buf), "%d%%", bPctInt);
      drawItalicText(sprWidget, buf, 105, 34, 62.0f, COL_ROSE_LIGHT, MC_DATUM);
      blitWidget(bx0 + (colW - 210) / 2, by0 + bh - 74, 210, 68);
    }

    int tPctInt = (int)(telem.throttlePct + 0.5f);
    if (tPctInt != lastP3ThrPct) {
      lastP3ThrPct = tPctInt;
      sprWidget.setClipRect(0, 0, 210, 68);
      sprWidget.fillRect(0, 0, 210, 68, COL_CARD_DARK);
      snprintf(buf, sizeof(buf), "%d%%", tPctInt);
      drawItalicText(sprWidget, buf, 105, 34, 62.0f, COL_EMERALD, MC_DATUM);
      blitWidget(tx0 + (colW - 210) / 2, by0 + bh - 74, 210, 68);
    }

    sprWidget.clearClipRect();
  } else if (currentPage == 3) {
    // P4: 620x620全体転送を廃止！針の外接矩形復元＋中央車速/ギアのみ最小更新で超高速化！
    const int tcx = 926, tcy = 360;
    int rpmStep = telem.rpm / 15;
    if (rpmStep != lastP4RpmStep) {
      lastP4RpmStep = rpmStep;
      float rpmK = clampf(telem.rpm / 1000.0f, 0.0f, 10.0f);
      float newDeg = rpmToLfaDeg(rpmK);
      M5.Display.startWrite();
      // 旧針の最小外接矩形だけ静的文字盤キャッシュ(sprTachoBg)から完璧に復元
      if (lastP4NeedleDeg > -900.0f) {
        float oRad = lastP4NeedleDeg * DEG_TO_RAD;
        int ox1 = tcx + (int)(cosf(oRad) * 177);
        int oy1 = tcy + (int)(sinf(oRad) * 177);
        int ox2 = tcx + (int)(cosf(oRad) * 257);
        int oy2 = tcy + (int)(sinf(oRad) * 257);
        int rx0 = min(ox1, ox2) - 10;
        int ry0 = min(oy1, oy2) - 10;
        int rw  = abs(ox2 - ox1) + 20;
        int rh  = abs(oy2 - oy1) + 20;
        M5.Display.setClipRect(rx0, ry0, rw, rh);
        sprTachoBg.pushSprite(tcx - 310, tcy - 310);
        M5.Display.clearClipRect();
      }
      // 新しい針を直接描画（中央黒コアの外側 178..255 に描くので数字と絶対に干渉しない）
      float nRad = newDeg * DEG_TO_RAD;
      uint16_t nCol = isRevLimit ? COL_RED_BRIGHT : COL_BLUE;
      M5.Display.drawWideLine(tcx + (int)(cosf(nRad) * 178), tcy + (int)(sinf(nRad) * 178),
                              tcx + (int)(cosf(nRad) * 255), tcy + (int)(sinf(nRad) * 255), 8, nCol);
      M5.Display.endWrite();
      lastP4NeedleDeg = newDeg;
      pollTouch();
    }

    int spdInt = (int)(telem.speedKmh + 0.5f);
    if (spdInt != lastP4Speed) {
      lastP4Speed = spdInt;
      sprWidget.setClipRect(0, 0, 230, 108);
      sprWidget.fillRect(0, 0, 230, 108, COL_BLACK);
      snprintf(buf, sizeof(buf), "%d", spdInt);
      drawItalicText(sprWidget, buf, 115, 54, 108.0f, COL_WHITE, MC_DATUM);
      blitWidget(tcx - 115, tcy - 78 - 54, 230, 108);
    }

    if (telem.gear != lastP4Gear) {
      lastP4Gear = telem.gear;
      sprWidget.setClipRect(0, 0, 88, 84);
      sprWidget.fillRect(0, 0, 88, 84, COL_BLACK);
      char gStr[4];
      if (telem.gear <= 0) snprintf(gStr, sizeof(gStr), "N");
      else snprintf(gStr, sizeof(gStr), "%d", telem.gear);
      drawItalicText(sprWidget, gStr, 44, 42, 82.0f, COL_WHITE, MC_DATUM);
      blitWidget(tcx - 36 - 44, tcy + 46 - 42, 88, 84);
    }

    sprWidget.clearClipRect();
  }

  // 3) レブリミット全画面外周・極太フラッシュ（点灯時は18px赤＋白枠、消灯時はキャッシュから綺麗に復元）
  bool revFlashNow = isRevLimit && (((millis() / 85) % 2) == 0);
  if (revFlashNow != prevRevFlash) {
    prevRevFlash = revFlashNow;
    const int fw = 18;
    M5.Display.startWrite();
    if (revFlashNow) {
      // 外周18pxを鮮やかな赤でフラッシュ＋内縁に白ハイライト
      M5.Display.fillRect(0, 0, 1280, fw, COL_RED_BRIGHT);
      M5.Display.fillRect(0, 720 - fw, 1280, fw, COL_RED_BRIGHT);
      M5.Display.fillRect(0, fw, fw, 720 - fw * 2, COL_RED_BRIGHT);
      M5.Display.fillRect(1280 - fw, fw, fw, 720 - fw * 2, COL_RED_BRIGHT);
      M5.Display.drawRect(fw - 3, fw - 3, 1280 - (fw - 3) * 2, 720 - (fw - 3) * 2, COL_WHITE);
      M5.Display.drawRect(fw - 2, fw - 2, 1280 - (fw - 2) * 2, 720 - (fw - 2) * 2, COL_WHITE);
      // P4時は中央タコメーターLCDリングも赤フラッシュ
      if (currentPage == 3) {
        M5.Display.drawCircle(926, 360, 176, COL_RED_BRIGHT);
        M5.Display.drawCircle(926, 360, 175, COL_RED_BRIGHT);
        M5.Display.drawCircle(926, 360, 174, COL_WHITE);
      }
    } else {
      // 消灯時は事前キャッシュから四辺18pxを綺麗に復元（カード端の欠けなし）
      M5.Display.setClipRect(0, 0, 1280, fw);
      sprPageCache[currentPage].pushSprite(0, 0);
      M5.Display.setClipRect(0, 720 - fw, 1280, fw);
      sprPageCache[currentPage].pushSprite(0, 0);
      M5.Display.setClipRect(0, fw, fw, 720 - fw * 2);
      sprPageCache[currentPage].pushSprite(0, 0);
      M5.Display.setClipRect(1280 - fw, fw, fw, 720 - fw * 2);
      sprPageCache[currentPage].pushSprite(0, 0);
      M5.Display.clearClipRect();
      if (currentPage == 3) {
        M5.Display.drawCircle(926, 360, 176, 0x296A);
        M5.Display.drawCircle(926, 360, 175, 0x18E5);
        M5.Display.drawCircle(926, 360, 174, COL_BLACK);
      }
      lastDrawnLedCnt = -99; // 上端LEDバーを即再同期
    }
    M5.Display.endWrite();
  }
}

static void renderFullFrame() {
  bool isRevLimit = (telem.rpm >= shiftLimitRpm);
  int  pageTopY   = (currentPage == 3) ? 0 : 86;
  p4MenuVis       = p4MenuOpen ? 1.0f : 0.0f;

  // 1) 中身入りのまま分解＆収束し、着地した瞬間にそのまま完了するトランジション
  if (isTransitioning) {
    uint32_t now = millis();

    if (transPhase == 0) {
      // フェーズ0：現ページのパーツを中身ごと中央から四方八方へ広げる
      uint16_t bgLeave = (transFromPage == 3) ? COL_BLACK : COL_BG;
      if ((now - transPhaseStartMs) < TRANS_LEAVE_MS) {
        drawTransitionAnimatedFrame(transFromPage, bgLeave, false);
        transLeaveFrames++;
        transFrameCnt++;
        return;
      }
      // 退場完了 → P4→P1-P3時は上部ヘッダーをここで1回描画
      if (transFromPage == 3 && transToPage != 3) {
        M5.Display.startWrite();
        M5.Display.setClipRect(0, 0, 1280, 86);
        sprPageCache[transToPage].pushSprite(0, 0);
        M5.Display.clearClipRect();
        M5.Display.endWrite();
      }
      // フェーズ1のタイマーをここで独立リセット（退場時間が長引いても入場アニメを削らない）
      transPhase = 1;
      transPhaseStartMs = millis();
      return;
    }

    if (transPhase == 1) {
      // フェーズ1：次ページのパーツを中身ごと四方八方から中央へ戻す
      uint16_t bgEnter = (transToPage == 3) ? COL_BLACK : COL_BG;
      if ((now - transPhaseStartMs) < TRANS_ENTER_MS) {
        drawTransitionAnimatedFrame(transToPage, bgEnter, false);
        transEnterFrames++;
        transFrameCnt++;
        return;
      }
      // 最終着地フレーム（dx=0, dy=0）を中身ごとピタッと描いてそのまま完了！（終了後の全画面上書きなし！）
      drawTransitionAnimatedFrame(transToPage, bgEnter, true);
      transEnterFrames++;
      transFrameCnt++;
      isTransitioning = false;
      resetPartialCaches();
      uint32_t totMs = millis() - transStartMs;
      Serial.printf("[TRANS] done: frames=%d (leave=%d enter=%d) elapsed=%ums avg=%ums/f\n",
                    transFrameCnt, transLeaveFrames, transEnterFrames,
                    (unsigned)totMs, (unsigned)(totMs / max(1, transFrameCnt)));
      return;
    }
  }

  // 2) 通常走行時は爆速部分更新パスを使用
  if (!staticDirty) {
    renderPartialWidgets(isRevLimit, pageTopY);
    return;
  }

  // 3) 静的再描画（起動初回やコース切替・設定変更時のみキャッシュ更新してタイル高速転送）
  ensurePageCache(currentPage, isRevLimit);
  staticDirty = false;
  resetPartialCaches();
  pushFrameTiled(sprPageCache[currentPage], 0);
}

// --- デモ走行シミュレーション（キビキビ動くサーキット加速・シフト・フルブレーキ再現） ---
static void updateDemoSim(float dt) {
  static float simTime = 0.0f;
  static float secAcc  = 0.0f;
  simTime += dt;
  secAcc  += dt;

  if (secAcc >= 1.0f) {
    secAcc -= 1.0f;
    telem.clockSec++;
    if (telem.clockSec >= 60) { telem.clockSec = 0; telem.clockMin++; }
    if (telem.clockMin >= 60) { telem.clockMin = 0; telem.clockHour = (telem.clockHour + 1) % 24; }
    if (telem.sessionSec > 0) telem.sessionSec--;
  }

  // 10秒周期で「2→3→4→5速フル加速 → フルブレーキング＆シフトダウン → コーナー立ち上がり」を再現
  float cyc = fmodf(simTime, 10.0f);
  float targetRpm = 3500.0f;
  float targetSpd = 75.0f;
  float targetThr = 0.0f;
  float targetBrk = 0.0f;
  float targetLat = 0.0f;
  float targetLon = 0.0f;

  if (cyc < 1.8f) {
    // 2速 全開加速 (0.0 - 1.8s)
    float u = cyc / 1.8f;
    telem.gear = 2;
    targetRpm = 4200.0f + u * 3400.0f;
    targetSpd = 68.0f + u * 44.0f;
    targetThr = 96.0f;
    targetBrk = 0.0f;
    targetLat = (1.0f - u) * 0.85f;
    targetLon = 0.55f;
  } else if (cyc < 3.8f) {
    // 3速 全開加速 (1.8 - 3.8s)
    float u = (cyc - 1.8f) / 2.0f;
    telem.gear = 3;
    targetRpm = 5100.0f + u * 2550.0f;
    targetSpd = 112.0f + u * 42.0f;
    targetThr = (u < 0.08f) ? 15.0f : 100.0f; // シフト一瞬オフ
    targetBrk = 0.0f;
    targetLat = sinf(u * 3.14f) * -0.25f;
    targetLon = 0.48f;
  } else if (cyc < 6.0f) {
    // 4速 ストレート全開〜レブ付近 (3.8 - 6.0s)
    float u = (cyc - 3.8f) / 2.2f;
    telem.gear = 4;
    targetRpm = 5400.0f + u * 2300.0f;
    targetSpd = 154.0f + u * 38.0f;
    targetThr = (u < 0.07f) ? 10.0f : 100.0f;
    targetBrk = 0.0f;
    targetLat = 0.05f;
    targetLon = 0.36f;
  } else if (cyc < 7.8f) {
    // ハードブレーキング & ブリッピングシフトダウン (6.0 - 7.8s)
    float u = (cyc - 6.0f) / 1.8f;
    targetThr = (u > 0.38f && u < 0.48f) ? 45.0f : (u > 0.72f && u < 0.82f) ? 40.0f : 0.0f;
    targetBrk = (u < 0.15f) ? (u / 0.15f) * 98.0f : max(15.0f, 98.0f - (u - 0.15f) * 85.0f);
    targetSpd = 192.0f - u * 116.0f;
    if (u < 0.42f)      { telem.gear = 4; targetRpm = 7500.0f - u * 4500.0f; }
    else if (u < 0.76f) { telem.gear = 3; targetRpm = 6800.0f - (u - 0.42f) * 5200.0f; }
    else                { telem.gear = 2; targetRpm = 6200.0f - (u - 0.76f) * 4200.0f; }
    targetLat = -u * 0.95f;
    targetLon = -(targetBrk / 100.0f) * 1.25f;
  } else {
    // コーナリング〜立ち上がり (7.8 - 10.0s)
    float u = (cyc - 7.8f) / 2.2f;
    telem.gear = 2;
    targetBrk = (u < 0.35f) ? (1.0f - u / 0.35f) * 25.0f : 0.0f;
    targetThr = (u > 0.30f) ? clampf((u - 0.30f) * 140.0f, 0.0f, 92.0f) : 0.0f;
    targetSpd = 76.0f - sinf(u * 3.14f) * 12.0f + u * 6.0f;
    targetRpm = 4000.0f + (targetThr / 100.0f) * 1200.0f;
    targetLat = -1.25f * sinf(u * 3.14159f) + u * 0.85f;
    targetLon = (targetThr > 0.0f) ? (targetThr / 100.0f) * 0.45f : -0.35f;
  }

  telem.rpm         = (int)lerpf((float)telem.rpm, targetRpm, min(1.0f, dt * 16.0f));
  telem.speedKmh    = lerpf(telem.speedKmh, targetSpd, min(1.0f, dt * 12.0f));
  telem.throttlePct = lerpf(telem.throttlePct, targetThr, min(1.0f, dt * 18.0f));
  telem.brakePct    = lerpf(telem.brakePct, targetBrk, min(1.0f, dt * 20.0f));
  telem.brakeMpa    = (telem.brakePct / 100.0f) * 11.5f;

  if (telem.brakePct > telem.brakePeakPct) telem.brakePeakPct = telem.brakePct;
  else telem.brakePeakPct = max(telem.brakePct, telem.brakePeakPct - dt * 18.0f);

  telem.latG     = lerpf(telem.latG, targetLat, min(1.0f, dt * 12.0f));
  telem.lonG     = lerpf(telem.lonG, targetLon, min(1.0f, dt * 14.0f));
  telem.steerDeg = clampf(telem.latG * 58.0f, -75.0f, 75.0f);

  telem.lapTimeSec += dt;
  telem.tripKm += telem.speedKmh * (dt / 3600.0f);
  telem.lapDistM = (telem.lapDistM + (int)(telem.speedKmh * dt * 0.277f)) % (int)CIRCUITS[circuitIdx].lengthM;
  if (telem.lapTimeSec >= 75.50f) {
    telem.lastLapSec = telem.lapTimeSec;
    if (telem.lastLapSec < telem.bestLapSec) telem.bestLapSec = telem.lastLapSec;
    telem.lapTimeSec = 0.0f;
    telem.lapCount++;
  }
  telem.deltaSec = sinf(simTime * 0.7f) * 0.45f;

  static float trailAcc = 0.0f;
  trailAcc += dt;
  if (trailAcc >= 0.04f) {
    trailAcc = 0.0f;
    if (gTrailCount < G_TRAIL_MAX) {
      gTrail[gTrailCount++] = {telem.latG - gZeroLat, telem.lonG - gZeroLon};
    } else {
      for (int i = 0; i < G_TRAIL_MAX - 1; i++) gTrail[i] = gTrail[i + 1];
      gTrail[G_TRAIL_MAX - 1] = {telem.latG - gZeroLat, telem.lonG - gZeroLon};
    }
  }
}

// --- CAN (AiM Solo 2 DL) ポーリング ---
static void pollCanBus() {
  if (!canOk) return;
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) {
    isDemoMode = false; // 実車CANを受信したら自動でCANモードへ
    if (msg.identifier == 0x420 && msg.data_length_code >= 8) {
      telem.rpm        = (int)((msg.data[0] << 8) | msg.data[1]);
      telem.speedKmh   = ((msg.data[2] << 8) | msg.data[3]) * 0.1f;
      telem.waterTempC = ((int16_t)((msg.data[6] << 8) | msg.data[7])) * 0.1f;
    } else if (msg.identifier == 0x421 && msg.data_length_code >= 8) {
      telem.fuelPct    = clampf((float)((msg.data[0] << 8) | msg.data[1]), 0.0f, 100.0f);
      telem.lapCount   = (int)((msg.data[2] << 8) | msg.data[3]);
      telem.lapTimeSec = ((msg.data[4] << 8) | msg.data[5]) * 0.01f;
      telem.deltaSec   = ((int16_t)((msg.data[6] << 8) | msg.data[7])) * 0.01f;
    } else if (msg.identifier == 0x422 && msg.data_length_code >= 8) {
      telem.throttlePct = clampf(((msg.data[2] << 8) | msg.data[3]) * 0.1f, 0.0f, 100.0f);
      telem.brakePct    = clampf(((msg.data[4] << 8) | msg.data[5]) * 0.1f, 0.0f, 100.0f);
      telem.brakeMpa    = telem.brakePct * 0.115f;
      telem.lastLapSec  = ((msg.data[6] << 8) | msg.data[7]) * 0.01f;
    } else if (msg.identifier == 0x423 && msg.data_length_code >= 8) {
      telem.steerDeg = ((int16_t)((msg.data[0] << 8) | msg.data[1])) * 0.1f;
      telem.latG     = ((int16_t)((msg.data[4] << 8) | msg.data[5])) * 0.01f;
      telem.lonG     = ((int16_t)((msg.data[6] << 8) | msg.data[7])) * 0.01f;
    }
  }
}

// --- タッチ＆スワイプ処理 ---
static void handleTap(int x, int y) {
  Serial.printf("[TAP] page=%d x=%d y=%d\n", currentPage, x, y);
  staticDirty = true;

  // P4 STREET 操作
  if (currentPage == 3) {
    if (p4MenuOpen && x >= 24 && x <= 540 && y >= 145 && y <= 575) {
      int row = (y - 148) / 70;
      if (row >= 0 && row < 6) {
        p4SelectedRow = row;
        if (row == 0)      switchPage(0);
        else if (row == 3) switchPage(1);
        else if (row == 4) switchPage(2);
        else if (row == 1) {
          int nextIdx = 0;
          for (int k = 0; k < REV_PRESET_COUNT; k++) {
            if (REV_PRESETS[k] > shiftLimitRpm) { nextIdx = k; break; }
          }
          shiftLimitRpm = REV_PRESETS[nextIdx];
          rebuildTachoCache();
          markAllPageCachesDirty();
          saveSettings();
          beepClick(1318, 35);
        } else if (row == 5) {
          lampState = (lampState + 1) % 4;
          markAllPageCachesDirty();
          beepClick(1046, 35);
        } else {
          pageCacheDirty[3] = true;
          beepClick(880, 30);
        }
        return;
      }
    }
    // 右側タコメーター領域タップで左メニュー開閉
    if (x >= 600) {
      p4MenuOpen = !p4MenuOpen;
      pageCacheDirty[3] = true;
      beepClick(987, 30);
    }
    return;
  }

  // P1〜P3 コントロールバー (y = 42..88)
  if (y >= 40 && y <= 90) {
    if (x >= 10 && x <= 130)      { switchPage(0); return; }
    if (x >= 132 && x <= 252)     { switchPage(1); return; }
    if (x >= 254 && x <= 372)     { switchPage(2); return; }
    if (x >= 374 && x <= 496)     { switchPage(3); return; }

    if (x >= 604 && x <= 640) {
      shiftLimitRpm = max(3000, shiftLimitRpm - 250);
      rebuildTachoCache();
      markAllPageCachesDirty();
      saveSettings();
      beepClick(880, 30);
      return;
    }
    if (x >= 690 && x <= 728) {
      shiftLimitRpm = min(9000, shiftLimitRpm + 250);
      rebuildTachoCache();
      markAllPageCachesDirty();
      saveSettings();
      beepClick(1174, 30);
      return;
    }
    // 画面輝度 (BRT) 段階切替: 25% -> 45% -> 65% -> 85% -> 100%
    if (x >= 732 && x <= 836) {
      static const int BRT_STEPS[5] = {25, 45, 65, 85, 100};
      int nextBrt = BRT_STEPS[0];
      for (int k = 0; k < 5; k++) {
        if (BRT_STEPS[k] > brightnessPct) { nextBrt = BRT_STEPS[k]; break; }
      }
      brightnessPct = nextBrt;
      M5.Display.setBrightness((uint8_t)((brightnessPct * 255) / 100));
      markAllPageCachesDirty();
      saveSettings();
      beepClick(1046, 30);
      return;
    }
    if (x >= 1000 && x <= 1134) {
      lampState = (lampState + 1) % 4;
      markAllPageCachesDirty();
      beepClick(1046, 30);
      return;
    }
    if (x >= 1138 && x <= 1272) {
      isDemoMode = !isDemoMode;
      markAllPageCachesDirty();
      beepClick(1318, 35);
      return;
    }
  }

  // P1: LAPリセット & コース切替
  if (currentPage == 0) {
    if (x >= 10 && x <= 215 && y >= 96 && y <= 286) {
      telem.lapCount = 0;
      telem.lapTimeSec = 0.0f;
      pageCacheDirty[0] = true;
      beepClick(784, 40);
      return;
    }
    if (x >= 900 && x <= 1265 && y >= 96 && y <= 150) {
      circuitIdx = (circuitIdx + 1) % CIRCUIT_COUNT;
      rebuildCarbonCache();
      markAllPageCachesDirty();
      saveSettings();
      beepClick(1174, 35);
      return;
    }
  }

  // P2: G-BALL ゼロ点較正
  if (currentPage == 1 && x >= 645 && y >= 96) {
    gZeroLat = telem.latG;
    gZeroLon = telem.lonG;
    gTrailCount = 0;
    beepClick(1318, 45);
    return;
  }
}

static void pollTouch() {
  M5.update();
  auto cnt = M5.Touch.getCount();
  if (cnt > 0) {
    auto det = M5.Touch.getDetail(0);
    if (!touchTracking) {
      touchTracking = true;
      touchConsumed = false;
      touchStartX = touchLastX = det.x;
      touchStartY = touchLastY = det.y;
      touchMoved = false;
      Serial.printf("[TOUCH DOWN] x=%d y=%d\n", det.x, det.y);

      // 上部コントロールバー(P1-P3)とP4左メニューは触れた瞬間に即反応！
      if ((currentPage != 3 && det.y <= 104) ||
          (currentPage == 3 && p4MenuOpen && det.x >= 24 && det.x <= 540 && det.y >= 145 && det.y <= 575)) {
        touchConsumed = true;
        handleTap(det.x, det.y);
      }
    } else {
      touchLastX = det.x;
      touchLastY = det.y;
      int dx = touchLastX - touchStartX;
      int dy = touchLastY - touchStartY;
      if (!touchConsumed && (abs(dx) > 18 || abs(dy) > 18)) {
        touchMoved = true;
      }
      // スワイプは指を離すのを待たず、65px以上動いた瞬間に即ページ切替！
      if (!touchConsumed && abs(dx) > 65 && abs(dx) > abs(dy) * 1.2f) {
        touchConsumed = true;
        if (dx < 0) switchPage(currentPage + 1);
        else        switchPage(currentPage - 1);
      }
    }
  } else if (touchTracking) {
    bool wasConsumed = touchConsumed;
    touchTracking = false;
    touchConsumed = false;
    if (!wasConsumed && !touchMoved) {
      handleTap(touchStartX, touchStartY);
    }
  }
}

static bool drawSplashImageFromSd(const char* path, bool isPng) {
  if (!SD_MMC.exists(path)) return false;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  if (sz < 16) { f.close(); return false; }
  uint8_t* buf = (uint8_t*)ps_malloc(sz);
  if (!buf) { f.close(); return false; }
  size_t rd = f.read(buf, sz);
  f.close();
  bool ok = false;
  if (rd == sz) {
    Serial.printf("[SPLASH] Drawing %s (%u bytes)\n", path, (unsigned)sz);
    if (isPng) {
      ok = M5.Display.drawPng(buf, sz, 0, 0, 1280, 720, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    } else {
      ok = M5.Display.drawJpg(buf, sz, 0, 0, 1280, 720, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    }
  }
  free(buf);
  return ok;
}

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;
  M5.begin(cfg);
  Serial.begin(115200);

  loadSettings();
  M5.Display.setRotation(3); // 1280x720 横向き
  uint8_t targetBrt = (uint8_t)((brightnessPct * 255) / 100);
  // 起動スプラッシュが上から走査描画されるのを見せないよう、最初はバックライトを消灯しておく
  M5.Display.setBrightness(0);
  M5.Display.fillScreen(COL_BLACK);
  Serial.printf("[BOOT] Display=%dx%d, PSRAM=%u bytes (free=%u)\n",
                M5.Display.width(), M5.Display.height(),
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());

  // SD_MMC 初期化 ＆ 起動スプラッシュ(消灯状態で描画完了後にふわっとフェードイン) ＆ VLWフォント読込
  uint32_t splashStartMs = 0;
  SD_MMC.setPins(TAB5_SD_CLK, TAB5_SD_CMD, TAB5_SD_D0, TAB5_SD_D1, TAB5_SD_D2, TAB5_SD_D3);
  if (SD_MMC.begin("/sdcard", false)) {
    sdMounted = true;

    // 暗転状態で画像全体を描き終えてから、バックライトを滑らかに上げて全体がふわっと浮かび上がる演出
    if (drawSplashImageFromSd("/38.jpg", false) || drawSplashImageFromSd("/38.png", true)) {
      const int steps = 28;
      for (int s = 1; s <= steps; s++) {
        float t = (float)s / (float)steps;
        float e = t * t * (3.0f - 2.0f * t); // smoothstep イージング
        M5.Display.setBrightness((uint8_t)(targetBrt * e + 0.5f));
        delay(14);
      }
      M5.Display.setBrightness(targetBrt);
      splashStartMs = millis();
    } else {
      M5.Display.setBrightness(targetBrt);
    }

    autoLoadVlwFonts();
    Serial.printf("[SD] Mounted. VLW loaded=%d/%d/%d/%d\n", hasVlwNumBig, hasVlwNumMed, hasVlwNumSm, hasVlwUi);
  } else {
    M5.Display.setBrightness(targetBrt);
    Serial.println("[SD] Mount failed (using built-in fonts).");
  }

  // CAN (TWAI 1Mbps) 初期化
  twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t  t_cfg = TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t  f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g_cfg, &t_cfg, &f_cfg) == ESP_OK && twai_start() == ESP_OK) {
    canOk = true;
    Serial.println("[CAN] TWAI 1Mbps started.");
  }

  // PSRAM スプライト確保 (16bit RGB565)
  sprFrame.setColorDepth(16);
  sprFrame.setPsram(true);
  bool okFrame = (sprFrame.createSprite(1280, 720) != nullptr);

  sprCarbonBg.setColorDepth(16);
  sprCarbonBg.setPsram(true);
  bool okCarbon = (sprCarbonBg.createSprite(1280, 720) != nullptr);

  sprTachoBg.setColorDepth(16);
  sprTachoBg.setPsram(true);
  bool okTacho = (sprTachoBg.createSprite(620, 620) != nullptr);

  sprLedBar.setColorDepth(16);
  sprLedBar.setPsram(true);
  bool okLed = (sprLedBar.createSprite(1280, 42) != nullptr);

  sprWidget.setColorDepth(16);
  sprWidget.setPsram(true);
  bool okWidget = (sprWidget.createSprite(620, 620) != nullptr);

  bool okCache = true;
  for (int i = 0; i < 4; i++) {
    sprPageCache[i].setColorDepth(16);
    sprPageCache[i].setPsram(true);
    if (sprPageCache[i].createSprite(1280, 720) == nullptr) okCache = false;
  }

  Serial.printf("[SPRITE] frame=%d carbon=%d tacho=%d led=%d widget=%d cache4=%d freePsram=%u\n",
                okFrame, okCarbon, okTacho, okLed, okWidget, okCache, (unsigned)ESP.getFreePsram());

  if (!okFrame || !okCarbon || !okTacho || !okLed || !okWidget || !okCache) {
    M5.Display.println("Check Arduino IDE -> Tools -> PSRAM: Enabled");
    while (true) { delay(1000); }
  }

  Serial.println("[INIT] Rebuilding caches...");
  rebuildCarbonCache();
  rebuildTachoCache();
  // 起動時に4ページすべてをPSRAMキャッシュへ事前構築（ページ切替の待ち時間0ms化）
  for (int p = 0; p < 4; p++) {
    Serial.printf("[INIT] Caching P%d...\n", p + 1);
    renderPageToCanvas(sprPageCache[p], p, false);
    pageCacheDirty[p] = false;
  }

  // 起動スプラッシュを最低1.0秒間見せてからダッシュボードを表示
  if (splashStartMs > 0) {
    uint32_t shown = millis() - splashStartMs;
    if (shown < 1000) delay(1000 - shown);
  }

  Serial.println("[INIT] Ready! Entering loop.");
  beepClick(1046, 80);
}

void loop() {
  static uint32_t lastMs = millis();
  static uint32_t hbMs = 0;
  static uint32_t frameCnt = 0;
  uint32_t now = millis();
  float dt = clampf((now - lastMs) / 1000.0f, 0.001f, 0.1f);
  lastMs = now;

  pollTouch();
  pollCanBus();
  if (isDemoMode) {
    updateDemoSim(dt);
  }

  // アニメ中は即描画、通常時は最大30fps(33ms間隔)に抑えてタッチ応答を最優先化
  if (isTransitioning || staticDirty || (now - lastRenderMs) >= 33) {
    lastRenderMs = now;
    renderFullFrame();
    frameCnt++;
  } else {
    delay(2);
  }

  if (now - hbMs >= 1000) {
    Serial.printf("[LOOP] fps=%u page=%d rpm=%d demo=%d touch=%d\n",
                  (unsigned)frameCnt, currentPage, telem.rpm, (int)isDemoMode, (int)M5.Touch.getCount());
    frameCnt = 0;
    hbMs = now;
  }
}

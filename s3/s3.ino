#include <LovyanGFX.hpp>
#include <TJpg_Decoder.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>
#include <SPI.h>
#include <SD.h>
#include <ctype.h>
#include <time.h>
#include <vector>
#include <queue>       // 閻劋绨幍锟介梿锟?flood fill

// ========== 鐏炲繐绠峰鏇″壖 ==========
#define PIN_SCK  12
#define PIN_SDA  11
#define PIN_CS   10
#define PIN_DC   9
#define PIN_RST  8

// ========== 閹藉洦娼屽鏇″壖 ==========
#define PIN_VRX  4
#define PIN_VRY  5
#define PIN_SW   6

// ========== 閹藉嫬鍎氭径缈犺閸欙絽绱╅懘?==========
#define CAM_RX   17
#define CAM_TX   16

// ========== TF閸椻€崇穿閼?(SPI3) ==========
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  14
#define SD_CS    13
SPIClass sdSPI(HSPI);

// ========== 鐟欏棝锟芥垶鎸遍弨鎯у棘閺侊拷 ==========
#define VIDEO_FPS 12
#define VIDEO_FRAME_DELAY_MS (1000 / VIDEO_FPS)

// ---------- 鐏炲繐绠锋す鍗炲З ----------
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 panel;
    lgfx::Bus_SPI bus;
public:
    LGFX() {
        auto b = bus.config();
        b.spi_host = SPI2_HOST;
        b.freq_write = 40000000;
        b.pin_sclk = PIN_SCK;
        b.pin_mosi = PIN_SDA;
        b.pin_dc   = PIN_DC;
        bus.config(b);
        panel.setBus(&bus);

        auto p = panel.config();
        p.pin_cs   = PIN_CS;
        p.pin_rst  = PIN_RST;
        p.panel_width  = 240;
        p.panel_height = 320;
        p.memory_width = 240;
        p.memory_height= 320;
        p.rgb_order = false;
        p.invert = false;
        panel.config(p);
        setPanel(&panel);
    }
};

LGFX tft;
LGFX_Sprite sprite(&tft);
HardwareSerial SerialCam(1);

// ========== UI 閻樿埖锟?==========
enum UIState { HOME, MENU, CAMERA, STORAGE,
               TODO_PAGE,
               NUM_GRID,
               MYSTERY_PAGE,
               ENGLISH_CHOOSE,
               ENGLISH_LEARN,
               GAME_FLY,
               GAME_MINESWEEPER,
               GAME_2048,
               GAME_BREAKOUT,
               GAME_SNAKE,
               GAME_DINO };
UIState currentState = HOME;

int selectedIndex = 0;
const int totalItems = 6;
const int cols = 3;
const int rows = 2;

unsigned long lastMoveTime = 0;
const unsigned long moveDelay = 200;
const int joyThreshold = 800;
unsigned long lastBtnTime = 0;
const unsigned long btnDelay = 300;
int lastSWState = HIGH;
bool screenDirty = true;

// ========== 閺傚洣娆㈠ù蹇氾拷鍫㈡祲閸忓啿褰夐柌锟?==========
#define MAX_FILES 50
#define MAX_DEPTH 5
String currentPath = "/";
String fileList[MAX_FILES];
bool isDir[MAX_FILES];
int fileCount = 0;
int fileSelectedIndex = 0;
int listScrollOffset = 0;
bool viewingImage = false;
bool viewingText = false;
bool viewingDayChart = false;
std::vector<String> textViewLines;
String textViewTitle = "";
int textViewTopLine = 0;
const int MAX_TEXT_VIEW_LINES = 260;
const size_t MAX_TEXT_VIEW_BYTES = 24 * 1024;
bool storageWebMode = false;

const int DAY_CHART_MAX_BUCKETS = 16;
String dayChartTitle = "";
String dayChartNames[DAY_CHART_MAX_BUCKETS];
unsigned long dayChartSecs[DAY_CHART_MAX_BUCKETS];
int dayChartCount = 0;
unsigned long dayChartTotalSec = 0;
int dayChartLegendOffset = 0;

enum ListFocus { LIST_FILES, LIST_BOTTOM_BAR };
ListFocus listFocusArea = LIST_FILES;
int listBottomBtnIndex = 0;

enum ImageFocus { IMAGE_AREA, IMAGE_BOTTOM_BAR };
ImageFocus imageFocusArea = IMAGE_AREA;
int imageBottomBtnIndex = 0;

bool showDeleteConfirm = false;
int deleteConfirmSelection = 0;

// ========== 閼挎粌宕熺敮鍐ㄧ湰 ==========
const int boxW = 90;
const int boxH = 70;
const int boxGapX = 12;
const int boxGapY = 20;
const int boxesTotalWidth = (boxW * cols) + (boxGapX * (cols - 1));
const int startX = (320 - boxesTotalWidth) / 2;
const int boxesTotalHeight = (boxH * rows) + boxGapY;
const int startY = (240 - boxesTotalHeight) / 2;

const char* menuTexts[] = {
    u8"\u7f51\u7ad9",
    u8"\u62cd\u6444",
    u8"\u5b58\u50a8",
    u8"\u8bed\u6587",
    u8"\u82f1\u8bed",
    u8"\u5f85\u505a"
};

// ========== 閹藉嫬鍎氭径瀵告祲閸忓啿褰夐柌?==========
#define MAX_SIZE 30000
uint8_t buf[MAX_SIZE];
enum { WAIT_SOF, WAIT_LEN, WAIT_DATA, WAIT_EOF } state = WAIT_SOF;
uint8_t sof = 0, eof = 0;
uint8_t lenBuf[4];
uint32_t len = 0, idx = 0;
bool ready = false;
uint16_t* img_rgb565 = nullptr;
uint16_t jpg_width = 0, jpg_height = 0;
static uint16_t* camera_preview_buffer = nullptr;
static size_t camera_preview_capacity = 0;
static uint16_t camera_preview_width = 0;
static uint16_t camera_preview_height = 0;

#define SWAP_BYTES true
#define FULLSCREEN_CROP false

// ========== 閹峰秶鍙庨崝鐔诲厴 ==========
int photoIndex = 0;
bool captureRequest = false;

// ========== 鐢呭芳缂佺喕锟斤拷 ==========
unsigned long lastFpsPrint = 0;
uint32_t frameCount = 0;
uint32_t lastFrameCount = 0;

// ---------- 鐟欏棝锟芥垶鎸遍弨鎯у弿鐏烇拷閸欐﹢鍣?----------
static uint16_t* video_row_buffer = nullptr;
static int video_row_cursor = 0;
static int video_row_y = -1;
static int video_dst_w = 0, video_dst_h = 0, video_dst_x = 0, video_dst_y = 0;
static int video_src_w = 0, video_src_h = 0;

// ========== 瀵板懎浠?/ 閺佹澘鐡х純鎴炵壐 / 缁佺偟锟芥﹢銆夐棃锟?閻╃鍙ч崣姗€鍣?==========
int numGridSelectedIndex = 0;
int passwordSequence[6];
int passwordIndex = 0;

// ========== 寰呭仛锛堢暘鑼勯挓锛?=========
#define TODO_MAX_TASKS 32
String todoTaskFiles[TODO_MAX_TASKS];
int todoTaskCount = 0;
int todoTaskSelected = 0;
bool todoTimerRunning = false;
String todoActiveTask = "";
unsigned long todoStartMillis = 0;
bool todoNeedReload = true;
int todoCurrentDay = -1;
bool todoSessionChosen = false;
int todoEntrySelected = 0; // 0=寮€濮嬫柊鐨勪竴澶? 1=缁х画浠婂ぉ

// ========== 缁佺偟锟芥﹢銆夐棃銏犲灙鐞涳拷 ==========
int mysterySelectedIndex = 0;
const int mysteryTotalItems = 6;
const char* mysteryGameNames[] = { "Plane", "Minesweeper", "Breakout", "Snake", "2048", "Dino" };

// ========== 閼昏精锟斤拷鐎涳缚绡勯惄绋垮彠閸欐﹢鍣?==========
#define MAX_WORDS 500
struct WordEntry {
  String word;
  String phonetic;
  String meaning;
};
WordEntry englishWords[MAX_WORDS];
int englishWordCount = 0;
int englishLearnMode = 0;   // 0=閼昏鲸鏋冨Ο鈥崇础, 1=娑擄拷閺傚洦膩瀵拷
int englishWordIndex = 0;
int englishPhase = 0;       // 0=娑撳娼? 1=缂堟槒娴嗛棃?

// ============================================================
//  濞撳憡鍨欓惄绋垮彠鐎规矮绠熼敍鍫ワ拷鐐存簚閹垫捇娅掗惌绛癸拷?
// ============================================================
#define MAX_METEORS 10
#define MAX_BULLETS 20

struct Meteor {
    int x, y;
    bool active;
    int speed;
};

struct Bullet {
    int x, y;
    bool active;
};

Meteor meteors[MAX_METEORS];
Bullet bullets[MAX_BULLETS];
int playerX, playerY;
int score;
int gameState;          // 0: playing, 1: game over
unsigned long lastMeteorSpawn;
int meteorSpeedBase;
int spawnInterval;      // ms
unsigned long lastGameFrameTime;
bool gameSwPressed = false;
unsigned long gameSwPressTime = 0;

// ============================================================
//  閺傛澘锟界偞鐖堕幋蹇撳弿鐏烇拷閸欐﹢鍣?
// ============================================================

// ---------- 閹碉拷闂嗭拷 ----------
#define MS_ROWS 16
#define MS_COLS 16
struct MSCell {
    bool mine;
    bool revealed;
    int neighborMines;  // -1 if mine
};
MSCell msGrid[MS_ROWS][MS_COLS];
int msCursorX, msCursorY;
bool msGameOver;
bool msWin;
int msRevealedCount;
int msTotalSafe;
int msCellSize;         // 鐠侊紕鐣诲妤€鍤?
int msOffsetX, msOffsetY;

// ---------- 2048 ----------
#define GRID_SIZE 4
int grid2048[GRID_SIZE][GRID_SIZE];
bool gridMerged[GRID_SIZE][GRID_SIZE];
int score2048;
bool gameOver2048;
bool gameWin2048;
bool moved2048;
unsigned long last2048InputTime;
const int GAME2048_DELAY = 150; // 娣囷拷婢跺稄绱伴崢鐔讹拷?2048_DELAY閿涘牓娼▔鏇熺垼鐠囧棛锟斤讣锟?

// ---------- 閹垫挾鐖鹃崸?----------
#define BRICK_ROWS 6
#define BRICK_COLS 8
struct Brick {
    bool alive;
    int x, y, w, h;
};
Brick bricks[BRICK_ROWS][BRICK_COLS];
int paddleX, paddleY;
int ballX, ballY;
int ballVx, ballVy;
int brickScore;
bool breakoutGameOver;
bool breakoutWin;
int brickW, brickH, brickGap;
int paddleW, paddleH;

// ---------- 鐠愶拷閸氬啳锟?----------
#define SNAKE_MAX_LEN 200
struct Point { int x, y; };
Point snake[SNAKE_MAX_LEN];
int snakeLen;
int snakeDir; // 0娑?1閸?2娑?3瀹?
int snakeNextDir;
bool snakeFood;
int foodX, foodY;
int snakeCellSize;
int snakeGridW, snakeGridH;
bool snakeGameOver;
unsigned long snakeMoveTimer;
int snakeMoveInterval = 200;

// ---------- 鐏忓繑浜规Λ?----------
#define MAX_OBSTACLES 10
struct Obstacle {
    int x, y, w, h;
    bool active;
};
Obstacle obstacles[MAX_OBSTACLES];
int dinoX, dinoY;
int dinoW, dinoH;
int dinoVy;
bool dinoOnGround;
bool dinoCrouching;
int dinoScore;
bool dinoGameOver;
unsigned long dinoSpawnTimer;
int dinoSpawnInterval = 1200;
int groundY;

// ============================================================
//  閸戣姤鏆熸竟鐗堟閿涘牆甯張?+ 閺傛澘锟界儑锟?
// ============================================================
void scanSD(const char* path);
void drawFileList();
void handleStorage();
void displaySelectedFile(const char* filepath, bool leaveBottomSpace);
void drawImageBottomBar();
void drawDeleteConfirm();
void deleteFile(const char* filepath);
void playMJPEGFromFileBrowser(const char* filename);
bool playOneFrame(File &file, uint8_t *frameBuf, size_t &frameLen, bool &stopFlag);
void resetParser();
bool loadTextFileForView(const char* filepath);
void drawTextViewer();
bool isDoDayRecordFile(const String &path, const String &name);
bool loadDayChartForView(const char* filepath);
void drawDayChartViewer();

// ========== 寰呭仛璁℃椂涓庢椂闂存牸寮?==========
// ========== ??????????????? ==========
String todoDayFilePath(int dayId) {
    return String("/do/day_") + String(dayId) + String(".txt");
}

String todoBaseName(const String &pathLike) {
    int slash = pathLike.lastIndexOf('/');
    if (slash >= 0 && slash + 1 < (int)pathLike.length()) return pathLike.substring(slash + 1);
    return pathLike;
}

bool todoReadCurrentDay(int &dayId) {
    dayId = -1;
    if (!SD.exists("/do/_meta.txt")) return false;
    File f = SD.open("/do/_meta.txt", FILE_READ);
    if (!f) return false;
    int latest = -1;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        if (key == "current_day") {
            int v = val.toInt();
            if (v >= 0) latest = v;
        }
    }
    f.close();
    dayId = latest;
    return dayId >= 0;
}

bool todoWriteCurrentDay(int dayId) {
    if (!SD.exists("/do")) SD.mkdir("/do");
    if (SD.exists("/do/_meta.txt")) SD.remove("/do/_meta.txt");
    File f = SD.open("/do/_meta.txt", FILE_WRITE);
    if (!f) return false;
    f.print("current_day=");
    f.println(dayId);
    f.close();
    return true;
}

bool todoStartNewDay() {
    int d = -1;
    if (!todoReadCurrentDay(d)) d = -1;
    d += 1;
    if (!todoWriteCurrentDay(d)) return false;
    String p = todoDayFilePath(d);
    if (!SD.exists(p.c_str())) {
        File f = SD.open(p.c_str(), FILE_WRITE);
        if (f) f.close();
    }
    todoCurrentDay = d;
    todoSessionChosen = true;
    return true;
}

bool todoContinueToday() {
    int d = -1;
    if (!todoReadCurrentDay(d)) {
        d = 0;
        if (!todoWriteCurrentDay(d)) return false;
    }
    String p = todoDayFilePath(d);
    if (!SD.exists(p.c_str())) {
        File f = SD.open(p.c_str(), FILE_WRITE);
        if (f) f.close();
    }
    todoCurrentDay = d;
    todoSessionChosen = true;
    return true;
}

String todoFormatDuration(unsigned long sec) {
    unsigned long h = sec / 3600;
    unsigned long m = (sec % 3600) / 60;
    unsigned long s2 = sec % 60;
    char b[16];
    snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", h, m, s2);
    return String(b);
}

String todoFormatDurationSmart(unsigned long sec) {
    unsigned long h = sec / 3600;
    unsigned long m = (sec % 3600) / 60;
    unsigned long s = sec % 60;
    char b[20];
    if (h > 0) {
        snprintf(b, sizeof(b), "%luh %lum %lus", h, m, s);
    } else if (m > 0) {
        snprintf(b, sizeof(b), "%lum %lus", m, s);
    } else {
        snprintf(b, sizeof(b), "%lus", s);
    }
    return String(b);
}

bool todoLoadTaskFiles() {
    todoTaskCount = 0;
    if (!SD.cardType()) return false;
    if (!SD.exists("/do")) return false;

    File dir = SD.open("/do");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return false;
    }

    File entry = dir.openNextFile();
    while (entry && todoTaskCount < TODO_MAX_TASKS) {
        String name = todoBaseName(String(entry.name()));
        String nameLower = name;
        nameLower.toLowerCase();
        bool isTask = !entry.isDirectory() && nameLower.endsWith(".txt");
        bool isMeta = (name == "_meta.txt");
        bool isDay = nameLower.startsWith("day_");
        if (isTask && !isMeta && !isDay) {
            todoTaskFiles[todoTaskCount++] = name;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    for (int i = 0; i < todoTaskCount - 1; i++) {
        for (int j = i + 1; j < todoTaskCount; j++) {
            if (todoTaskFiles[i] > todoTaskFiles[j]) {
                String t = todoTaskFiles[i];
                todoTaskFiles[i] = todoTaskFiles[j];
                todoTaskFiles[j] = t;
            }
        }
    }
    if (todoTaskSelected >= todoTaskCount) todoTaskSelected = (todoTaskCount > 0) ? (todoTaskCount - 1) : 0;
    return true;
}

bool todoStartTimer() {
    if (todoTaskCount <= 0) return false;
    if (todoTaskSelected < 0 || todoTaskSelected >= todoTaskCount) return false;
    todoActiveTask = todoBaseName(todoTaskFiles[todoTaskSelected]);
    todoStartMillis = millis();
    todoTimerRunning = true;
    return true;
}

bool todoStopTimerAndSave() {
    if (!todoTimerRunning) return false;
    if (todoCurrentDay < 0) return false;

    unsigned long durationSec = (unsigned long)((millis() - todoStartMillis) / 1000);
    String path = todoDayFilePath(todoCurrentDay);

    File f = SD.open(path.c_str(), FILE_APPEND);
    if (!f) return false;
    f.print(todoActiveTask);
    f.print(',');
    f.println(durationSec);
    f.close();

    todoTimerRunning = false;
    todoActiveTask = "";
    todoStartMillis = 0;
    return true;
}

unsigned long todoTodayTotalSec() {
    if (todoCurrentDay < 0) return 0;
    String path = todoDayFilePath(todoCurrentDay);
    if (!SD.exists(path.c_str())) return 0;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return 0;
    unsigned long total = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        int c = line.lastIndexOf(',');
        if (c >= 0) {
            String sec = line.substring(c + 1);
            sec.trim();
            total += (unsigned long)sec.toInt();
        }
    }
    f.close();
    return total;
}

String todoTaskDisplayName(String taskName) {
    String n = todoBaseName(taskName);
    String lower = n;
    lower.toLowerCase();
    if (lower.endsWith(".txt") && n.length() > 4) {
        n = n.substring(0, n.length() - 4);
    }
    return n;
}

int todoLoadTodayBuckets(String names[], unsigned long secs[], int maxBuckets, unsigned long &totalSec) {
    totalSec = 0;
    if (maxBuckets <= 0 || todoCurrentDay < 0) return 0;
    String path = todoDayFilePath(todoCurrentDay);
    if (!SD.exists(path.c_str())) return 0;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return 0;

    int count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;

        int c = line.lastIndexOf(',');
        if (c < 0) continue;

        String rawTask = line.substring(0, c);
        String secStr = line.substring(c + 1);
        rawTask.trim();
        secStr.trim();
        unsigned long sec = (unsigned long)secStr.toInt();
        if (sec == 0) continue;

        String task = todoTaskDisplayName(rawTask);
        if (!task.length()) task = u8"\u672a\u547d\u540d";

        int hit = -1;
        for (int i = 0; i < count; i++) {
            if (names[i] == task) {
                hit = i;
                break;
            }
        }

        if (hit >= 0) {
            secs[hit] += sec;
        } else if (count < maxBuckets) {
            names[count] = task;
            secs[count] = sec;
            count++;
        } else {
            names[maxBuckets - 1] = u8"\u5176\u4ed6";
            secs[maxBuckets - 1] += sec;
        }
        totalSec += sec;
    }
    f.close();

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (secs[j] > secs[i]) {
                unsigned long ts = secs[i];
                secs[i] = secs[j];
                secs[j] = ts;
                String tn = names[i];
                names[i] = names[j];
                names[j] = tn;
            }
        }
    }
    return count;
}
void todoDrawSector(int cx, int cy, int r, float degStart, float degEnd, uint16_t color) {
    if (degEnd <= degStart) return;
    float a = degStart;
    const float step = 3.0f;
    while (a < degEnd) {
        float b = a + step;
        if (b > degEnd) b = degEnd;
        float ar = a * DEG_TO_RAD;
        float br = b * DEG_TO_RAD;
        int x1 = cx + (int)(cosf(ar) * r);
        int y1 = cy + (int)(sinf(ar) * r);
        int x2 = cx + (int)(cosf(br) * r);
        int y2 = cy + (int)(sinf(br) * r);
        sprite.fillTriangle(cx, cy, x1, y1, x2, y2, color);
        a = b;
    }
}

void drawTodoPieChart(int cx, int cy, int r) {
    const int MAX_BUCKETS = 8;
    String names[MAX_BUCKETS];
    unsigned long secs[MAX_BUCKETS];
    for (int i = 0; i < MAX_BUCKETS; i++) secs[i] = 0;

    unsigned long totalSec = 0;
    int count = todoLoadTodayBuckets(names, secs, MAX_BUCKETS, totalSec);

    sprite.fillCircle(cx, cy, r, TFT_DARKGREY);
    sprite.drawCircle(cx, cy, r, TFT_DARKGREY);

    sprite.setTextDatum(top_left);
    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    sprite.setCursor(cx - r, cy + r + 6);
    sprite.print(u8"\u4eca\u65e5\u5206\u5e03");

    if (count <= 0 || totalSec == 0) {
        sprite.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        sprite.setCursor(cx - r, cy - 8);
        sprite.print(u8"\u6682\u65e0\u6570\u636e");
        return;
    }

    const uint16_t colors[8] = {
        TFT_ORANGE, TFT_GREEN, TFT_CYAN, TFT_MAGENTA,
        TFT_YELLOW, TFT_BLUE, TFT_RED, TFT_WHITE
    };

    float angle = -90.0f;
    for (int i = 0; i < count; i++) {
        float sweep = 360.0f * ((float)secs[i] / (float)totalSec);
        if (sweep < 1.0f) sweep = 1.0f;
        todoDrawSector(cx, cy, r - 1, angle, angle + sweep, colors[i % 8]);
        angle += sweep;
    }

    int legendY = cy + r + 20;
    int legendX = cx - r;
    int maxLegend = count;
    if (maxLegend > 3) maxLegend = 3;
    for (int i = 0; i < maxLegend; i++) {
        sprite.fillRect(legendX, legendY + i * 14 + 3, 8, 8, colors[i % 8]);
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.setCursor(legendX + 12, legendY + i * 14);
        String row = names[i] + " " + todoFormatDurationSmart(secs[i]);
        sprite.print(row);
    }
}

void drawTodoPage() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setTextDatum(top_left);
    sprite.setCursor(6, 5);
    sprite.print(u8"\u5f85\u505a / \u4e13\u6ce8");

    if (!todoSessionChosen) {
        const char* opts[2] = {u8"\u5f00\u59cb\u65b0\u7684\u4e00\u5929", u8"\u7ee7\u7eed\u4eca\u5929"};
        for (int i = 0; i < 2; i++) {
            int y = 70 + i * 46;
            bool sel = (todoEntrySelected == i);
            uint16_t bg = sel ? TFT_YELLOW : TFT_DARKGREY;
            uint16_t tc = sel ? TFT_BLACK : TFT_WHITE;
            sprite.fillRoundRect(26, y, tft.width() - 52, 34, 8, bg);
            sprite.setTextDatum(middle_center);
            sprite.setTextColor(tc, bg);
            sprite.drawString(opts[i], tft.width() / 2, y + 17);
        }
        sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        sprite.setTextDatum(top_left);
        sprite.setCursor(6, tft.height() - 18);
        sprite.print(u8"\u77ed\u6309\u9009\u62e9  \u957f\u6309\u8fd4\u56de");
        return;
    }

    if (todoTaskCount <= 0) {
        sprite.setTextDatum(middle_center);
        sprite.drawString(u8"\u8bf7\u5728TF\u5361 /do \u4e0b\u653e\u4efb\u52a1txt", tft.width() / 2, tft.height() / 2 - 8);
        return;
    }

    if (!todoTimerRunning) {
        sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        sprite.setCursor(6, 24);
        sprite.print(String(u8"Day ") + String(todoCurrentDay));
        sprite.setTextColor(TFT_CYAN, TFT_BLACK);
        sprite.setCursor(110, 24);
        sprite.print(String(u8"\u4eca\u65e5\u7d2f\u8ba1: ") + todoFormatDuration(todoTodayTotalSec()));

        int top = 46;
        int lineH = 22;
        int maxVisible = 7;
        const int listW = 150;
        int startIdx = todoTaskSelected - 2;
        if (startIdx < 0) startIdx = 0;
        if (startIdx > todoTaskCount - maxVisible) startIdx = todoTaskCount - maxVisible;
        if (startIdx < 0) startIdx = 0;

        for (int i = 0; i < maxVisible; i++) {
            int idx = startIdx + i;
            if (idx >= todoTaskCount) break;
            int y = top + i * lineH;
            bool sel = (idx == todoTaskSelected);
            if (sel) sprite.fillRoundRect(4, y - 1, listW, lineH, 5, TFT_YELLOW);
            sprite.setTextColor(sel ? TFT_BLACK : TFT_LIGHTGREY, sel ? TFT_YELLOW : TFT_BLACK);
            sprite.setCursor(10, y + 2);
            sprite.print(todoTaskDisplayName(todoTaskFiles[idx]));
        }

        int pieCx = tft.width() - 78;
        int pieCy = 116;
        int pieR = 48;
        drawTodoPieChart(pieCx, pieCy, pieR);

        sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        sprite.setCursor(6, tft.height() - 18);
        sprite.print(u8"\u77ed\u6309\u5f00\u59cb  \u957f\u6309\u8fd4\u56de");
    } else {
        unsigned long elapsed = (unsigned long)((millis() - todoStartMillis) / 1000);
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        sprite.drawString(u8"\u6b63\u5728\u4e13\u6ce8", tft.width() / 2, 72);
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.drawString(todoActiveTask, tft.width() / 2, 104);
        sprite.setTextColor(TFT_GREEN, TFT_BLACK);
        sprite.drawString(todoFormatDuration(elapsed), tft.width() / 2, 140);
        sprite.setTextColor(TFT_CYAN, TFT_BLACK);
        sprite.drawString(String(u8"Day ") + String(todoCurrentDay), tft.width() / 2, 166);
        sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
        sprite.drawString(u8"\u77ed\u6309\u7ed3\u675f\u5e76\u8bb0\u5f55\u65f6\u957f  \u957f\u6309\u8fd4\u56de", tft.width() / 2, 196);
    }
}

void handleTodoPage() {
    if (todoNeedReload) {
        todoLoadTaskFiles();
        todoNeedReload = false;
        todoSessionChosen = false;
        todoEntrySelected = 0;
        todoCurrentDay = -1;
        screenDirty = true;
    }

    static unsigned long lastJoyTime = 0;
    const unsigned long joyDelay = 180;
    if (millis() - lastJoyTime > joyDelay) {
        int vry = analogRead(PIN_VRY);
        bool moved = false;
        if (!todoSessionChosen) {
            if (vry < 2048 - joyThreshold) { if (todoEntrySelected > 0) { todoEntrySelected--; moved = true; } }
            else if (vry > 2048 + joyThreshold) { if (todoEntrySelected < 1) { todoEntrySelected++; moved = true; } }
        } else if (!todoTimerRunning && todoTaskCount > 0) {
            if (vry < 2048 - joyThreshold) { if (todoTaskSelected > 0) { todoTaskSelected--; moved = true; } }
            else if (vry > 2048 + joyThreshold) { if (todoTaskSelected < todoTaskCount - 1) { todoTaskSelected++; moved = true; } }
        }
        if (moved) {
            lastJoyTime = millis();
            screenDirty = true;
        }
    }

    static unsigned long pressStart = 0;
    static bool wasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);
    unsigned long now = millis();

    if (curPressed && !wasPressed) {
        pressStart = now;
        wasPressed = true;
    } else if (!curPressed && wasPressed) {
        unsigned long held = now - pressStart;
        if (held >= 1200) {
            if (todoTimerRunning) todoStopTimerAndSave();
            currentState = MENU;
            screenDirty = true;
        } else {
            if (!todoSessionChosen) {
                if (todoEntrySelected == 0) todoStartNewDay();
                else todoContinueToday();
                screenDirty = true;
            } else if (!todoTimerRunning) {
                if (todoStartTimer()) screenDirty = true;
            } else {
                todoStopTimerAndSave();
                screenDirty = true;
            }
        }
        wasPressed = false;
    }

    if (todoTimerRunning) screenDirty = true;
}

void drawNumGrid();
void handleNumGrid();
void drawMysteryPage();
void handleMysteryPage();

bool loadEnglishWords();
void drawEnglishChoose();
void drawEnglishLearn();

void splitTextIntoLines(const String &text, int maxWidth, std::vector<String> &lines);
bool ensureCameraPreviewBuffer(uint16_t w, uint16_t h);
void releaseCameraPreviewBuffer();
bool camera_preview_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);

// 妞嬬偞婧€濞撳憡鍨?
void initGame();
void spawnMeteor();
void shootBullet();
void updateGame();
void renderGame();
void handleGameInput();
void handleGame();

// 閺傛澘锟界偞鐖堕幋蹇擄紣閺勶拷
void initMinesweeper();
void handleMinesweeper();
void updateMinesweeper();
void renderMinesweeper();

void init2048();
void handle2048();
void update2048();
void render2048();

void initBreakout();
void handleBreakout();
void updateBreakout();
void renderBreakout();

void initSnake();
void spawnFood();
bool isOnSnake(int x, int y);
void handleSnake();
void updateSnake();
void renderSnake();

void initDino();
void handleDino();
void updateDino();
void renderDino();

// ---------- 閼惧嘲褰囨稉瀣╃娑擄拷閻撗呭缂傛牕锟?----------
int getNextPhotoIndex() {
    int i = 0;
    while (SD.exists(String("/photo_") + i + ".jpg")) i++;
    return i;
}

// ---------- 娣囨繂鐡ㄩ悡褏澧栭崚鐧滵閸?----------
void savePhotoToSD(uint8_t* data, uint32_t length) {
    if (!SD.cardType()) {
        Serial.println(u8"SD卡未初始化，无法保存照片");
        return;
    }
    char name[32];
    sprintf(name, "/photo_%d.jpg", photoIndex++);
    File f = SD.open(name, FILE_WRITE);
    if (!f) {
        Serial.println(u8"创建照片文件失败");
        return;
    }
    size_t w = f.write(data, length);
    f.close();
    if (w == length) {
        Serial.printf("閻撗呭瀹歌弓绻氱€? %s (%u 鐎涙濡?\n", name, length);
    } else {
        Serial.println("閸愭瑥鍙嗘径杈Е");
    }
}

// ---------- 閹藉嫬鍎氭径鏉戞礀鐠?----------
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!img_rgb565) return true;
    for (uint16_t row = 0; row < h; row++) {
        memcpy(img_rgb565 + (y + row) * jpg_width + x,
               bitmap + row * w,
               w * sizeof(uint16_t));
    }
    return true;
}

bool ensureCameraPreviewBuffer(uint16_t w, uint16_t h) {
    size_t required = (size_t)w * h * sizeof(uint16_t);
    if (required == 0) return false;

    if (camera_preview_buffer && camera_preview_capacity >= required) {
        camera_preview_width = w;
        camera_preview_height = h;
        return true;
    }

    releaseCameraPreviewBuffer();
    camera_preview_buffer = (uint16_t*)heap_caps_malloc(required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!camera_preview_buffer) camera_preview_buffer = (uint16_t*)malloc(required);
    if (!camera_preview_buffer) return false;

    camera_preview_capacity = required;
    camera_preview_width = w;
    camera_preview_height = h;
    return true;
}

void releaseCameraPreviewBuffer() {
    if (camera_preview_buffer) {
        free(camera_preview_buffer);
        camera_preview_buffer = nullptr;
    }
    camera_preview_capacity = 0;
    camera_preview_width = 0;
    camera_preview_height = 0;
}

bool camera_preview_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!camera_preview_buffer) return true;
    for (uint16_t row = 0; row < h; row++) {
        memcpy(camera_preview_buffer + (y + row) * camera_preview_width + x,
               bitmap + row * w,
               w * sizeof(uint16_t));
    }
    return true;
}

// ---------- 閸ユ儳鍎氱紓鈺傛杹閺勫墽銇?----------
void draw_scaled_image(uint16_t* src, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h) {
    if (dst_w <= 0 || dst_h <= 0) return;
    tft.startWrite();
    uint16_t* line_buf = (uint16_t*)malloc(dst_w * sizeof(uint16_t));
    if (!line_buf) return;
    int* y_map = (int*)malloc(dst_h * sizeof(int));
    if (!y_map) {
        free(line_buf);
        return;
    }
    for (int dy = 0; dy < dst_h; dy++) y_map[dy] = (dy * src_h) / dst_h;
    for (int dy = 0; dy < dst_h; dy++) {
        uint16_t* src_row = src + y_map[dy] * src_w;
        for (int dx = 0; dx < dst_w; dx++) {
            int src_x = (dx * src_w) / dst_w;
            line_buf[dx] = src_row[src_x];
        }
        tft.pushImage(dst_x, dst_y + dy, dst_w, 1, line_buf);
    }
    free(y_map);
    free(line_buf);
    tft.endWrite();
}

// ---------- 娑撴彃褰涢弫鐗堝祦鐟欙絾鐎?----------
void process(uint8_t b) {
    switch (state) {
        case WAIT_SOF:
            if (b == 0xAA) sof++;
            else if (sof == 3 && b == 0x01) {
                state = WAIT_LEN;
                sof = 0; idx = 0;
            } else sof = 0;
            break;
        case WAIT_LEN:
            lenBuf[idx++] = b;
            if (idx == 4) {
                len = (lenBuf[0]<<24)|(lenBuf[1]<<16)|(lenBuf[2]<<8)|lenBuf[3];
                if (len == 0 || len > MAX_SIZE) state = WAIT_SOF;
                else { idx = 0; state = WAIT_DATA; }
            }
            break;
        case WAIT_DATA:
            buf[idx++] = b;
            if (idx >= len) { state = WAIT_EOF; eof = 0; }
            break;
        case WAIT_EOF:
            if (b == 0xAA) eof++;
            else if (eof == 3 && b == 0x02) { ready = true; state = WAIT_SOF; }
            else state = WAIT_SOF;
            break;
    }
}

// ---------- 閹藉嫬鍎氭径缈犲瘜婢跺嫮鎮?----------
void handleCamera() {
    while (SerialCam.available()) {
        process(SerialCam.read());
    }

    if (ready) {
        ready = false;
        frameCount++;

        if (captureRequest) {
            captureRequest = false;
            savePhotoToSD(buf, len);
            Serial.printf("閹峰秶鍙? 鐢冦亣鐏?%u 鐎涙濡璡n", len);
        }

        if (TJpgDec.getJpgSize(&jpg_width, &jpg_height, buf, len) == JDR_OK) {
            if (ensureCameraPreviewBuffer(jpg_width, jpg_height)) {
                TJpgDec.setCallback(camera_preview_output);
                TJpgDec.drawJpg(0, 0, buf, len);
                TJpgDec.setCallback(tft_output);

                int screen_w = tft.width();
                int screen_h = tft.height();
                float scale_w = (float)screen_w / jpg_width;
                float scale_h = (float)screen_h / jpg_height;
                float scale = (scale_w < scale_h) ? scale_w : scale_h;
                int dst_w = jpg_width * scale;
                int dst_h = jpg_height * scale;
                int dst_x = (screen_w - dst_w) / 2;
                int dst_y = (screen_h - dst_h) / 2;
                draw_scaled_image(camera_preview_buffer, jpg_width, jpg_height, dst_x, dst_y, dst_w, dst_h);

                tft.setTextColor(TFT_WHITE, TFT_BLACK);
                tft.setCursor(5, tft.height() - 10);
                tft.printf("FPS: %u", frameCount - lastFrameCount);
            }
        }

        unsigned long now = millis();
        if (now - lastFpsPrint >= 1000) {
            uint32_t fps = frameCount - lastFrameCount;
            Serial.printf("閹藉嫬鍎氭径鏉戞姎閻? %u fps\n", fps);
            lastFrameCount = frameCount;
            lastFpsPrint = now;
        }
    }
}

// ========== 妫ｆ牠銆夌紒妯哄煑 ==========
void drawHome() {
    sprite.fillScreen(TFT_BLACK);
    for (int i = 0; i < 8; i++)
        sprite.drawCircle(160, 120, 30 + i * 15, sprite.color565(20 + i * 10, 0, 40 - i * 3));
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_WHITE);
    sprite.setFont(&fonts::FreeSansBold24pt7b);
    sprite.drawString("chxnb", 160, 100);
    sprite.fillCircle(160, 150, 4, TFT_GOLD);
}

// ========== 閼挎粌宕熺紒妯哄煑 ==========
void drawMenu() {
    sprite.fillScreen(TFT_BLACK);
    const uint16_t BOX_COLOR_LIGHT_BLUE = sprite.color565(135, 206, 235);
    const uint16_t SELECT_COLOR_YELLOW  = sprite.color565(255, 220, 0);
    const uint16_t TEXT_COLOR_BLACK     = TFT_BLACK;
    const uint16_t BORDER_WHITE         = TFT_WHITE;
    sprite.setFont(&fonts::efontCN_16);

    for (int i = 0; i < totalItems; i++) {
        int col = i % cols;
        int row = i / cols;
        int x = startX + col * (boxW + boxGapX);
        int y = startY + row * (boxH + boxGapY);

        if (i == selectedIndex) {
            sprite.fillRoundRect(x, y, boxW, boxH, 8, SELECT_COLOR_YELLOW);
            sprite.drawRoundRect(x - 2, y - 2, boxW + 4, boxH + 4, 10, sprite.color565(255,200,0));
            sprite.drawRoundRect(x - 4, y - 4, boxW + 8, boxH + 8, 12, sprite.color565(255,180,0));
            sprite.drawRoundRect(x - 6, y - 6, boxW +12, boxH +12, 14, sprite.color565(255,160,0));
            sprite.drawRoundRect(x - 8, y - 8, boxW +16, boxH +16, 16, sprite.color565(255,140,0));
            sprite.drawRoundRect(x, y, boxW, boxH, 8, BORDER_WHITE);
        } else {
            sprite.fillRoundRect(x, y, boxW, boxH, 8, BOX_COLOR_LIGHT_BLUE);
            sprite.drawRoundRect(x, y, boxW, boxH, 8, BORDER_WHITE);
        }
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TEXT_COLOR_BLACK);
        sprite.drawString(menuTexts[i], x + boxW/2, y + boxH/2);
    }
}

// ========== 閹藉洦娼岄弬鐟版倻鐠囪褰?==========
void readJoystick() {
    if (millis() - lastMoveTime < moveDelay) return;
    int vrx = analogRead(PIN_VRX);
    int vry = analogRead(PIN_VRY);
    int col = selectedIndex % cols;
    int row = selectedIndex / cols;
    bool moved = false;

    if (vrx < 2048 - joyThreshold) { col = (col - 1 + cols) % cols; moved = true; }
    else if (vrx > 2048 + joyThreshold) { col = (col + 1) % cols; moved = true; }
    if (vry < 2048 - joyThreshold) { row = (row - 1 + rows) % rows; moved = true; }
    else if (vry > 2048 + joyThreshold) { row = (row + 1) % rows; moved = true; }

    if (moved) {
        selectedIndex = row * cols + col;
        lastMoveTime = millis();
        screenDirty = true;
    }
}

// ========== 閹稿鎸抽幐澶夌瑓濡拷濞?==========
bool isSWPressed() {
    int reading = digitalRead(PIN_SW);
    if (reading == LOW && lastSWState == HIGH) {
        if (millis() - lastBtnTime > btnDelay) {
            lastBtnTime = millis();
            lastSWState = LOW;
            return true;
        }
    }
    if (reading == HIGH) lastSWState = HIGH;
    return false;
}

// ========== 閻╁憡婧€濡€崇础閹稿鎸虫径鍕倞 ==========
void handleCameraButtons() {
    static unsigned long pressStart = 0;
    static bool wasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);
    unsigned long now = millis();

    if (curPressed && !wasPressed) {
        pressStart = now;
        wasPressed = true;
    }
    else if (curPressed && wasPressed) {
        if (now - pressStart >= 5000) {
            currentState = MENU;
            screenDirty = true;
            while (SerialCam.available()) SerialCam.read();
            state = WAIT_SOF;
            sof = eof = idx = 0;
            ready = false;
            captureRequest = false;
            releaseCameraPreviewBuffer();
            wasPressed = false;
            Serial.println("Long press: exit camera");
        }
    }
    else if (!curPressed && wasPressed) {
        if (now - pressStart < 5000) {
            captureRequest = true;
            Serial.println("閹峰秶鍙庣拠閿嬬湴");
        }
        wasPressed = false;
    }
}

// ========== 閹碉拷閹诲粻D閸楋紕娲拌ぐ?==========
// 鎵弿SD鍗＄洰褰曪紙缃戠珯妯″紡涓嶆樉绀衡€?.鈥濊繑鍥炰笂绾э級
void scanSD(const char* path) {
    fileCount = 0;
    fileSelectedIndex = 0;
    listScrollOffset = 0;

    if (!SD.cardType()) {
        Serial.println("SD not initialized");
        return;
    }

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("閺冪姵纭堕幍鎾崇磻閻╋拷瑜帮拷: %s\n", path);
        if (dir) dir.close();
        return;
    }

    String pathStr = String(path);
    if (pathStr != "/" && !storageWebMode) {
        fileList[fileCount] = "..";
        isDir[fileCount] = true;
        fileCount++;
    }

    File entry = dir.openNextFile();
    while (entry && fileCount < MAX_FILES) {
        String name = entry.name();
        if (name.startsWith(".")) {
            entry.close();
            entry = dir.openNextFile();
            continue;
        }

        if (entry.isDirectory()) {
            fileList[fileCount] = name;
            isDir[fileCount] = true;
            fileCount++;
        } else {
            bool allow = false;
            if (storageWebMode) {
                allow = name.endsWith(".txt") || name.endsWith(".TXT") ||
                        name.endsWith(".json") || name.endsWith(".JSON");
            } else {
                allow = name.endsWith(".jpg") || name.endsWith(".JPG") ||
                        name.endsWith(".mjpg") || name.endsWith(".mjpeg") ||
                        name.endsWith(".txt") || name.endsWith(".TXT") ||
                        name.endsWith(".json") || name.endsWith(".JSON");
            }
            if (allow) {
                fileList[fileCount] = name;
                isDir[fileCount] = false;
                fileCount++;
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    for (int i = 0; i < fileCount - 1; i++) {
        for (int j = i + 1; j < fileCount; j++) {
            bool swap = false;
            if (isDir[i] && !isDir[j]) swap = false;
            else if (!isDir[i] && isDir[j]) swap = true;
            else if (fileList[i] > fileList[j]) swap = true;
            if (swap) {
                String tmpName = fileList[i]; fileList[i] = fileList[j]; fileList[j] = tmpName;
                bool tmpDir = isDir[i]; isDir[i] = isDir[j]; isDir[j] = tmpDir;
            }
        }
    }
    Serial.printf("閹碉拷閹伙拷 %s : %d 娑擄拷閺夛紕娲癨n", path, fileCount);
}

// 鐢喰掗弸鎰珤閻樿埖锟?
static uint8_t parser_state = 0;
static size_t parser_bytesInFrame = 0;
static bool parser_foundFF = false;
static bool parser_prevWasFF = false;

void resetParser() {
    parser_state = 0;
    parser_bytesInFrame = 0;
    parser_foundFF = false;
    parser_prevWasFF = false;
}

bool playOneFrame(File &file, uint8_t *frameBuf, size_t &frameLen, bool &stopFlag) {
    if (stopFlag) return false;

    while (file.available()) {
        uint8_t b = file.read();

        if (parser_state == 0) {
            if (!parser_foundFF && b == 0xFF) {
                parser_foundFF = true;
            } else if (parser_foundFF && b == 0xD8) {
                parser_state = 1;
                parser_bytesInFrame = 2;
                frameBuf[0] = 0xFF;
                frameBuf[1] = 0xD8;
                parser_foundFF = false;
            } else {
                parser_foundFF = false;
            }
        }
        else if (parser_state == 1) {
            frameBuf[parser_bytesInFrame++] = b;

            if (parser_bytesInFrame >= 200 * 1024) {
                Serial.println(u8"视频单帧超过200KB，已丢弃该帧");
                parser_state = 0;
                parser_bytesInFrame = 0;
                parser_prevWasFF = false;
                continue;
            }

            if (parser_prevWasFF && b == 0xD9) {
                frameLen = parser_bytesInFrame;
                parser_state = 0;
                parser_bytesInFrame = 0;
                parser_prevWasFF = false;
                return true;
            }
            parser_prevWasFF = (b == 0xFF);
        }
    }
    return false;
}

bool video_tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!video_row_buffer) return true;
    if (video_dst_w <= 0 || video_dst_h <= 0) return true;

    for (uint16_t row = 0; row < h; row++) {
        int src_y = y + row;
        if (src_y < 0 || src_y >= video_src_h) continue;
        int dst_y = video_dst_y + (src_y * video_dst_h) / video_src_h;
        if (dst_y < 0 || dst_y >= tft.height()) continue;

        if (video_row_y != dst_y && video_row_cursor > 0) {
            tft.pushImage(video_dst_x, video_row_y, video_dst_w, 1, video_row_buffer);
            video_row_cursor = 0;
        }
        video_row_y = dst_y;

        for (int dx = 0; dx < video_dst_w; dx++) {
            int src_x = (dx * video_src_w) / video_dst_w;
            if (src_x >= x && src_x < x + (int)w) {
                video_row_buffer[video_row_cursor + dx] = bitmap[row * w + (src_x - x)];
            }
        }
        video_row_cursor += video_dst_w;
    }
    return true;
}

void playMJPEGFromFileBrowser(const char* filename) {
    File videoFile = SD.open(filename, FILE_READ);
    if (!videoFile) {
        Serial.printf("閺冪姵纭堕幍鎾崇磻鐟欏棝锟芥垶鏋冩禒锟? %s\n", filename);
        return;
    }

    Serial.printf("瀵拷婵鎸遍弨? %s\n", filename);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFont(&fonts::efontCN_16);
    tft.setCursor(5, 5);
    tft.print("閹撅拷閺€锟? ");
    tft.println(filename);
    delay(1500);
    tft.fillRect(0, 0, tft.width(), 25, TFT_BLACK);

    size_t maxFrameSize = 64 * 1024;
    uint8_t* jpegFrame = (uint8_t*)malloc(maxFrameSize);
    if (!jpegFrame) {
        Serial.println(u8"分配JPEG帧缓冲失败");
        videoFile.close();
        return;
    }

    int screen_w = tft.width();
    video_row_buffer = (uint16_t*)malloc(screen_w * sizeof(uint16_t));
    if (!video_row_buffer) {
        Serial.println("閺冪姵纭堕崚鍡涘帳鐞涘瞼绱﹂崘鎻掑隘");
        free(jpegFrame);
        videoFile.close();
        return;
    }

    resetParser();
    TJpgDec.setCallback(video_tft_output);

    bool stopPlaying = false;
    uint16_t jpgW = 0, jpgH = 0;
    int frameCount_local = 0;
    unsigned long lastPrintTime = millis();

    while (!stopPlaying && videoFile.available()) {
        uint32_t frameStart = millis();

        size_t frameLen = 0;
        if (!playOneFrame(videoFile, jpegFrame, frameLen, stopPlaying)) {
            if (stopPlaying) break;
            break;
        }

        if (frameLen > maxFrameSize) {
            free(jpegFrame);
            maxFrameSize = frameLen + 4096;
            jpegFrame = (uint8_t*)malloc(maxFrameSize);
            if (!jpegFrame) break;
            Serial.printf("JPEG缂傛挸鍟块崠鐑樺⒖鐎圭鍤?%u 鐎涙濡璡n", maxFrameSize);
        }

        if (TJpgDec.getJpgSize(&jpgW, &jpgH, jpegFrame, frameLen) != JDR_OK) {
            Serial.println("鐢喰掗弸鎰亼鐠愩儻绱濈捄瀹犵箖");
            continue;
        }

        int screen_h = tft.height();
        float scale_w = (float)screen_w / jpgW;
        float scale_h = (float)screen_h / jpgH;
        float scale = (scale_w < scale_h) ? scale_w : scale_h;
        video_dst_w = jpgW * scale;
        video_dst_h = jpgH * scale;
        video_dst_x = (screen_w - video_dst_w) / 2;
        video_dst_y = (screen_h - video_dst_h) / 2;
        video_src_w = jpgW;
        video_src_h = jpgH;

        video_row_cursor = 0;
        video_row_y = -1;

        if (TJpgDec.drawJpg(0, 0, jpegFrame, frameLen) != JDR_OK) {
            Serial.println("JPEG decode failed");
            continue;
        }

        if (video_row_cursor > 0) {
            tft.pushImage(video_dst_x, video_row_y, video_dst_w, 1, video_row_buffer);
        }

        frameCount_local++;
        unsigned long now = millis();
        if (now - lastPrintTime >= 1000) {
            Serial.printf("鐟欏棝锟芥垵鎶氶悳锟? %d fps\n", frameCount_local);
            frameCount_local = 0;
            lastPrintTime = now;
        }

        uint32_t elapsed = millis() - frameStart;
        if (elapsed < VIDEO_FRAME_DELAY_MS) {
            delay(VIDEO_FRAME_DELAY_MS - elapsed);
        }

        if (digitalRead(PIN_SW) == LOW) {
            stopPlaying = true;
            break;
        }
    }

    TJpgDec.setCallback(tft_output);
    free(video_row_buffer);
    video_row_buffer = nullptr;
    free(jpegFrame);
    videoFile.close();

    tft.fillScreen(TFT_BLACK);
    delay(200);
    Serial.println("鐟欏棝锟芥垶鎸遍弨鍓х波閺夛拷");
}

// ========== 缂佹ê鍩楅弬鍥︽閸掓銆?+ 鎼存洟鍎撮幐澶愭尦 ==========
// 缁樺埗鏂囦欢鍒楄〃锛堢綉绔欐ā寮忎娇鐢ㄢ€滃北鐏€濆ご閮級
void drawFileList() {
    sprite.fillScreen(TFT_BLACK);

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.setCursor(5, 5);
    sprite.print(currentPath);

    const int btnH = 28;
    const int btnY = tft.height() - btnH - 4;
    const int btnW = 70;
    const int btnGap = 25;
    const int leftBtnX = (tft.width() - (2*btnW + btnGap)) / 2;
    const int rightBtnX = leftBtnX + btnW + btnGap;

    if (storageWebMode) {
        const uint16_t BG = sprite.color565(242, 245, 250);
        const uint16_t NAV = sprite.color565(17, 24, 39);
        const uint16_t CARD = TFT_WHITE;
        const uint16_t SEL = sprite.color565(59, 130, 246);
        const uint16_t SUB = sprite.color565(90, 100, 120);

        sprite.fillScreen(BG);
        sprite.fillRect(0, 0, tft.width(), 26, NAV);
        sprite.setFont(&fonts::efontCN_16);
        sprite.setTextColor(TFT_WHITE, NAV);
        sprite.setTextDatum(middle_center);
        sprite.drawString(u8"\u5c71\u706b", tft.width() / 2, 13);
        sprite.setTextDatum(top_left);

        sprite.fillRoundRect(4, 30, tft.width() - 8, 24, 6, TFT_WHITE);
        sprite.drawRoundRect(4, 30, tft.width() - 8, 24, 6, sprite.color565(210, 214, 220));
        sprite.setTextColor(sprite.color565(45, 55, 72), TFT_WHITE);
        String pathLabel = currentPath;
        if (pathLabel.length() > 34) pathLabel = "..." + pathLabel.substring(pathLabel.length() - 31);
        sprite.setCursor(8, 35);
        sprite.print(pathLabel);

        const int listTop = 60;
        const int rowH = 28;
        int maxVisible = (btnY - listTop) / rowH;
        if (maxVisible <= 0) maxVisible = 1;

        if (fileCount == 0) {
            sprite.setTextDatum(middle_center);
            sprite.setTextColor(sprite.color565(90, 100, 120), BG);
            sprite.drawString(u8"\u6682\u65e0\u5185\u5bb9", tft.width()/2, tft.height()/2);
        } else {
            for (int i = 0; i < maxVisible; i++) {
                int idx = listScrollOffset + i;
                if (idx >= fileCount) break;
                int y = listTop + i * rowH;
                bool selected = (listFocusArea == LIST_FILES && idx == fileSelectedIndex);

                uint16_t cardBg = selected ? SEL : CARD;
                uint16_t cardText = selected ? TFT_WHITE : sprite.color565(30, 41, 59);
                sprite.fillRoundRect(6, y, tft.width() - 12, rowH - 4, 6, cardBg);
                if (!selected) {
                    sprite.drawRoundRect(6, y, tft.width() - 12, rowH - 4, 6, sprite.color565(225, 229, 235));
                }
                String marker = isDir[idx] ? "DIR " : "TXT ";
                String displayName = marker + fileList[idx];
                if (displayName.length() > 28) displayName = displayName.substring(0, 25) + "...";
                sprite.setTextColor(cardText, cardBg);
                sprite.setCursor(12, y + 5);
                sprite.print(displayName);
            }
        }

        sprite.setTextColor(SUB, BG);
        sprite.setCursor(6, tft.height() - 18);
        sprite.print(u8"\u7f51\u9875\u6a21\u5f0f");

        const char* btnLabels[2] = {u8"\u4e3b\u9875", u8"\u9000\u51fa"};
        for (int i = 0; i < 2; i++) {
            int bx = (i == 0) ? leftBtnX : rightBtnX;
            bool btnSelected = (listFocusArea == LIST_BOTTOM_BAR && listBottomBtnIndex == i);
            uint16_t btnColor = btnSelected ? TFT_YELLOW : TFT_DARKGREY;
            uint16_t textCol = btnSelected ? TFT_BLACK : TFT_WHITE;
            sprite.fillRoundRect(bx, btnY, btnW, btnH, 6, btnColor);
            if (btnSelected) {
                sprite.drawRoundRect(bx-2, btnY-2, btnW+4, btnH+4, 8, sprite.color565(255,200,0));
                sprite.drawRoundRect(bx, btnY, btnW, btnH, 6, TFT_WHITE);
            }
            sprite.setTextColor(textCol, btnColor);
            sprite.setTextDatum(middle_center);
            sprite.drawString(btnLabels[i], bx + btnW/2, btnY + btnH/2);
        }
        return;
    }

    const int lineHeight = 22;
    const int listTop = 22;
    int maxVisible = (btnY - listTop) / lineHeight;
    if (maxVisible <= 0) maxVisible = 1;

    if (fileCount == 0) {
        sprite.setFont(&fonts::efontCN_16);
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawString(u8"\u65e0\u6587\u4ef6", tft.width()/2, tft.height()/2);
    } else {
        for (int i = 0; i < maxVisible; i++) {
            int idx = listScrollOffset + i;
            if (idx >= fileCount) break;
            int y = listTop + i * lineHeight;
            bool selected = (listFocusArea == LIST_FILES && idx == fileSelectedIndex);

            if (selected) {
                sprite.fillRoundRect(2, y-1, tft.width()-4, lineHeight, 5, TFT_YELLOW);
                sprite.drawRoundRect(1, y-2, tft.width()-2, lineHeight+4, 6, sprite.color565(255,200,0));
                sprite.drawRoundRect(2, y-1, tft.width()-4, lineHeight, 6, TFT_WHITE);
            }

            sprite.setFont(&fonts::efontCN_16);
            String prefix = isDir[idx] ? u8"[\u76ee\u5f55] " : "";
            uint16_t textColor = isDir[idx] ?
                                 (selected ? TFT_BLACK : sprite.color565(100,200,255)) :
                                 (selected ? TFT_BLACK : TFT_LIGHTGREY);
            uint16_t bgColor = selected ? TFT_YELLOW : TFT_BLACK;

            String displayName = prefix + fileList[idx];
            if (displayName.length() > 20) displayName = displayName.substring(0, 17) + "...";
            sprite.setTextColor(textColor, bgColor);
            sprite.setCursor(8, y+2);
            sprite.print(displayName);
        }
    }

    const char* btnLabels[2] = {u8"\u5220\u9664", u8"\u9000\u51fa"};
    for (int i = 0; i < 2; i++) {
        int bx = (i == 0) ? leftBtnX : rightBtnX;
        bool btnSelected = (listFocusArea == LIST_BOTTOM_BAR && listBottomBtnIndex == i);
        uint16_t btnColor = btnSelected ? TFT_YELLOW : TFT_DARKGREY;
        uint16_t textCol = btnSelected ? TFT_BLACK : TFT_WHITE;
        sprite.fillRoundRect(bx, btnY, btnW, btnH, 6, btnColor);
        if (btnSelected) {
            sprite.drawRoundRect(bx-2, btnY-2, btnW+4, btnH+4, 8, sprite.color565(255,200,0));
            sprite.drawRoundRect(bx, btnY, btnW, btnH, 6, TFT_WHITE);
        }
        sprite.setFont(&fonts::efontCN_16);
        sprite.setTextColor(textCol, btnColor);
        sprite.setTextDatum(middle_center);
        sprite.drawString(btnLabels[i], bx + btnW/2, btnY + btnH/2);
    }
}

// ---------- 閺勫墽銇氶崶鍓у ----------
void displaySelectedFile(const char* filepath, bool leaveBottomSpace) {
    File imgFile = SD.open(filepath, FILE_READ);
    if (!imgFile) {
        Serial.printf("閹垫挸绱戞径杈Е: %s\n", filepath);
        return;
    }
    size_t fileSize = imgFile.size();
    uint8_t* jpgBuf = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpgBuf) jpgBuf = (uint8_t*)malloc(fileSize);
    if (!jpgBuf) { imgFile.close(); return; }
    size_t readLen = imgFile.read(jpgBuf, fileSize);
    imgFile.close();
    if (readLen != fileSize) { free(jpgBuf); return; }

    uint16_t jpg_w, jpg_h;
    if (TJpgDec.getJpgSize(&jpg_w, &jpg_h, jpgBuf, fileSize) != JDR_OK) { free(jpgBuf); return; }
    // tft_output() uses global jpg_width/jpg_height when writing decode output.
    // Keep globals in sync here, otherwise direct image viewing after boot can be corrupted.
    jpg_width = jpg_w;
    jpg_height = jpg_h;

    size_t rgbSize = jpg_w * jpg_h * sizeof(uint16_t);
    img_rgb565 = (uint16_t*)heap_caps_malloc(rgbSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img_rgb565) img_rgb565 = (uint16_t*)malloc(rgbSize);
    if (!img_rgb565) { free(jpgBuf); return; }

    TJpgDec.drawJpg(0, 0, jpgBuf, fileSize);

    tft.fillScreen(TFT_BLACK);
    int screen_w = tft.width();
    int screen_h = tft.height();
    int avail_h = leaveBottomSpace ? screen_h - 30 : screen_h;
    float scale_w = (float)screen_w / jpg_w;
    float scale_h = (float)avail_h / jpg_h;
    float scale = (scale_w < scale_h) ? scale_w : scale_h;
    int dst_w = jpg_w * scale;
    int dst_h = jpg_h * scale;
    int dst_x = (screen_w - dst_w) / 2;
    int dst_y = (avail_h - dst_h) / 2;
    draw_scaled_image(img_rgb565, jpg_w, jpg_h, dst_x, dst_y, dst_w, dst_h);

    free(img_rgb565); img_rgb565 = nullptr;
    free(jpgBuf);
}

// ---------- 缂佹ê鍩楅崶鍓у鎼存洟鍎撮幐澶愭尦 ----------
void drawImageBottomBar() {
    const int btnH = 28;
    const int btnY = tft.height() - btnH - 2;
    const int btnW = 70;
    const int btnGap = 25;
    const int leftBtnX = (tft.width() - (2*btnW + btnGap)) / 2;
    const int rightBtnX = leftBtnX + btnW + btnGap;

    tft.fillRect(0, btnY - 2, tft.width(), btnH + 4, TFT_BLACK);

    const char* labels[2];
    if (storageWebMode) {
        labels[0] = "Home";
        labels[1] = "Back";
    } else {
        labels[0] = "Delete";
        labels[1] = "Exit";
    }
    tft.setFont(&fonts::efontCN_16);
    for (int i = 0; i < 2; i++) {
        int bx = (i == 0) ? leftBtnX : rightBtnX;
        bool selected = (imageFocusArea == IMAGE_BOTTOM_BAR && imageBottomBtnIndex == i);
        uint16_t bg = selected ? TFT_YELLOW : TFT_DARKGREY;
        uint16_t tc = selected ? TFT_BLACK : TFT_WHITE;

        tft.fillRoundRect(bx, btnY, btnW, btnH, 6, bg);
        if (selected) {
            tft.drawRoundRect(bx-2, btnY-2, btnW+4, btnH+4, 8, tft.color565(255,200,0));
            tft.drawRoundRect(bx, btnY, btnW, btnH, 6, TFT_WHITE);
        }
        tft.setTextColor(tc, bg);
        tft.setTextDatum(middle_center);
        tft.drawString(labels[i], bx + btnW/2, btnY + btnH/2);
    }
}

// ---------- 閸掔娀娅庣涵锟界拋銈呰剨濡楋拷 ----------
bool loadTextFileForView(const char* filepath) {
    File txtFile = SD.open(filepath, FILE_READ);
    if (!txtFile) {
        Serial.printf("閹垫挸绱戦弬鍥ㄦ拱婢惰精瑙? %s\n", filepath);
        return false;
    }

    textViewLines.clear();
    textViewTitle = String(filepath);
    int slashPos = textViewTitle.lastIndexOf('/');
    if (slashPos >= 0 && slashPos < (int)textViewTitle.length() - 1) {
        textViewTitle = textViewTitle.substring(slashPos + 1);
    }
    textViewTopLine = 0;

    size_t loadedBytes = 0;
    while (txtFile.available() &&
           (int)textViewLines.size() < MAX_TEXT_VIEW_LINES &&
           loadedBytes < MAX_TEXT_VIEW_BYTES) {
        String line = txtFile.readStringUntil('\n');
        loadedBytes += line.length() + 1;
        line.replace("\r", "");
        textViewLines.push_back(line);
    }
    txtFile.close();

    if (textViewLines.empty()) {
        textViewLines.push_back("(empty)");
    }
    if ((int)textViewLines.size() >= MAX_TEXT_VIEW_LINES || loadedBytes >= MAX_TEXT_VIEW_BYTES) {
        textViewLines.push_back("...閸愬懎锟界绶濇径姘剧礉閺嬩線锟界喐膩瀵繋绮庨弰鍓с仛閸撳秴宕愰柈銊ュ瀻...");
    }
    Serial.printf("閺傚洦婀板鎻掑鏉? %s, %d 鐞涘n", filepath, (int)textViewLines.size());
    return true;
}

// 鏂囨湰闃呰鍣細缃戠珯妯″紡鏄剧ず鈥滃北鐏€濋《鏍?
void drawTextViewer() {
    if (storageWebMode) {
        const uint16_t BG = sprite.color565(246, 248, 252);
        const uint16_t NAV = sprite.color565(17, 24, 39);
        sprite.fillScreen(BG);
        sprite.setFont(&fonts::efontCN_16);

        sprite.fillRect(0, 0, tft.width(), 26, NAV);
        sprite.setTextColor(TFT_WHITE, NAV);
        sprite.setTextDatum(middle_center);
        sprite.drawString(u8"\u5c71\u706b", tft.width() / 2, 13);
        sprite.setTextDatum(top_left);

        sprite.fillRoundRect(4, 30, tft.width() - 8, 24, 6, TFT_WHITE);
        sprite.drawRoundRect(4, 30, tft.width() - 8, 24, 6, sprite.color565(210, 214, 220));
        sprite.setTextColor(sprite.color565(45, 55, 72), TFT_WHITE);
        String title2 = textViewTitle;
        if (title2.length() > 28) title2 = title2.substring(0, 25) + "...";
        sprite.setCursor(8, 35);
        sprite.print(title2);

        const int topY2 = 60;
        const int bottomY2 = tft.height() - 20;
        const int lineH2 = 16;
        int maxVisible2 = (bottomY2 - topY2) / lineH2;
        if (maxVisible2 <= 0) maxVisible2 = 1;

        sprite.setTextColor(sprite.color565(30, 41, 59), BG);
        for (int i = 0; i < maxVisible2; i++) {
            int idx = textViewTopLine + i;
            if (idx >= (int)textViewLines.size()) break;
            sprite.setCursor(6, topY2 + i * lineH2);
            sprite.print(textViewLines[idx]);
        }

        sprite.setTextColor(sprite.color565(100, 116, 139), BG);
        sprite.setCursor(6, tft.height() - 16);
        sprite.print(String(textViewTopLine + 1) + "/" + String(textViewLines.size()) + "  " + String(u8"SW杩斿洖"));
        return;
    }

    sprite.fillScreen(TFT_BLACK);
    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);

    sprite.setCursor(4, 2);
    String title = textViewTitle;
    if (title.length() > 20) title = title.substring(0, 17) + "...";
    sprite.print(title);

    const int topY = 22;
    const int bottomY = tft.height() - 22;
    const int lineH = 20;
    int maxVisible = (bottomY - topY) / lineH;
    if (maxVisible <= 0) maxVisible = 1;

    for (int i = 0; i < maxVisible; i++) {
        int idx = textViewTopLine + i;
        if (idx >= (int)textViewLines.size()) break;
        sprite.setCursor(4, topY + i * lineH);
        sprite.print(textViewLines[idx]);
    }

    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.setCursor(4, tft.height() - 18);
    sprite.print(String(textViewTopLine + 1) + "/" + String(textViewLines.size()) + "  " + String(u8"SW杩斿洖"));
}

bool isDoDayRecordFile(const String &path, const String &name) {
    String lp = path;
    String ln = name;
    lp.toLowerCase();
    ln.toLowerCase();
    if (!(lp == "/do" || lp == "/do/")) return false;
    return ln.startsWith("day_") && ln.endsWith(".txt");
}

bool loadDayChartForView(const char* filepath) {
    File f = SD.open(filepath, FILE_READ);
    if (!f) return false;

    for (int i = 0; i < DAY_CHART_MAX_BUCKETS; i++) {
        dayChartNames[i] = "";
        dayChartSecs[i] = 0;
    }
    dayChartCount = 0;
    dayChartTotalSec = 0;
    dayChartLegendOffset = 0;

    String title = String(filepath);
    int slashPos = title.lastIndexOf('/');
    if (slashPos >= 0 && slashPos < (int)title.length() - 1) {
        title = title.substring(slashPos + 1);
    }
    dayChartTitle = title;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;

        int c = line.lastIndexOf(',');
        if (c < 0) continue;
        String rawTask = line.substring(0, c);
        String secStr = line.substring(c + 1);
        rawTask.trim();
        secStr.trim();
        unsigned long sec = (unsigned long)secStr.toInt();
        if (sec == 0) continue;

        String task = todoTaskDisplayName(rawTask);
        if (!task.length()) task = u8"\u672a\u547d\u540d";

        int hit = -1;
        for (int i = 0; i < dayChartCount; i++) {
            if (dayChartNames[i] == task) { hit = i; break; }
        }

        if (hit >= 0) {
            dayChartSecs[hit] += sec;
        } else if (dayChartCount < DAY_CHART_MAX_BUCKETS) {
            dayChartNames[dayChartCount] = task;
            dayChartSecs[dayChartCount] = sec;
            dayChartCount++;
        } else {
            int oi = DAY_CHART_MAX_BUCKETS - 1;
            if (dayChartNames[oi].length() == 0) dayChartNames[oi] = u8"\u5176\u4ed6";
            dayChartSecs[oi] += sec;
        }
        dayChartTotalSec += sec;
    }
    f.close();

    for (int i = 0; i < dayChartCount - 1; i++) {
        for (int j = i + 1; j < dayChartCount; j++) {
            if (dayChartSecs[j] > dayChartSecs[i]) {
                unsigned long ts = dayChartSecs[i];
                dayChartSecs[i] = dayChartSecs[j];
                dayChartSecs[j] = ts;
                String tn = dayChartNames[i];
                dayChartNames[i] = dayChartNames[j];
                dayChartNames[j] = tn;
            }
        }
    }
    return true;
}

void drawDayChartViewer() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextDatum(top_left);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(6, 4);
    sprite.print(u8"\u5386\u53f2\u5206\u5e03  ");
    sprite.print(dayChartTitle);

    int cx = 80;
    int cy = 120;
    int r = 56;
    sprite.fillCircle(cx, cy, r, TFT_DARKGREY);
    sprite.drawCircle(cx, cy, r, TFT_DARKGREY);

    if (dayChartTotalSec > 0 && dayChartCount > 0) {
        const uint16_t colors[8] = {
            TFT_ORANGE, TFT_GREEN, TFT_CYAN, TFT_MAGENTA,
            TFT_YELLOW, TFT_BLUE, TFT_RED, TFT_WHITE
        };
        float angle = -90.0f;
        for (int i = 0; i < dayChartCount; i++) {
            float sweep = 360.0f * ((float)dayChartSecs[i] / (float)dayChartTotalSec);
            if (sweep < 1.0f) sweep = 1.0f;
            todoDrawSector(cx, cy, r - 1, angle, angle + sweep, colors[i % 8]);
            angle += sweep;
        }
    } else {
        sprite.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
        sprite.setTextDatum(middle_center);
        sprite.drawString(u8"\u65e0\u6570\u636e", cx, cy);
        sprite.setTextDatum(top_left);
    }

    int lx = 148;
    int ly = 30;
    int lh = 18;
    int maxRows = 10;
    if (maxRows > dayChartCount) maxRows = dayChartCount;
    if (dayChartLegendOffset < 0) dayChartLegendOffset = 0;
    if (dayChartLegendOffset > dayChartCount - maxRows) dayChartLegendOffset = dayChartCount - maxRows;
    if (dayChartLegendOffset < 0) dayChartLegendOffset = 0;

    const uint16_t colors2[8] = {
        TFT_ORANGE, TFT_GREEN, TFT_CYAN, TFT_MAGENTA,
        TFT_YELLOW, TFT_BLUE, TFT_RED, TFT_WHITE
    };
    for (int i = 0; i < maxRows; i++) {
        int idx = dayChartLegendOffset + i;
        int y = ly + i * lh;
        sprite.fillRect(lx, y + 3, 9, 9, colors2[idx % 8]);
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.setCursor(lx + 12, y);
        String name = dayChartNames[idx];
        if (name.length() > 7) name = name.substring(0, 7) + "...";
        sprite.print(name + " " + todoFormatDurationSmart(dayChartSecs[idx]));
    }

    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    sprite.setCursor(6, 186);
    sprite.print(String(u8"\u603b\u65f6\u957f: ") + todoFormatDuration(dayChartTotalSec));
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.setCursor(6, 206);
    sprite.print(u8"\u6447\u6746\u4e0a\u4e0b\u6eda\u52a8  SW\u8fd4\u56de");
}

void drawDeleteConfirm() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setFont(&fonts::efontCN_16);

    sprite.fillRoundRect(20, 70, tft.width() - 40, 100, 12, sprite.color565(30, 30, 30));
    sprite.drawRoundRect(20, 70, tft.width() - 40, 100, 12, TFT_YELLOW);

    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_YELLOW, sprite.color565(30, 30, 30));
    sprite.drawString("Delete this file?", tft.width() / 2, 95);
    sprite.setTextColor(TFT_WHITE, sprite.color565(30, 30, 30));
    sprite.drawString(fileList[fileSelectedIndex], tft.width() / 2, 120);

    const int btnW = 60, btnH = 24, btnY = 145;
    const int leftBtnX = tft.width()/2 - btnW - 10;
    const int rightBtnX = tft.width()/2 + 10;

    const char* options[2] = {u8"\u786e\u8ba4", u8"\u53d6\u6d88"};
    for (int i = 0; i < 2; i++) {
        int bx = (i == 0) ? leftBtnX : rightBtnX;
        bool sel = (deleteConfirmSelection == i);
        uint16_t bg = sel ? TFT_YELLOW : TFT_DARKGREY;
        uint16_t tc = sel ? TFT_BLACK : TFT_WHITE;
        sprite.fillRoundRect(bx, btnY, btnW, btnH, 6, bg);
        if (sel) sprite.drawRoundRect(bx, btnY, btnW, btnH, 6, TFT_WHITE);
        sprite.setTextColor(tc, bg);
        sprite.drawString(options[i], bx + btnW/2, btnY + btnH/2);
    }
}

// ---------- 閸掔娀娅庨弬鍥︽ ----------
void deleteFile(const char* filepath) {
    if (SD.exists(filepath)) {
        if (SD.remove(filepath)) {
            Serial.printf("瀹告彃鍨归梽? %s\n", filepath);
        } else {
            Serial.printf("閸掔娀娅庢径杈Е: %s\n", filepath);
        }
    } else {
        Serial.printf("娑撳秴鐡ㄩ崷? %s\n", filepath);
    }
}

// ========== 鐎涙ê鍋嶅Ο鈥崇础娴滃娆㈡径鍕倞 ==========
void handleStorage() {
    static unsigned long lastJoyTime = 0;
    const unsigned long joyDelay = 200;
    int vrx = analogRead(PIN_VRX);
    int vry = analogRead(PIN_VRY);

    if (showDeleteConfirm) {
        if (millis() - lastJoyTime > joyDelay) {
            if (vrx < 2048 - joyThreshold) {
                deleteConfirmSelection = (deleteConfirmSelection - 1 + 2) % 2;
                lastJoyTime = millis();
                screenDirty = true;
            } else if (vrx > 2048 + joyThreshold) {
                deleteConfirmSelection = (deleteConfirmSelection + 1) % 2;
                lastJoyTime = millis();
                screenDirty = true;
            }
        }
        static bool confirmWasPressed = false;
        bool curPressed = (digitalRead(PIN_SW) == LOW);
        if (curPressed && !confirmWasPressed) {
            confirmWasPressed = true;
        } else if (!curPressed && confirmWasPressed) {
            if (deleteConfirmSelection == 0) {
                String fullPath = currentPath;
                if (!fullPath.endsWith("/")) fullPath += "/";
                fullPath += fileList[fileSelectedIndex];
                deleteFile(fullPath.c_str());
                scanSD(currentPath.c_str());
                if (fileSelectedIndex >= fileCount) fileSelectedIndex = fileCount - 1;
                viewingImage = false;
                viewingText = false;
                viewingDayChart = false;
                listFocusArea = LIST_FILES;
                imageFocusArea = IMAGE_AREA;
            }
            showDeleteConfirm = false;
            deleteConfirmSelection = 0;
            screenDirty = true;
            confirmWasPressed = false;
        }
        return;
    }

    if (viewingText) {
        const int lineH = 20;
        int maxVisible = (tft.height() - 44) / lineH;
        if (maxVisible <= 0) maxVisible = 1;
        int maxTop = (int)textViewLines.size() - maxVisible;
        if (maxTop < 0) maxTop = 0;

        if (millis() - lastJoyTime > joyDelay) {
            bool moved = false;
            if (vry < 2048 - joyThreshold) {
                if (textViewTopLine > 0) {
                    textViewTopLine--;
                    moved = true;
                }
            } else if (vry > 2048 + joyThreshold) {
                if (textViewTopLine < maxTop) {
                    textViewTopLine++;
                    moved = true;
                }
            }
            if (moved) {
                lastJoyTime = millis();
                screenDirty = true;
            }
        }

        static bool txtWasPressed = false;
        bool txtPressed = (digitalRead(PIN_SW) == LOW);
        if (txtPressed && !txtWasPressed) {
            txtWasPressed = true;
        } else if (!txtPressed && txtWasPressed) {
            viewingText = false;
            screenDirty = true;
            txtWasPressed = false;
        }
        return;
    }

    if (viewingDayChart) {
        if (millis() - lastJoyTime > joyDelay) {
            bool moved = false;
            int maxRows = 10;
            if (maxRows > dayChartCount) maxRows = dayChartCount;
            int maxOffset = dayChartCount - maxRows;
            if (maxOffset < 0) maxOffset = 0;
            if (vry < 2048 - joyThreshold) {
                if (dayChartLegendOffset > 0) {
                    dayChartLegendOffset--;
                    moved = true;
                }
            } else if (vry > 2048 + joyThreshold) {
                if (dayChartLegendOffset < maxOffset) {
                    dayChartLegendOffset++;
                    moved = true;
                }
            }
            if (moved) {
                lastJoyTime = millis();
                screenDirty = true;
            }
        }

        static bool chartWasPressed = false;
        bool chartPressed = (digitalRead(PIN_SW) == LOW);
        if (chartPressed && !chartWasPressed) {
            chartWasPressed = true;
        } else if (!chartPressed && chartWasPressed) {
            viewingDayChart = false;
            screenDirty = true;
            chartWasPressed = false;
        }
        return;
    }

    if (!viewingImage) {
        if (millis() - lastJoyTime > joyDelay) {
            bool moved = false;
            if (vry < 2048 - joyThreshold) {
                if (listFocusArea == LIST_FILES) {
                    if (fileCount > 0) {
                        fileSelectedIndex = (fileSelectedIndex - 1 + fileCount) % fileCount;
                        moved = true;
                    }
                } else {
                    listFocusArea = LIST_FILES;
                    moved = true;
                }
            } else if (vry > 2048 + joyThreshold) {
                if (listFocusArea == LIST_FILES) {
                    if (fileCount > 0) {
                        if (fileSelectedIndex < fileCount - 1) {
                            fileSelectedIndex++;
                        } else {
                            listFocusArea = LIST_BOTTOM_BAR;
                            listBottomBtnIndex = 0;
                        }
                        moved = true;
                    }
                }
            }

            if (listFocusArea == LIST_BOTTOM_BAR) {
                if (vrx < 2048 - joyThreshold) {
                    if (listBottomBtnIndex > 0) { listBottomBtnIndex--; moved = true; }
                } else if (vrx > 2048 + joyThreshold) {
                    if (listBottomBtnIndex < 1) { listBottomBtnIndex++; moved = true; }
                }
            }

            if (moved) {
                lastJoyTime = millis();
                if (listFocusArea == LIST_FILES) {
                    int maxVisible = (tft.height() - 22 - 32) / 22;
                    if (maxVisible <= 0) maxVisible = 1;
                    int centerOffset = maxVisible / 2;
                    listScrollOffset = fileSelectedIndex - centerOffset;
                    if (listScrollOffset < 0) listScrollOffset = 0;
                    if (listScrollOffset > fileCount - maxVisible) listScrollOffset = fileCount - maxVisible;
                    if (listScrollOffset < 0) listScrollOffset = 0;
                }
                screenDirty = true;
            }
        }

        static bool listWasPressed = false;
        bool curPressed = (digitalRead(PIN_SW) == LOW);
        if (curPressed && !listWasPressed) {
            listWasPressed = true;
        } else if (!curPressed && listWasPressed) {
            if (fileCount > 0 || listFocusArea == LIST_BOTTOM_BAR) {
                if (listFocusArea == LIST_FILES) {
                    if (isDir[fileSelectedIndex]) {
                        String name = fileList[fileSelectedIndex];
                        if (name == "..") {
                            int lastSlash = currentPath.lastIndexOf('/');
                            if (lastSlash == 0) currentPath = "/";
                            else currentPath = currentPath.substring(0, lastSlash);
                        } else {
                            if (!currentPath.endsWith("/")) currentPath += "/";
                            currentPath += name;
                        }
                        int depth = 0;
                        for (int i = 0; i < currentPath.length(); i++) if (currentPath[i] == '/') depth++;
                        if (depth > MAX_DEPTH) {
                            int lastSlash = currentPath.lastIndexOf('/');
                            if (lastSlash == 0) currentPath = "/";
                            else currentPath = currentPath.substring(0, lastSlash);
                            Serial.println(u8"目录层级超出限制");
                        }
                        scanSD(currentPath.c_str());
                        fileSelectedIndex = 0;
                        listScrollOffset = 0;
                        listFocusArea = LIST_FILES;
                        screenDirty = true;
                    } else {
                        String fullPath = currentPath;
                        if (!fullPath.endsWith("/")) fullPath += "/";
                        fullPath += fileList[fileSelectedIndex];
                        String lowerName = fileList[fileSelectedIndex];
                        lowerName.toLowerCase();

                        if (lowerName.endsWith(".mjpg") || lowerName.endsWith(".mjpeg")) {
                            playMJPEGFromFileBrowser(fullPath.c_str());
                            screenDirty = true;
                            drawFileList();
                            sprite.pushSprite(0, 0);
                        } else if (lowerName.endsWith(".jpg") || lowerName.endsWith(".jpeg")) {
                            viewingImage = true;
                            imageFocusArea = IMAGE_AREA;
                            imageBottomBtnIndex = 0;
                            displaySelectedFile(fullPath.c_str(), true);
                            drawImageBottomBar();
                        } else if (lowerName.endsWith(".txt") || lowerName.endsWith(".json")) {
                            tft.fillScreen(TFT_BLACK);
                            tft.setFont(&fonts::efontCN_16);
                            tft.setTextColor(TFT_WHITE, TFT_BLACK);
                            tft.setTextDatum(middle_center);
                            tft.drawString("閸旂姾娴囨稉?..", tft.width()/2, tft.height()/2);
                            if (isDoDayRecordFile(currentPath, fileList[fileSelectedIndex])) {
                                if (loadDayChartForView(fullPath.c_str())) {
                                    viewingDayChart = true;
                                    screenDirty = true;
                                }
                            } else {
                                if (loadTextFileForView(fullPath.c_str())) {
                                    viewingText = true;
                                    screenDirty = true;
                                }
                            }
                        } else {
                            Serial.println(u8"不支持的文件类型");
                        }
                    }
                } else {
                    if (listBottomBtnIndex == 0) {
                        if (storageWebMode) {
                            currentPath = SD.exists("/web") ? "/web" : "/";
                            scanSD(currentPath.c_str());
                            fileSelectedIndex = 0;
                            listScrollOffset = 0;
                            listFocusArea = LIST_FILES;
                            screenDirty = true;
                        } else if (fileCount > 0 && !isDir[fileSelectedIndex] && fileList[fileSelectedIndex] != "..") {
                            showDeleteConfirm = true;
                            deleteConfirmSelection = 0;
                            screenDirty = true;
                        }
                    } else {
                        viewingImage = false;
                        viewingText = false;
                        viewingDayChart = false;
                        showDeleteConfirm = false;
                        currentState = MENU;
                        screenDirty = true;
                        Serial.println("Exit storage");
                    }
                }
            }
            listWasPressed = false;
        }
        return;
    }

    if (viewingImage && !showDeleteConfirm) {
        if (millis() - lastJoyTime > joyDelay) {
            bool moved = false;
            if (imageFocusArea == IMAGE_AREA) {
                if (vrx < 2048 - joyThreshold) {
                    int currentIdx = -1;
                    for (int i = 0; i < fileCount; i++) {
                        if (!isDir[i] && fileList[i] == fileList[fileSelectedIndex]) { currentIdx = i; break; }
                    }
                    if (currentIdx != -1) {
                        for (int i = currentIdx - 1; i >= 0; i--) {
                            if (!isDir[i]) { fileSelectedIndex = i; moved = true; break; }
                        }
                    }
                } else if (vrx > 2048 + joyThreshold) {
                    int currentIdx = -1;
                    for (int i = 0; i < fileCount; i++) {
                        if (!isDir[i] && fileList[i] == fileList[fileSelectedIndex]) { currentIdx = i; break; }
                    }
                    if (currentIdx != -1) {
                        for (int i = currentIdx + 1; i < fileCount; i++) {
                            if (!isDir[i]) { fileSelectedIndex = i; moved = true; break; }
                        }
                    }
                }
                if (vry > 2048 + joyThreshold) {
                    imageFocusArea = IMAGE_BOTTOM_BAR;
                    imageBottomBtnIndex = 0;
                    moved = true;
                }
                if (moved) {
                    lastJoyTime = millis();
                    if (vrx != 0) {
                        String fullPath = currentPath;
                        if (!fullPath.endsWith("/")) fullPath += "/";
                        fullPath += fileList[fileSelectedIndex];
                        displaySelectedFile(fullPath.c_str(), true);
                    }
                    drawImageBottomBar();
                }
            } else {
                if (vrx < 2048 - joyThreshold) {
                    if (imageBottomBtnIndex > 0) { imageBottomBtnIndex--; moved = true; }
                } else if (vrx > 2048 + joyThreshold) {
                    if (imageBottomBtnIndex < 1) { imageBottomBtnIndex++; moved = true; }
                }
                if (vry < 2048 - joyThreshold) {
                    imageFocusArea = IMAGE_AREA;
                    moved = true;
                }
                if (moved) {
                    lastJoyTime = millis();
                    drawImageBottomBar();
                }
            }
        }

        static bool imgWasPressed = false;
        bool curPressed = (digitalRead(PIN_SW) == LOW);
        if (curPressed && !imgWasPressed) {
            imgWasPressed = true;
        } else if (!curPressed && imgWasPressed) {
            if (imageFocusArea == IMAGE_AREA) {
                viewingImage = false;
                screenDirty = true;
            } else {
                if (imageBottomBtnIndex == 0) {
                    showDeleteConfirm = true;
                    deleteConfirmSelection = 0;
                    screenDirty = true;
                } else {
                    viewingImage = false;
                    screenDirty = true;
                }
            }
            imgWasPressed = false;
        }
    }
}

// ========== 閸掓繂锟藉锟?==========
void setup() {
    Serial.begin(115200);
    pinMode(PIN_SW, INPUT_PULLUP);
    analogReadResolution(12);

    randomSeed(analogRead(0));

    tft.init();
    tft.setRotation(1);
    tft.setColorDepth(16);
    tft.fillScreen(TFT_BLACK);

    sprite.setColorDepth(16);
    sprite.createSprite(tft.width(), tft.height());

    SerialCam.begin(115200, SERIAL_8N1, CAM_RX, CAM_TX);

    TJpgDec.setCallback(tft_output);
    TJpgDec.setSwapBytes(SWAP_BYTES);

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI)) {
        Serial.println("TF閸椻€冲灥婵瀵叉径杈Е閿涘本濯块悡褍濮涢懗鎴掔瑝閸欙拷閻拷");
    } else {
        Serial.println("TF閸椻€冲灥婵瀵查幋鎰");
        photoIndex = getNextPhotoIndex();
        Serial.printf("娑撳绔存稉锟介悡褏澧栫紓鏍э拷? %d\n", photoIndex);
    }

    drawHome();
    sprite.pushSprite(0, 0);
}

// ========== 瀵板懎浠涙い鐢告桨 ==========
void drawNumGrid() {
    sprite.fillScreen(TFT_BLACK);

    const uint16_t BOX_COLOR_LIGHT_BLUE = sprite.color565(135, 206, 235);
    const uint16_t SELECT_COLOR_YELLOW  = sprite.color565(255, 220, 0);
    const uint16_t TEXT_COLOR_BLACK     = TFT_BLACK;
    const uint16_t BORDER_WHITE         = TFT_WHITE;
    sprite.setFont(&fonts::efontCN_16);

    const int gridCols = 3;
    const int gridRows = 3;
    const int gridBoxW = 72;
    const int gridBoxH = 60;
    const int gridGapX = 20;
    const int gridGapY = 16;

    const int gridTotalWidth = (gridBoxW * gridCols) + (gridGapX * (gridCols - 1));
    const int gridStartX = (tft.width() - gridTotalWidth) / 2;
    const int gridTotalHeight = (gridBoxH * gridRows) + (gridGapY * (gridRows - 1));
    const int gridStartY = (tft.height() - gridTotalHeight) / 2;

    for (int i = 0; i < 9; i++) {
        int col = i % gridCols;
        int row = i / gridCols;
        int x = gridStartX + col * (gridBoxW + gridGapX);
        int y = gridStartY + row * (gridBoxH + gridGapY);

        if (i == numGridSelectedIndex) {
            sprite.fillRoundRect(x, y, gridBoxW, gridBoxH, 8, SELECT_COLOR_YELLOW);
            sprite.drawRoundRect(x - 2, y - 2, gridBoxW + 4, gridBoxH + 4, 10, sprite.color565(255,200,0));
            sprite.drawRoundRect(x - 4, y - 4, gridBoxW + 8, gridBoxH + 8, 12, sprite.color565(255,180,0));
            sprite.drawRoundRect(x - 6, y - 6, gridBoxW +12, gridBoxH +12, 14, sprite.color565(255,160,0));
            sprite.drawRoundRect(x - 8, y - 8, gridBoxW +16, gridBoxH +16, 16, sprite.color565(255,140,0));
            sprite.drawRoundRect(x, y, gridBoxW, gridBoxH, 8, BORDER_WHITE);
        } else {
            sprite.fillRoundRect(x, y, gridBoxW, gridBoxH, 8, BOX_COLOR_LIGHT_BLUE);
            sprite.drawRoundRect(x, y, gridBoxW, gridBoxH, 8, BORDER_WHITE);
        }
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TEXT_COLOR_BLACK);
        sprite.drawString(String(i + 1), x + gridBoxW/2, y + gridBoxH/2);
    }
}

void handleNumGrid() {
    static unsigned long lastJoyTime = 0;
    const unsigned long joyDelay = 200;
    if (millis() - lastJoyTime > joyDelay) {
        int vrx = analogRead(PIN_VRX);
        int vry = analogRead(PIN_VRY);
        const int gridCols = 3;
        const int gridRows = 3;
        int col = numGridSelectedIndex % gridCols;
        int row = numGridSelectedIndex / gridCols;
        bool moved = false;

        if (vrx < 2048 - joyThreshold) { col = (col - 1 + gridCols) % gridCols; moved = true; }
        else if (vrx > 2048 + joyThreshold) { col = (col + 1) % gridCols; moved = true; }
        if (vry < 2048 - joyThreshold) { row = (row - 1 + gridRows) % gridRows; moved = true; }
        else if (vry > 2048 + joyThreshold) { row = (row + 1) % gridRows; moved = true; }

        if (moved) {
            numGridSelectedIndex = row * gridCols + col;
            lastJoyTime = millis();
            drawNumGrid();
            sprite.pushSprite(0, 0);
        }
    }

    static unsigned long pressStart = 0;
    static bool wasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);
    unsigned long now = millis();

    if (curPressed && !wasPressed) {
        pressStart = now;
        wasPressed = true;
    }
    else if (!curPressed && wasPressed) {
        if (now - pressStart >= 3000) {
            currentState = TODO_PAGE;
            screenDirty = true;
            passwordIndex = 0;
        } else {
            int digit = numGridSelectedIndex + 1;
            if (passwordIndex < 6) {
                passwordSequence[passwordIndex] = digit;
                passwordIndex++;
                if (passwordIndex == 6) {
                    const int correct[6] = {1, 1, 4, 5, 1, 4};
                    bool match = true;
                    for (int i = 0; i < 6; i++) {
                        if (passwordSequence[i] != correct[i]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        currentState = MYSTERY_PAGE;
                        mysterySelectedIndex = 0;
                        screenDirty = true;
                    }
                    passwordIndex = 0;
                }
            }
        }
        wasPressed = false;
    }

    if (screenDirty) {
        drawNumGrid();
        sprite.pushSprite(0, 0);
        screenDirty = false;
    }
}

// ========== 缁佺偟锟芥﹢銆夐棃锟介敍鍫熺埗閹村繐鍨悰锟介敍锟?==========
void drawMysteryPage() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setFont(&fonts::efontCN_16);

    const int btnW = 180;
    const int btnH = 36;
    const int gap = 8;
    int totalH = mysteryTotalItems * (btnH + gap) - gap;
    int startY = (tft.height() - totalH) / 2;
    int startX = (tft.width() - btnW) / 2;

    for (int i = 0; i < mysteryTotalItems; i++) {
        int y = startY + i * (btnH + gap);
        bool selected = (i == mysterySelectedIndex);
        uint16_t bg = selected ? TFT_YELLOW : sprite.color565(30, 30, 30);
        uint16_t tc = selected ? TFT_BLACK : TFT_WHITE;

        sprite.fillRoundRect(startX, y, btnW, btnH, 8, bg);
        if (selected) {
            sprite.drawRoundRect(startX - 2, y - 2, btnW + 4, btnH + 4, 10, sprite.color565(255,200,0));
            sprite.drawRoundRect(startX, y, btnW, btnH, 8, TFT_WHITE);
        }
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(tc, bg);
        sprite.drawString(mysteryGameNames[i], tft.width() / 2, y + btnH / 2);
    }
}

void handleMysteryPage() {
    static unsigned long lastJoyTime = 0;
    const unsigned long joyDelay = 200;

    if (millis() - lastJoyTime > joyDelay) {
        int vrx = analogRead(PIN_VRX);
        int vry = analogRead(PIN_VRY);
        bool moved = false;

        if (vry < 2048 - joyThreshold) {
            mysterySelectedIndex = (mysterySelectedIndex - 1 + mysteryTotalItems) % mysteryTotalItems;
            moved = true;
        } else if (vry > 2048 + joyThreshold) {
            mysterySelectedIndex = (mysterySelectedIndex + 1) % mysteryTotalItems;
            moved = true;
        }
        if (moved) {
            lastJoyTime = millis();
            screenDirty = true;
        }
    }

    static bool wasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);
    if (curPressed && !wasPressed) {
        wasPressed = true;
    } else if (!curPressed && wasPressed) {
        switch (mysterySelectedIndex) {
            case 0: currentState = GAME_FLY; initGame(); screenDirty = true; break;
            case 1: currentState = GAME_MINESWEEPER; initMinesweeper(); screenDirty = true; break;
            case 2: currentState = GAME_BREAKOUT; initBreakout(); screenDirty = true; break;
            case 3: currentState = GAME_SNAKE; initSnake(); screenDirty = true; break;
            case 4: currentState = GAME_2048; init2048(); screenDirty = true; break;
            case 5: currentState = GAME_DINO; initDino(); screenDirty = true; break;
            default: break;
        }
        wasPressed = false;
    }

    if (screenDirty) {
        drawMysteryPage();
        sprite.pushSprite(0, 0);
        screenDirty = false;
    }
}

// ============================================================
//  妞嬬偞婧€閹垫捇娅掗惌铏埗閹村繐鐤勯悳?
// ============================================================
void initGame() {
    playerX = tft.width() / 2;
    playerY = tft.height() - 30;
    score = 0;
    gameState = 0;

    for (int i = 0; i < MAX_METEORS; i++) meteors[i].active = false;
    for (int i = 0; i < MAX_BULLETS; i++) bullets[i].active = false;

    meteorSpeedBase = 2;
    spawnInterval = 1000;
    lastMeteorSpawn = millis();
    lastGameFrameTime = millis();

    gameSwPressed = false;
    gameSwPressTime = 0;
}

void spawnMeteor() {
    for (int i = 0; i < MAX_METEORS; i++) {
        if (!meteors[i].active) {
            meteors[i].x = random(10, tft.width() - 10);
            meteors[i].y = 0;
            meteors[i].active = true;
            meteors[i].speed = meteorSpeedBase + random(0, 3);
            break;
        }
    }
}

void shootBullet() {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            bullets[i].x = playerX;
            bullets[i].y = playerY - 10;
            bullets[i].active = true;
            break;
        }
    }
}

void updateGame() {
    if (gameState != 0) return;

    for (int i = 0; i < MAX_METEORS; i++) {
        if (meteors[i].active) {
            meteors[i].y += meteors[i].speed;
            if (meteors[i].y > tft.height() + 10) {
                meteors[i].active = false;
            }
        }
    }

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            bullets[i].y -= 6;
            if (bullets[i].y < -10) bullets[i].active = false;
        }
    }

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;
        for (int j = 0; j < MAX_METEORS; j++) {
            if (!meteors[j].active) continue;
            int dx = bullets[i].x - meteors[j].x;
            int dy = bullets[i].y - meteors[j].y;
            if (abs(dx) < 10 && abs(dy) < 10) {
                bullets[i].active = false;
                meteors[j].active = false;
                score++;
                if (score % 5 == 0) {
                    meteorSpeedBase++;
                    if (spawnInterval > 200) spawnInterval -= 50;
                }
                break;
            }
        }
    }

    for (int i = 0; i < MAX_METEORS; i++) {
        if (meteors[i].active) {
            int dx = meteors[i].x - playerX;
            int dy = meteors[i].y - playerY;
            if (abs(dx) < 15 && abs(dy) < 15) {
                gameState = 1;
                break;
            }
        }
    }

    if (millis() - lastMeteorSpawn > spawnInterval) {
        spawnMeteor();
        lastMeteorSpawn = millis();
    }
}

void renderGame() {
    sprite.fillScreen(TFT_BLACK);

    for (int i = 0; i < MAX_METEORS; i++) {
        if (meteors[i].active) {
            sprite.fillCircle(meteors[i].x, meteors[i].y, 6, TFT_RED);
        }
    }

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            sprite.drawFastVLine(bullets[i].x, bullets[i].y, 8, TFT_GREEN);
        }
    }

    sprite.fillTriangle(playerX, playerY - 12,
                        playerX - 12, playerY + 6,
                        playerX + 12, playerY + 6,
                        TFT_CYAN);

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(5, 5);
    sprite.printf("Score: %d", score);

    if (gameState == 1) {
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TFT_RED, TFT_BLACK);
        sprite.setFont(&fonts::FreeSansBold24pt7b);
        sprite.drawString("GAME OVER", tft.width()/2, tft.height()/2 - 30);
        sprite.setFont(&fonts::efontCN_16);
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.drawString("Press SW to restart", tft.width()/2, tft.height()/2 + 20);
    }

    sprite.pushSprite(0, 0);
}

void handleGameInput() {
    int vrx = analogRead(PIN_VRX);
    if (vrx < 2048 - joyThreshold) {
        playerX -= 6;
    } else if (vrx > 2048 + joyThreshold) {
        playerX += 6;
    }
    if (playerX < 12) playerX = 12;
    if (playerX > tft.width() - 12) playerX = tft.width() - 12;

    int sw = digitalRead(PIN_SW);
    if (sw == LOW && !gameSwPressed) {
        gameSwPressed = true;
        gameSwPressTime = millis();
    } else if (sw == HIGH && gameSwPressed) {
        unsigned long duration = millis() - gameSwPressTime;
        if (duration < 200) {
            if (gameState == 0) {
                shootBullet();
            }
        } else if (duration > 2000) {
            currentState = MYSTERY_PAGE;
            screenDirty = true;
        }
        gameSwPressed = false;
    }

    if (gameState == 1 && sw == LOW) {
        static bool overPressed = false;
        if (!overPressed) {
            overPressed = true;
        }
        if (sw == HIGH && overPressed) {
            initGame();
            overPressed = false;
        }
    }
}

void handleGame() {
    unsigned long now = millis();
    if (now - lastGameFrameTime < 20) {
        delay(1);
        return;
    }
    lastGameFrameTime = now;

    updateGame();
    handleGameInput();
    renderGame();
}

// ============================================================
//  閹碉拷闂嗛攱鐖堕幋蹇撶杽閻滐拷
// ============================================================
void initMinesweeper() {
    int screenW = tft.width();
    int screenH = tft.height();
    msCellSize = (screenW < screenH) ? screenW / MS_COLS : screenH / MS_ROWS;
    if (msCellSize < 4) msCellSize = 4;
    int totalW = msCellSize * MS_COLS;
    int totalH = msCellSize * MS_ROWS;
    msOffsetX = (screenW - totalW) / 2;
    msOffsetY = (screenH - totalH) / 2;

    for (int r = 0; r < MS_ROWS; r++) {
        for (int c = 0; c < MS_COLS; c++) {
            msGrid[r][c].mine = false;
            msGrid[r][c].revealed = false;
            msGrid[r][c].neighborMines = 0;
        }
    }

    int minesPlaced = 0;
    while (minesPlaced < 40) {
        int r = random(MS_ROWS);
        int c = random(MS_COLS);
        if (!msGrid[r][c].mine) {
            msGrid[r][c].mine = true;
            minesPlaced++;
        }
    }

    for (int r = 0; r < MS_ROWS; r++) {
        for (int c = 0; c < MS_COLS; c++) {
            if (msGrid[r][c].mine) {
                msGrid[r][c].neighborMines = -1;
                continue;
            }
            int count = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < MS_ROWS && nc >= 0 && nc < MS_COLS && msGrid[nr][nc].mine)
                        count++;
                }
            }
            msGrid[r][c].neighborMines = count;
        }
    }

    msCursorX = 0;
    msCursorY = 0;
    msGameOver = false;
    msWin = false;
    msRevealedCount = 0;
    msTotalSafe = MS_ROWS * MS_COLS - 40;
}

void floodFillReveal(int row, int col) {
    std::queue<std::pair<int,int>> q;
    q.push({row, col});
    while (!q.empty()) {
        auto [r, c] = q.front(); q.pop();
        if (r < 0 || r >= MS_ROWS || c < 0 || c >= MS_COLS) continue;
        if (msGrid[r][c].revealed) continue;
        if (msGrid[r][c].mine) continue;
        msGrid[r][c].revealed = true;
        msRevealedCount++;
        if (msGrid[r][c].neighborMines == 0) {
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < MS_ROWS && nc >= 0 && nc < MS_COLS && !msGrid[nr][nc].revealed && !msGrid[nr][nc].mine) {
                        q.push({nr, nc});
                    }
                }
            }
        }
    }
}

void handleMinesweeper() {
    static unsigned long lastJoyTime = 0;
    const unsigned long joyDelay = 150;
    if (millis() - lastJoyTime > joyDelay && !msGameOver && !msWin) {
        int vrx = analogRead(PIN_VRX);
        int vry = analogRead(PIN_VRY);
        bool moved = false;
        if (vrx < 2048 - joyThreshold) { msCursorX = (msCursorX - 1 + MS_COLS) % MS_COLS; moved = true; }
        else if (vrx > 2048 + joyThreshold) { msCursorX = (msCursorX + 1) % MS_COLS; moved = true; }
        if (vry < 2048 - joyThreshold) { msCursorY = (msCursorY - 1 + MS_ROWS) % MS_ROWS; moved = true; }
        else if (vry > 2048 + joyThreshold) { msCursorY = (msCursorY + 1) % MS_ROWS; moved = true; }
        if (moved) {
            lastJoyTime = millis();
            renderMinesweeper();
            return;
        }
    }

    static bool swWasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);
    if (curPressed && !swWasPressed) {
        swWasPressed = true;
    } else if (!curPressed && swWasPressed) {
        swWasPressed = false;
        if (!msGameOver && !msWin) {
            int r = msCursorY, c = msCursorX;
            if (msGrid[r][c].mine) {
                msGameOver = true;
                for (int i = 0; i < MS_ROWS; i++)
                    for (int j = 0; j < MS_COLS; j++)
                        if (msGrid[i][j].mine) msGrid[i][j].revealed = true;
            } else {
                if (!msGrid[r][c].revealed) {
                    floodFillReveal(r, c);
                    if (msRevealedCount == msTotalSafe) {
                        msWin = true;
                    }
                }
            }
            renderMinesweeper();
        } else {
            currentState = MYSTERY_PAGE;
            screenDirty = true;
        }
    }
}

void renderMinesweeper() {
    sprite.fillScreen(TFT_BLACK);
    int screenW = tft.width(), screenH = tft.height();

    for (int r = 0; r < MS_ROWS; r++) {
        for (int c = 0; c < MS_COLS; c++) {
            int x = msOffsetX + c * msCellSize;
            int y = msOffsetY + r * msCellSize;
            bool revealed = msGrid[r][c].revealed;
            uint16_t color = revealed ? TFT_DARKGREY : TFT_WHITE;
            sprite.fillRect(x, y, msCellSize, msCellSize, color);
            sprite.drawRect(x, y, msCellSize, msCellSize, TFT_BLACK);

            if (revealed) {
                if (msGrid[r][c].mine) {
                    sprite.fillCircle(x + msCellSize/2, y + msCellSize/2, msCellSize/4, TFT_RED);
                } else if (msGrid[r][c].neighborMines > 0) {
                    sprite.setFont(&fonts::efontCN_16);
                    sprite.setTextDatum(middle_center);
                    sprite.setTextColor(TFT_BLACK);
                    sprite.drawString(String(msGrid[r][c].neighborMines), x + msCellSize/2, y + msCellSize/2);
                }
            }
        }
    }

    if (!msGameOver && !msWin) {
        int x = msOffsetX + msCursorX * msCellSize;
        int y = msOffsetY + msCursorY * msCellSize;
        sprite.drawRect(x-2, y-2, msCellSize+4, msCellSize+4, TFT_YELLOW);
    }

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(5, 5);
    if (msWin) {
        sprite.printf("娴ｇ姾鑰芥禍鍡磼");
    } else if (msGameOver) {
        sprite.printf("濞撳憡鍨欑紒鎾存将");
    } else {
        sprite.printf("闂? %d", 40);
    }

    sprite.pushSprite(0, 0);
}

// ============================================================
//  2048 濞撳憡鍨欑€圭偟骞?
// ============================================================
void init2048() {
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            grid2048[r][c] = 0;
    score2048 = 0;
    gameOver2048 = false;
    gameWin2048 = false;
    for (int i = 0; i < 2; i++) {
        int r, c;
        do {
            r = random(GRID_SIZE);
            c = random(GRID_SIZE);
        } while (grid2048[r][c] != 0);
        grid2048[r][c] = (random(10) < 9) ? 2 : 4;
    }
    last2048InputTime = millis();
    moved2048 = false;
}

bool slide2048(int dir) {
    bool moved = false;
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            gridMerged[r][c] = false;

    int dr = 0, dc = 0;
    int startR = 0, startC = 0, stepR = 1, stepC = 1;
    if (dir == 0) { dr = -1; startR = 1; stepR = 1; }
    else if (dir == 2) { dr = 1; startR = GRID_SIZE-2; stepR = -1; }
    else if (dir == 1) { dc = 1; startC = GRID_SIZE-2; stepC = -1; }
    else if (dir == 3) { dc = -1; startC = 1; stepC = 1; }

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int rr = (dir == 0 || dir == 2) ? (dir == 0 ? r : GRID_SIZE-1-r) : (dir == 1 ? c : GRID_SIZE-1-c);
            int cc = (dir == 0 || dir == 2) ? c : r;
            int val = grid2048[rr][cc];
            if (val == 0) continue;
            int nr = rr, nc = cc;
            while (true) {
                int tr = nr + dr, tc = nc + dc;
                if (tr < 0 || tr >= GRID_SIZE || tc < 0 || tc >= GRID_SIZE) break;
                if (grid2048[tr][tc] == 0) {
                    nr = tr; nc = tc;
                    continue;
                } else if (grid2048[tr][tc] == val && !gridMerged[tr][tc]) {
                    grid2048[tr][tc] = val * 2;
                    gridMerged[tr][tc] = true;
                    grid2048[nr][nc] = 0;
                    score2048 += val * 2;
                    moved = true;
                    if (val * 2 == 2048) gameWin2048 = true;
                    break;
                } else {
                    break;
                }
            }
            if (nr != rr || nc != cc) {
                grid2048[nr][nc] = val;
                if (nr != rr || nc != cc) grid2048[rr][cc] = 0;
                moved = true;
            }
        }
    }
    return moved;
}

bool canMove2048() {
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (grid2048[r][c] == 0) return true;
            if (r < GRID_SIZE-1 && grid2048[r][c] == grid2048[r+1][c]) return true;
            if (c < GRID_SIZE-1 && grid2048[r][c] == grid2048[r][c+1]) return true;
        }
    }
    return false;
}

void spawn2048() {
    int r, c;
    do {
        r = random(GRID_SIZE);
        c = random(GRID_SIZE);
    } while (grid2048[r][c] != 0);
    grid2048[r][c] = (random(10) < 9) ? 2 : 4;
}

void handle2048() {
    unsigned long now = millis();
    if (now - last2048InputTime < GAME2048_DELAY) return;

    int vrx = analogRead(PIN_VRX);
    int vry = analogRead(PIN_VRY);
    int dir = -1;
    if (vrx < 2048 - joyThreshold) dir = 3;
    else if (vrx > 2048 + joyThreshold) dir = 1;
    else if (vry < 2048 - joyThreshold) dir = 0;
    else if (vry > 2048 + joyThreshold) dir = 2;
    else return;

    last2048InputTime = now;
    if (gameOver2048 || gameWin2048) {
        if (digitalRead(PIN_SW) == LOW) {
            init2048();
            render2048();
        }
        return;
    }

    if (dir >= 0) {
        if (slide2048(dir)) {
            spawn2048();
            if (!canMove2048()) gameOver2048 = true;
            render2048();
        }
    }
}

void render2048() {
    sprite.fillScreen(TFT_BLACK);
    int cellSize = (tft.width() < tft.height()) ? tft.width() / GRID_SIZE : tft.height() / GRID_SIZE;
    if (cellSize > 70) cellSize = 70;
    int offsetX = (tft.width() - cellSize * GRID_SIZE) / 2;
    int offsetY = (tft.height() - cellSize * GRID_SIZE) / 2;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int x = offsetX + c * cellSize;
            int y = offsetY + r * cellSize;
            int val = grid2048[r][c];
            uint16_t bg = TFT_DARKGREY;
            if (val > 0) {
                uint8_t hue = (val == 2) ? 200 : (val == 4) ? 180 : (val == 8) ? 160 : (val == 16) ? 140 : (val == 32) ? 120 : (val == 64) ? 100 : (val == 128) ? 80 : (val == 256) ? 60 : (val == 512) ? 40 : (val == 1024) ? 20 : 0;
                bg = sprite.color565(255 - hue/2, 200 - hue/2, 100);
            }
            sprite.fillRect(x, y, cellSize, cellSize, bg);
            sprite.drawRect(x, y, cellSize, cellSize, TFT_WHITE);
            if (val > 0) {
                sprite.setFont(&fonts::efontCN_16);
                sprite.setTextDatum(middle_center);
                sprite.setTextColor(TFT_BLACK);
                sprite.drawString(String(val), x + cellSize/2, y + cellSize/2);
            }
        }
    }

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(5, 5);
    sprite.printf("Score: %d", score2048);
    if (gameWin2048) sprite.printf("  WIN!");
    if (gameOver2048) sprite.printf("  GAME OVER");

    sprite.pushSprite(0, 0);
}

// ============================================================
//  閹垫挾鐖鹃崸妤€鐤勯悳?
// ============================================================
void initBreakout() {
    brickScore = 0;
    breakoutGameOver = false;
    breakoutWin = false;

    brickW = 30;
    brickH = 12;
    brickGap = 4;
    int totalBrickW = BRICK_COLS * (brickW + brickGap) - brickGap;
    int startX = (tft.width() - totalBrickW) / 2;
    int startY = 20;
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            bricks[r][c].alive = true;
            bricks[r][c].x = startX + c * (brickW + brickGap);
            bricks[r][c].y = startY + r * (brickH + brickGap);
            bricks[r][c].w = brickW;
            bricks[r][c].h = brickH;
        }
    }

    paddleW = 50;
    paddleH = 8;
    paddleX = (tft.width() - paddleW) / 2;
    paddleY = tft.height() - 20;

    ballX = tft.width() / 2;
    ballY = paddleY - 8;
    ballVx = 3;
    ballVy = -4;
}

void updateBreakout() {
    if (breakoutGameOver || breakoutWin) return;

    ballX += ballVx;
    ballY += ballVy;

    if (ballX < 0 || ballX > tft.width()) ballVx = -ballVx;
    if (ballY < 0) ballVy = -ballVy;
    if (ballY > tft.height()) {
        breakoutGameOver = true;
        return;
    }

    if (ballY + 4 >= paddleY && ballY - 4 <= paddleY + paddleH &&
        ballX >= paddleX && ballX <= paddleX + paddleW) {
        ballVy = -ballVy;
        int hitPos = (ballX - paddleX) / (paddleW / 5);
        ballVx = (hitPos - 2) * 2;
        if (ballVx == 0) ballVx = 2;
    }

    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!bricks[r][c].alive) continue;
            Brick &b = bricks[r][c];
            if (ballX + 4 >= b.x && ballX - 4 <= b.x + b.w &&
                ballY + 4 >= b.y && ballY - 4 <= b.y + b.h) {
                b.alive = false;
                brickScore += 10;
                if (ballX < b.x || ballX > b.x + b.w) ballVx = -ballVx;
                else ballVy = -ballVy;
                bool allGone = true;
                for (int rr = 0; rr < BRICK_ROWS; rr++)
                    for (int cc = 0; cc < BRICK_COLS; cc++)
                        if (bricks[rr][cc].alive) { allGone = false; break; }
                if (allGone) breakoutWin = true;
                return;
            }
        }
    }
}

void handleBreakout() {
    int vrx = analogRead(PIN_VRX);
    if (vrx < 2048 - joyThreshold) paddleX -= 8;
    else if (vrx > 2048 + joyThreshold) paddleX += 8;
    if (paddleX < 0) paddleX = 0;
    if (paddleX > tft.width() - paddleW) paddleX = tft.width() - paddleW;

    static bool swPressed = false;
    bool cur = digitalRead(PIN_SW) == LOW;
    if (cur && !swPressed) {
        swPressed = true;
    } else if (!cur && swPressed) {
        swPressed = false;
        if (breakoutGameOver || breakoutWin) {
            currentState = MYSTERY_PAGE;
            screenDirty = true;
        } else {
            initBreakout();
        }
    }
}

void renderBreakout() {
    sprite.fillScreen(TFT_BLACK);

    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (bricks[r][c].alive) {
                uint16_t color = sprite.color565(255 - r*30, 100, 50 + r*20);
                sprite.fillRect(bricks[r][c].x, bricks[r][c].y, bricks[r][c].w, bricks[r][c].h, color);
                sprite.drawRect(bricks[r][c].x, bricks[r][c].y, bricks[r][c].w, bricks[r][c].h, TFT_WHITE);
            }
        }
    }

    sprite.fillRect(paddleX, paddleY, paddleW, paddleH, TFT_GREEN);
    sprite.fillCircle(ballX, ballY, 4, TFT_YELLOW);

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(5, 5);
    sprite.printf("Score: %d", brickScore);
    if (breakoutWin) {
        sprite.setTextColor(TFT_GREEN);
        sprite.setCursor(5, 25);
        sprite.print("YOU WIN!");
    } else if (breakoutGameOver) {
        sprite.setTextColor(TFT_RED);
        sprite.setCursor(5, 25);
        sprite.print("GAME OVER");
    }

    sprite.pushSprite(0, 0);
}

// ============================================================
//  鐠愶拷閸氬啳娉х€圭偟锟?
// ============================================================
void initSnake() {
    snakeCellSize = 16;
    snakeGridW = tft.width() / snakeCellSize;
    snakeGridH = tft.height() / snakeCellSize;
    if (snakeGridW < 10) snakeGridW = 10;
    if (snakeGridH < 10) snakeGridH = 10;

    snakeLen = 3;
    snake[0] = {snakeGridW/2, snakeGridH/2};
    snake[1] = {snakeGridW/2 - 1, snakeGridH/2};
    snake[2] = {snakeGridW/2 - 2, snakeGridH/2};
    snakeDir = 1;
    snakeNextDir = 1;
    snakeFood = false;
    snakeGameOver = false;
    snakeMoveTimer = millis();
    snakeMoveInterval = 200;

    spawnFood();
}

void spawnFood() {
    do {
        foodX = random(snakeGridW);
        foodY = random(snakeGridH);
    } while (isOnSnake(foodX, foodY));
    snakeFood = true;
}

bool isOnSnake(int x, int y) {
    for (int i = 0; i < snakeLen; i++) {
        if (snake[i].x == x && snake[i].y == y) return true;
    }
    return false;
}

void updateSnake() {
    if (snakeGameOver) return;

    unsigned long now = millis();
    if (now - snakeMoveTimer < snakeMoveInterval) return;
    snakeMoveTimer = now;

    snakeDir = snakeNextDir;

    Point newHead = snake[0];
    switch (snakeDir) {
        case 0: newHead.y--; break;
        case 1: newHead.x++; break;
        case 2: newHead.y++; break;
        case 3: newHead.x--; break;
    }

    bool ate = (newHead.x == foodX && newHead.y == foodY);

    for (int i = snakeLen - 1; i > 0; i--) {
        snake[i] = snake[i-1];
    }
    snake[0] = newHead;

    if (ate) {
        snakeLen++;
        snake[snakeLen-1] = snake[snakeLen-2];
        spawnFood();
    }

    if (newHead.x < 0 || newHead.x >= snakeGridW || newHead.y < 0 || newHead.y >= snakeGridH) {
        snakeGameOver = true;
        return;
    }
    for (int i = 1; i < snakeLen; i++) {
        if (snake[i].x == newHead.x && snake[i].y == newHead.y) {
            snakeGameOver = true;
            return;
        }
    }
}

void handleSnake() {
    int vrx = analogRead(PIN_VRX);
    int vry = analogRead(PIN_VRY);
    if (vrx < 2048 - joyThreshold && snakeDir != 1) snakeNextDir = 3;
    else if (vrx > 2048 + joyThreshold && snakeDir != 3) snakeNextDir = 1;
    else if (vry < 2048 - joyThreshold && snakeDir != 2) snakeNextDir = 0;
    else if (vry > 2048 + joyThreshold && snakeDir != 0) snakeNextDir = 2;

    static bool swPressed = false;
    bool cur = digitalRead(PIN_SW) == LOW;
    if (cur && !swPressed) {
        swPressed = true;
    } else if (!cur && swPressed) {
        swPressed = false;
        if (snakeGameOver) {
            currentState = MYSTERY_PAGE;
            screenDirty = true;
        } else {
            initSnake();
        }
    }
}

void renderSnake() {
    sprite.fillScreen(TFT_BLACK);

    int offsetX = (tft.width() - snakeGridW * snakeCellSize) / 2;
    int offsetY = (tft.height() - snakeGridH * snakeCellSize) / 2;

    if (snakeFood) {
        sprite.fillRect(offsetX + foodX * snakeCellSize, offsetY + foodY * snakeCellSize,
                        snakeCellSize, snakeCellSize, TFT_RED);
    }

    for (int i = 0; i < snakeLen; i++) {
        uint16_t color = (i == 0) ? TFT_YELLOW : TFT_GREEN;
        sprite.fillRect(offsetX + snake[i].x * snakeCellSize,
                        offsetY + snake[i].y * snakeCellSize,
                        snakeCellSize, snakeCellSize, color);
        sprite.drawRect(offsetX + snake[i].x * snakeCellSize,
                        offsetY + snake[i].y * snakeCellSize,
                        snakeCellSize, snakeCellSize, TFT_WHITE);
    }

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(5, 5);
    sprite.printf("Len: %d", snakeLen);
    if (snakeGameOver) {
        sprite.setCursor(5, 25);
        sprite.setTextColor(TFT_RED);
        sprite.print("GAME OVER");
    }

    sprite.pushSprite(0, 0);
}

// ============================================================
//  鐏忓繑浜规Λ娆忕杽閻?
// ============================================================
void initDino() {
    dinoW = 20;
    dinoH = 30;
    dinoX = 30;
    groundY = tft.height() - 40;
    dinoY = groundY - dinoH;
    dinoVy = 0;
    dinoOnGround = true;
    dinoCrouching = false;
    dinoScore = 0;
    dinoGameOver = false;
    dinoSpawnTimer = millis();
    dinoSpawnInterval = 1200;

    for (int i = 0; i < MAX_OBSTACLES; i++) obstacles[i].active = false;
}

void updateDino() {
    if (dinoGameOver) return;

    dinoVy += 1;
    dinoY += dinoVy;
    if (dinoY >= groundY - dinoH) {
        dinoY = groundY - dinoH;
        dinoVy = 0;
        dinoOnGround = true;
    } else {
        dinoOnGround = false;
    }

    if (millis() - dinoSpawnTimer > dinoSpawnInterval) {
        dinoSpawnTimer = millis();
        for (int i = 0; i < MAX_OBSTACLES; i++) {
            if (!obstacles[i].active) {
                obstacles[i].active = true;
                obstacles[i].w = 12 + random(10);
                obstacles[i].h = 20 + random(10);
                obstacles[i].x = tft.width();
                obstacles[i].y = groundY - obstacles[i].h;
                break;
            }
        }
    }

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (obstacles[i].active) {
            obstacles[i].x -= 5;
            if (obstacles[i].x + obstacles[i].w < 0) {
                obstacles[i].active = false;
                dinoScore++;
            }
        }
    }

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (!obstacles[i].active) continue;
        if (dinoX < obstacles[i].x + obstacles[i].w &&
            dinoX + dinoW > obstacles[i].x &&
            dinoY < obstacles[i].y + obstacles[i].h &&
            dinoY + dinoH > obstacles[i].y) {
            dinoGameOver = true;
            return;
        }
    }
}

void handleDino() {
    int vry = analogRead(PIN_VRY);
    bool jump = false, crouch = false;
    if (vry < 2048 - joyThreshold) {
        jump = true;
    }
    if (vry > 2048 + joyThreshold) {
        crouch = true;
    }

    static bool jumpKeyPressed = false;
    static int jumpCount = 0;
    if (jump && !jumpKeyPressed) {
        jumpKeyPressed = true;
        if (dinoOnGround) {
            dinoVy = -14;
            dinoOnGround = false;
            jumpCount = 1;
        } else if (jumpCount < 2) {
            dinoVy = -12;
            jumpCount++;
        }
    } else if (!jump) {
        jumpKeyPressed = false;
    }

    if (crouch && dinoOnGround) {
        dinoH = 18;
        dinoY = groundY - dinoH;
        dinoCrouching = true;
    } else {
        if (!crouch) {
            dinoH = 30;
            dinoY = groundY - dinoH;
            dinoCrouching = false;
        }
    }

    static bool swPressed = false;
    bool cur = digitalRead(PIN_SW) == LOW;
    if (cur && !swPressed) {
        swPressed = true;
    } else if (!cur && swPressed) {
        swPressed = false;
        if (dinoGameOver) {
            currentState = MYSTERY_PAGE;
            screenDirty = true;
        } else {
            initDino();
        }
    }
}

void renderDino() {
    sprite.fillScreen(TFT_BLACK);

    sprite.drawLine(0, groundY, tft.width(), groundY, TFT_WHITE);

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (obstacles[i].active) {
            sprite.fillRect(obstacles[i].x, obstacles[i].y, obstacles[i].w, obstacles[i].h, TFT_GREEN);
        }
    }

    uint16_t color = dinoCrouching ? TFT_ORANGE : TFT_YELLOW;
    sprite.fillRect(dinoX, dinoY, dinoW, dinoH, color);
    sprite.drawRect(dinoX, dinoY, dinoW, dinoH, TFT_WHITE);

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(5, 5);
    sprite.printf("Score: %d", dinoScore);
    if (dinoGameOver) {
        sprite.setCursor(5, 25);
        sprite.setTextColor(TFT_RED);
        sprite.print("GAME OVER");
    }

    sprite.pushSprite(0, 0);
}

// ========== 閼昏精锟斤拷閸旂喕鍏樼€圭偟骞?==========
static bool isChineseChar(uint8_t c) {
    return (c >= 0xE4 && c <= 0xE9);
}

bool loadEnglishWords() {
    englishWordCount = 0;
    if (!SD.cardType()) {
        Serial.println(u8"SD卡未初始化，无法读取英语词库");
        return false;
    }

    const char* filepath = "/english/book.txt";
    if (!SD.exists(filepath)) {
        Serial.printf("閺傚洣娆㈡稉宥呯摠閸? %s\n", filepath);
        return false;
    }

    File file = SD.open(filepath, FILE_READ);
    if (!file) {
        Serial.printf("閹垫挸绱戦弬鍥︽婢惰精瑙? %s\n", filepath);
        return false;
    }

    Serial.println("瀵拷婵锟芥劘锟藉矁袙閺嬫劕宕熺拠锟?..");
    while (file.available() && englishWordCount < MAX_WORDS) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        String word = "";
        String phonetic = "";
        String meaning = "";

        int bracketStart = line.indexOf('[');
        int bracketEnd = line.indexOf(']', bracketStart);

        if (bracketStart != -1 && bracketEnd != -1) {
            word = line.substring(0, bracketStart);
            word.trim();
            phonetic = line.substring(bracketStart + 1, bracketEnd);
            meaning = line.substring(bracketEnd + 1);
            meaning.trim();
        } else {
            word = line;
            for (int i = 0; i < (int)word.length(); i++) {
                if (isChineseChar((uint8_t)word[i])) {
                    meaning = word.substring(i);
                    meaning.trim();
                    word = word.substring(0, i);
                    word.trim();
                    break;
                }
            }
        }

        if (word.length() == 0) continue;

        englishWords[englishWordCount].word = word;
        englishWords[englishWordCount].phonetic = phonetic;
        englishWords[englishWordCount].meaning = meaning;
        englishWordCount++;

        if (englishWordCount % 100 == 0) {
            Serial.printf("瀹歌尪袙閺?%d 娑擄拷閸楁洝锟?..\n", englishWordCount);
        }
    }

    file.close();
    Serial.printf("鐟欙絾鐎界€瑰本鍨氶敍灞藉彙 %d 娑擄拷閸楁洝鐦漒n", englishWordCount);
    return englishWordCount > 0;
}

void splitTextIntoLines(const String &text, int maxWidth, std::vector<String> &lines) {
    if (text.length() == 0) return;
    String currentLine = "";
    int i = 0;
    while (i < text.length()) {
        int charLen = 1;
        unsigned char c = text[i];
        if ((c & 0x80) != 0) {
            if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
        }
        String nextChar = text.substring(i, i + charLen);
        if (sprite.textWidth(currentLine + nextChar) > maxWidth) {
            lines.push_back(currentLine);
            currentLine = nextChar;
        } else {
            currentLine += nextChar;
        }
        i += charLen;
    }
    if (currentLine.length() > 0) {
        lines.push_back(currentLine);
    }
}

void drawEnglishChoose() {
    sprite.fillScreen(TFT_BLACK);
    const uint16_t BOX_COLOR = sprite.color565(135, 206, 235);
    const uint16_t SEL_COLOR = TFT_YELLOW;
    const uint16_t TEXT_COLOR = TFT_BLACK;
    const uint16_t BORDER     = TFT_WHITE;
    sprite.setFont(&fonts::efontCN_16);

    const int btnW = 90, btnH = 60, gap = 30;
    const int totalW = 2 * btnW + gap;
    const int startX = (tft.width() - totalW) / 2;
    const int startY = (tft.height() - btnH) / 2;

    const char* labels[2] = {"涓枃", "鑻辨枃"};

    for (int i = 0; i < 2; i++) {
        int x = startX + i * (btnW + gap);
        bool sel = (i == englishLearnMode);
        if (sel) {
            sprite.fillRoundRect(x, startY, btnW, btnH, 8, SEL_COLOR);
            sprite.drawRoundRect(x-2, startY-2, btnW+4, btnH+4, 10, sprite.color565(255,200,0));
            sprite.drawRoundRect(x, startY, btnW, btnH, 8, BORDER);
        } else {
            sprite.fillRoundRect(x, startY, btnW, btnH, 8, BOX_COLOR);
            sprite.drawRoundRect(x, startY, btnW, btnH, 8, BORDER);
        }
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TEXT_COLOR);
        sprite.drawString(labels[i], x + btnW/2, startY + btnH/2);
    }
}

void drawEnglishLearn() {
    sprite.fillScreen(TFT_BLACK);
    if (englishWordCount == 0) return;

    WordEntry &w = englishWords[englishWordIndex];
    String line1, line2;

    if (englishLearnMode == 0) {
        if (englishPhase == 0) {
            line1 = w.word;
            line2 = "[" + w.phonetic + "]";
        } else {
            line1 = w.meaning;
            line2 = "";
        }
    } else {
        if (englishPhase == 0) {
            line1 = w.meaning;
            line2 = "";
        } else {
            line1 = w.word;
            line2 = "[" + w.phonetic + "]";
        }
    }

    sprite.setFont(&fonts::efontCN_16);
    sprite.setTextDatum(middle_center);

    const int maxTextWidth = tft.width() - 40;
    const int lineHeight = 22;

    std::vector<String> lines1, lines2;
    splitTextIntoLines(line1, maxTextWidth, lines1);
    if (line2.length() > 0) {
        splitTextIntoLines(line2, maxTextWidth, lines2);
    }

    int totalLines = lines1.size() + lines2.size();
    int totalHeight = totalLines * lineHeight;
    int startY = (tft.height() - totalHeight) / 2 + lineHeight / 2;

    sprite.setTextColor(TFT_WHITE);
    for (const auto &l : lines1) {
        sprite.drawString(l, tft.width() / 2, startY);
        startY += lineHeight;
    }

    if (lines2.size() > 0) {
        sprite.setTextColor(TFT_LIGHTGREY);
        for (const auto &l : lines2) {
            sprite.drawString(l, tft.width() / 2, startY);
            startY += lineHeight;
        }
    }

    sprite.setTextColor(TFT_DARKGREY);
    int progressY = startY + 8;
    if (progressY > tft.height() - 10) progressY = tft.height() - 10;
    sprite.drawString(String(englishWordIndex + 1) + "/" + String(englishWordCount),
                      tft.width() / 2, progressY);
}

// ========== 娑撹鎯婇悳?==========
void loop() {
    // 閼昏精锟斤拷闁瀚ㄩ悾宀勬桨
    if (currentState == ENGLISH_CHOOSE) {
        static unsigned long lastJoyTime = 0;
        if (millis() - lastJoyTime > moveDelay) {
            int vrx = analogRead(PIN_VRX);
            if (vrx < 2048 - joyThreshold) {
                englishLearnMode = (englishLearnMode - 1 + 2) % 2;
                lastJoyTime = millis();
                screenDirty = true;
            } else if (vrx > 2048 + joyThreshold) {
                englishLearnMode = (englishLearnMode + 1) % 2;
                lastJoyTime = millis();
                screenDirty = true;
            }
        }

        static bool btnWasPressed = false;
        bool curPressed = (digitalRead(PIN_SW) == LOW);
        if (curPressed && !btnWasPressed) {
            btnWasPressed = true;
        } else if (!curPressed && btnWasPressed) {
            btnWasPressed = false;
            englishWordIndex = 0;
            englishPhase = 0;
            currentState = ENGLISH_LEARN;
            screenDirty = true;
        }

        if (screenDirty) {
            drawEnglishChoose();
            sprite.pushSprite(0, 0);
            screenDirty = false;
        }
        delay(10);
        return;
    }

    // 閼昏精锟斤拷鐎涳缚绡勯悾宀勬桨
    if (currentState == ENGLISH_LEARN) {
        static unsigned long lastJoyTime = 0;
        if (millis() - lastJoyTime > moveDelay) {
            int vrx = analogRead(PIN_VRX);
            if (vrx > 2048 + joyThreshold) {
                if (englishPhase == 0) {
                    englishPhase = 1;
                } else {
                    englishPhase = 0;
                    if (englishWordIndex < englishWordCount - 1) {
                        englishWordIndex++;
                    }
                }
                lastJoyTime = millis();
                screenDirty = true;
            } else if (vrx < 2048 - joyThreshold) {
                if (englishPhase == 1) {
                    englishPhase = 0;
                } else {
                    if (englishWordIndex > 0) {
                        englishWordIndex--;
                        englishPhase = 1;
                    }
                }
                lastJoyTime = millis();
                screenDirty = true;
            }
        }

        static bool btnWasPressed = false;
        bool curPressed = (digitalRead(PIN_SW) == LOW);
        if (curPressed && !btnWasPressed) {
            btnWasPressed = true;
        } else if (!curPressed && btnWasPressed) {
            btnWasPressed = false;
            currentState = ENGLISH_CHOOSE;
            screenDirty = true;
        }

        if (screenDirty) {
            drawEnglishLearn();
            sprite.pushSprite(0, 0);
            screenDirty = false;
        }
        delay(10);
        return;
    }

    // 濞撳憡鍨欓悩鑸碉拷浣革拷鍕拷?
    if (currentState == GAME_FLY) {
        handleGame();
        return;
    }
    if (currentState == GAME_MINESWEEPER) {
        static unsigned long lastRender = 0;
        if (millis() - lastRender > 50) {
            lastRender = millis();
            handleMinesweeper();
            renderMinesweeper();
        }
        return;
    }
    if (currentState == GAME_2048) {
        handle2048();
        render2048();
        delay(10);
        return;
    }
    if (currentState == GAME_BREAKOUT) {
        updateBreakout();
        handleBreakout();
        renderBreakout();
        delay(10);
        return;
    }
    if (currentState == GAME_SNAKE) {
        updateSnake();
        handleSnake();
        renderSnake();
        delay(10);
        return;
    }
    if (currentState == GAME_DINO) {
        updateDino();
        handleDino();
        renderDino();
        delay(10);
        return;
    }

    // --- 閸樼喐婀侀崗鏈电铂閻樿埖锟戒礁锟藉嫮锟?---
    if (currentState == TODO_PAGE) {
        handleTodoPage();
        if (screenDirty) {
            drawTodoPage();
            sprite.pushSprite(0, 0);
            screenDirty = false;
        }
        delay(10);
        return;
    }
    if (currentState == NUM_GRID) {
        handleNumGrid();
        delay(10);
        return;
    }
    if (currentState == MYSTERY_PAGE) {
        handleMysteryPage();
        delay(10);
        return;
    }

    if (currentState != CAMERA && currentState != STORAGE && isSWPressed()) {
        if (currentState == HOME) {
            currentState = MENU;
            selectedIndex = 0;
            screenDirty = true;
        }
        else if (currentState == MENU) {
            if (selectedIndex == 0) {
                currentState = STORAGE;
                storageWebMode = true;
                currentPath = SD.exists("/web") ? "/web" : "/";
                scanSD(currentPath.c_str());
                fileSelectedIndex = 0;
                listScrollOffset = 0;
                viewingImage = false;
                viewingText = false;
                viewingDayChart = false;
                textViewLines.clear();
                listFocusArea = LIST_FILES;
                listBottomBtnIndex = 0;
                imageFocusArea = IMAGE_AREA;
                imageBottomBtnIndex = 0;
                showDeleteConfirm = false;
                screenDirty = true;
                tft.fillScreen(TFT_BLACK);
            }
            else if (selectedIndex == 1) {
                currentState = CAMERA;
                tft.fillScreen(TFT_BLACK);
                state = WAIT_SOF;
                sof = eof = idx = 0;
                ready = false;
                captureRequest = false;
            }
            else if (selectedIndex == 2) {
                currentState = STORAGE;
                storageWebMode = false;
                currentPath = "/";
                scanSD(currentPath.c_str());
                fileSelectedIndex = 0;
                listScrollOffset = 0;
                viewingImage = false;
                viewingText = false;
                viewingDayChart = false;
                textViewLines.clear();
                listFocusArea = LIST_FILES;
                listBottomBtnIndex = 0;
                imageFocusArea = IMAGE_AREA;
                imageBottomBtnIndex = 0;
                showDeleteConfirm = false;
                screenDirty = true;
                tft.fillScreen(TFT_BLACK);
            }
            else if (selectedIndex == 4) {
                if (loadEnglishWords()) {
                    englishLearnMode = 1;
                    currentState = ENGLISH_CHOOSE;
                    screenDirty = true;
                } else {
                    tft.fillScreen(TFT_BLACK);
                    tft.setTextColor(TFT_RED);
                    tft.setFont(&fonts::efontCN_16);
                    tft.setTextDatum(middle_center);
                    tft.drawString(u8"\u5355\u8bcd\u52a0\u8f7d\u5931\u8d25", tft.width()/2, tft.height()/2);
                    delay(1500);
                    currentState = MENU;
                    screenDirty = true;
                }
            }
            else if (selectedIndex == 5) {
                currentState = TODO_PAGE;
                todoNeedReload = true;
                screenDirty = true;
            }
            else {
                currentState = HOME;
                screenDirty = true;
            }
        }
    }

    if (currentState == MENU) {
        readJoystick();
    }
    else if (currentState == STORAGE) {
        handleStorage();
        if (showDeleteConfirm) {
            drawDeleteConfirm();
            sprite.pushSprite(0, 0);
        } else if (viewingDayChart) {
            if (screenDirty) {
                drawDayChartViewer();
                sprite.pushSprite(0, 0);
                screenDirty = false;
            }
        } else if (viewingText) {
            if (screenDirty) {
                drawTextViewer();
                sprite.pushSprite(0, 0);
                screenDirty = false;
            }
        } else if (!viewingImage) {
            if (screenDirty) {
                drawFileList();
                sprite.pushSprite(0, 0);
                screenDirty = false;
            }
        }
        delay(10);
        return;
    }
    else if (currentState == CAMERA) {
        handleCameraButtons();
        handleCamera();
        delay(1);
        return;
    }

    if (screenDirty) {
        if (currentState == HOME) drawHome();
        else if (currentState == MENU) drawMenu();
        sprite.pushSprite(0, 0);
        screenDirty = false;
    }

    delay(10);
}














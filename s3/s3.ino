#include <LovyanGFX.hpp>
#include <TJpg_Decoder.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>
#include <SPI.h>
#include <SD.h>
#include <ctype.h>
#include <vector>
#include <queue>       // 用于扫雷 flood fill

// ========== 屏幕引脚 ==========
#define PIN_SCK  12
#define PIN_SDA  11
#define PIN_CS   10
#define PIN_DC   9
#define PIN_RST  8

// ========== 摇杆引脚 ==========
#define PIN_VRX  4
#define PIN_VRY  5
#define PIN_SW   6

// ========== 摄像头串口引脚 ==========
#define CAM_RX   17
#define CAM_TX   16

// ========== TF卡引脚 (SPI3) ==========
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  14
#define SD_CS    13
SPIClass sdSPI(HSPI);

// ========== 视频播放参数 ==========
#define VIDEO_FPS 12
#define VIDEO_FRAME_DELAY_MS (1000 / VIDEO_FPS)

// ---------- 屏幕驱动 ----------
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

// ========== UI 状态 ==========
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

// ========== 文件浏览相关变量 ==========
#define MAX_FILES 50
#define MAX_DEPTH 5
String currentPath = "/";
String fileList[MAX_FILES];
bool isDir[MAX_FILES];
int fileCount = 0;
int fileSelectedIndex = 0;
int listScrollOffset = 0;
bool viewingImage = false;

enum ListFocus { LIST_FILES, LIST_BOTTOM_BAR };
ListFocus listFocusArea = LIST_FILES;
int listBottomBtnIndex = 0;

enum ImageFocus { IMAGE_AREA, IMAGE_BOTTOM_BAR };
ImageFocus imageFocusArea = IMAGE_AREA;
int imageBottomBtnIndex = 0;

bool showDeleteConfirm = false;
int deleteConfirmSelection = 0;

// ========== 菜单布局 ==========
const int boxW = 90;
const int boxH = 70;
const int boxGapX = 12;
const int boxGapY = 20;
const int boxesTotalWidth = (boxW * cols) + (boxGapX * (cols - 1));
const int startX = (320 - boxesTotalWidth) / 2;
const int boxesTotalHeight = (boxH * rows) + boxGapY;
const int startY = (240 - boxesTotalHeight) / 2;

const char* menuTexts[] = { "搜索", "拍摄", "存储", "语文", "英语", "待做" };

// ========== 摄像头相关变量 ==========
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

// ========== 拍照功能 ==========
int photoIndex = 0;
bool captureRequest = false;

// ========== 帧率统计 ==========
unsigned long lastFpsPrint = 0;
uint32_t frameCount = 0;
uint32_t lastFrameCount = 0;

// ---------- 视频播放全局变量 ----------
static uint16_t* video_row_buffer = nullptr;
static int video_row_cursor = 0;
static int video_row_y = -1;
static int video_dst_w = 0, video_dst_h = 0, video_dst_x = 0, video_dst_y = 0;
static int video_src_w = 0, video_src_h = 0;

// ========== 待做 / 数字网格 / 神秘页面 相关变量 ==========
int numGridSelectedIndex = 0;
int passwordSequence[6];
int passwordIndex = 0;

// ========== 神秘页面列表 ==========
int mysterySelectedIndex = 0;
const int mysteryTotalItems = 6;
const char* mysteryGameNames[] = { "飞机游戏", "扫雷", "打砖块", "贪吃蛇", "2048", "小恐龙" };

// ========== 英语学习相关变量 ==========
#define MAX_WORDS 500
struct WordEntry {
  String word;
  String phonetic;
  String meaning;
};
WordEntry englishWords[MAX_WORDS];
int englishWordCount = 0;
int englishLearnMode = 0;   // 0=英文模式, 1=中文模式
int englishWordIndex = 0;
int englishPhase = 0;       // 0=主面, 1=翻转面

// ============================================================
//  游戏相关定义（飞机打陨石）
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
//  新增游戏全局变量
// ============================================================

// ---------- 扫雷 ----------
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
int msCellSize;         // 计算得出
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
const int GAME2048_DELAY = 150; // 修复：原为 2048_DELAY（非法标识符）

// ---------- 打砖块 ----------
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

// ---------- 贪吃蛇 ----------
#define SNAKE_MAX_LEN 200
struct Point { int x, y; };
Point snake[SNAKE_MAX_LEN];
int snakeLen;
int snakeDir; // 0上 1右 2下 3左
int snakeNextDir;
bool snakeFood;
int foodX, foodY;
int snakeCellSize;
int snakeGridW, snakeGridH;
bool snakeGameOver;
unsigned long snakeMoveTimer;
int snakeMoveInterval = 200;

// ---------- 小恐龙 ----------
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
//  函数声明（原有 + 新增）
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

void drawTodoPage();
void handleTodoPage();
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

// 飞机游戏
void initGame();
void spawnMeteor();
void shootBullet();
void updateGame();
void renderGame();
void handleGameInput();
void handleGame();

// 新增游戏声明
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
void handleSnake();
void updateSnake();
void renderSnake();

void initDino();
void handleDino();
void updateDino();
void renderDino();

// ---------- 获取下一个照片编号 ----------
int getNextPhotoIndex() {
    int i = 0;
    while (SD.exists(String("/photo_") + i + ".jpg")) i++;
    return i;
}

// ---------- 保存照片到SD卡 ----------
void savePhotoToSD(uint8_t* data, uint32_t length) {
    if (!SD.cardType()) {
        Serial.println("SD卡未初始化，无法保存照片");
        return;
    }
    char name[32];
    sprintf(name, "/photo_%d.jpg", photoIndex++);
    File f = SD.open(name, FILE_WRITE);
    if (!f) {
        Serial.println("创建文件失败");
        return;
    }
    size_t w = f.write(data, length);
    f.close();
    if (w == length) {
        Serial.printf("照片已保存: %s (%u 字节)\n", name, length);
    } else {
        Serial.println("写入失败");
    }
}

// ---------- 摄像头回调 ----------
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

// ---------- 图像缩放显示 ----------
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

// ---------- 串口数据解析 ----------
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

// ---------- 摄像头主处理 ----------
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
            Serial.printf("拍照: 帧大小 %u 字节\n", len);
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
            Serial.printf("摄像头帧率: %u fps\n", fps);
            lastFrameCount = frameCount;
            lastFpsPrint = now;
        }
    }
}

// ========== 首页绘制 ==========
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

// ========== 菜单绘制 ==========
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

// ========== 摇杆方向读取 ==========
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

// ========== 按钮按下检测 ==========
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

// ========== 相机模式按钮处理 ==========
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
            Serial.println("长按退出相机");
        }
    }
    else if (!curPressed && wasPressed) {
        if (now - pressStart < 5000) {
            captureRequest = true;
            Serial.println("拍照请求");
        }
        wasPressed = false;
    }
}

// ========== 扫描SD卡目录 ==========
void scanSD(const char* path) {
    fileCount = 0;
    fileSelectedIndex = 0;
    listScrollOffset = 0;

    if (!SD.cardType()) {
        Serial.println("SD卡未初始化");
        return;
    }

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("无法打开目录: %s\n", path);
        if (dir) dir.close();
        return;
    }

    String pathStr = String(path);
    if (pathStr != "/") {
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
        } else if (name.endsWith(".jpg") || name.endsWith(".JPG") ||
                   name.endsWith(".mjpg") || name.endsWith(".mjpeg")) {
            fileList[fileCount] = name;
            isDir[fileCount] = false;
            fileCount++;
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
    Serial.printf("扫描 %s : %d 个条目\n", path, fileCount);
}

// 帧解析器状态
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
                Serial.println("帧过大（>200KB），放弃并重置解析器");
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
        Serial.printf("无法打开视频文件: %s\n", filename);
        return;
    }

    Serial.printf("开始播放: %s\n", filename);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFont(&fonts::efontCN_16);
    tft.setCursor(5, 5);
    tft.print("播放: ");
    tft.println(filename);
    delay(1500);
    tft.fillRect(0, 0, tft.width(), 25, TFT_BLACK);

    size_t maxFrameSize = 64 * 1024;
    uint8_t* jpegFrame = (uint8_t*)malloc(maxFrameSize);
    if (!jpegFrame) {
        Serial.println("无法分配JPEG帧缓冲区");
        videoFile.close();
        return;
    }

    int screen_w = tft.width();
    video_row_buffer = (uint16_t*)malloc(screen_w * sizeof(uint16_t));
    if (!video_row_buffer) {
        Serial.println("无法分配行缓冲区");
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
            Serial.printf("JPEG缓冲区扩容至 %u 字节\n", maxFrameSize);
        }

        if (TJpgDec.getJpgSize(&jpgW, &jpgH, jpegFrame, frameLen) != JDR_OK) {
            Serial.println("帧解析失败，跳过");
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
            Serial.println("帧解码失败");
            continue;
        }

        if (video_row_cursor > 0) {
            tft.pushImage(video_dst_x, video_row_y, video_dst_w, 1, video_row_buffer);
        }

        frameCount_local++;
        unsigned long now = millis();
        if (now - lastPrintTime >= 1000) {
            Serial.printf("视频帧率: %d fps\n", frameCount_local);
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
    Serial.println("视频播放结束");
}

// ========== 绘制文件列表 + 底部按钮 ==========
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

    const int lineHeight = 22;
    const int listTop = 22;
    int maxVisible = (btnY - listTop) / lineHeight;
    if (maxVisible <= 0) maxVisible = 1;

    if (fileCount == 0) {
        sprite.setFont(&fonts::efontCN_16);
        sprite.setTextDatum(middle_center);
        sprite.setTextColor(TFT_WHITE);
        sprite.drawString("无文件", tft.width()/2, tft.height()/2);
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
            String prefix = isDir[idx] ? "[目录] " : "";
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

    const char* btnLabels[2] = {"删除", "退出"};
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

// ---------- 显示图片 ----------
void displaySelectedFile(const char* filepath, bool leaveBottomSpace) {
    File imgFile = SD.open(filepath, FILE_READ);
    if (!imgFile) {
        Serial.printf("打开失败: %s\n", filepath);
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

// ---------- 绘制图片底部按钮 ----------
void drawImageBottomBar() {
    const int btnH = 28;
    const int btnY = tft.height() - btnH - 2;
    const int btnW = 70;
    const int btnGap = 25;
    const int leftBtnX = (tft.width() - (2*btnW + btnGap)) / 2;
    const int rightBtnX = leftBtnX + btnW + btnGap;

    tft.fillRect(0, btnY - 2, tft.width(), btnH + 4, TFT_BLACK);

    const char* labels[2] = {"删除", "退出"};
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

// ---------- 删除确认弹框 ----------
void drawDeleteConfirm() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setFont(&fonts::efontCN_16);

    sprite.fillRoundRect(20, 70, tft.width() - 40, 100, 12, sprite.color565(30, 30, 30));
    sprite.drawRoundRect(20, 70, tft.width() - 40, 100, 12, TFT_YELLOW);

    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_YELLOW, sprite.color565(30, 30, 30));
    sprite.drawString("确认删除？", tft.width() / 2, 95);
    sprite.setTextColor(TFT_WHITE, sprite.color565(30, 30, 30));
    sprite.drawString(fileList[fileSelectedIndex], tft.width() / 2, 120);

    const int btnW = 60, btnH = 24, btnY = 145;
    const int leftBtnX = tft.width()/2 - btnW - 10;
    const int rightBtnX = tft.width()/2 + 10;

    const char* options[2] = {"确认", "取消"};
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

// ---------- 删除文件 ----------
void deleteFile(const char* filepath) {
    if (SD.exists(filepath)) {
        if (SD.remove(filepath)) {
            Serial.printf("已删除: %s\n", filepath);
        } else {
            Serial.printf("删除失败: %s\n", filepath);
        }
    } else {
        Serial.printf("不存在: %s\n", filepath);
    }
}

// ========== 存储模式事件处理 ==========
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
                            Serial.println("目录过深");
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
                        } else {
                            Serial.println("不支持的文件类型");
                        }
                    }
                } else {
                    if (listBottomBtnIndex == 0) {
                        if (fileCount > 0 && !isDir[fileSelectedIndex] && fileList[fileSelectedIndex] != "..") {
                            showDeleteConfirm = true;
                            deleteConfirmSelection = 0;
                            screenDirty = true;
                        }
                    } else {
                        viewingImage = false;
                        showDeleteConfirm = false;
                        currentState = MENU;
                        screenDirty = true;
                        Serial.println("退出存储");
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

// ========== 初始化 ==========
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
        Serial.println("TF卡初始化失败，拍照功能不可用");
    } else {
        Serial.println("TF卡初始化成功");
        photoIndex = getNextPhotoIndex();
        Serial.printf("下一个照片编号: %d\n", photoIndex);
    }

    drawHome();
    sprite.pushSprite(0, 0);
}

// ========== 待做页面 ==========
void drawTodoPage() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_WHITE);
    sprite.setFont(&fonts::efontCN_16);
    sprite.drawString("什么也没有", tft.width() / 2, tft.height() / 2);
}

void handleTodoPage() {
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
            numGridSelectedIndex = 0;
            passwordIndex = 0;
            currentState = NUM_GRID;
            screenDirty = true;
        } else {
            currentState = MENU;
            screenDirty = true;
        }
        wasPressed = false;
    }
}

// ========== 3×3 数字网格 ==========
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

// ========== 神秘页面（游戏列表） ==========
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
//  飞机打陨石游戏实现
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
        sprite.drawString("按 SW 重新开始", tft.width()/2, tft.height()/2 + 20);
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
//  扫雷游戏实现
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
        sprite.printf("你赢了！");
    } else if (msGameOver) {
        sprite.printf("游戏结束");
    } else {
        sprite.printf("雷: %d", 40);
    }

    sprite.pushSprite(0, 0);
}

// ============================================================
//  2048 游戏实现
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
//  打砖块实现
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
//  贪吃蛇实现
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
//  小恐龙实现
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

// ========== 英语功能实现 ==========
static bool isChineseChar(uint8_t c) {
    return (c >= 0xE4 && c <= 0xE9);
}

bool loadEnglishWords() {
    englishWordCount = 0;
    if (!SD.cardType()) {
        Serial.println("SD卡未初始化，无法读取单词");
        return false;
    }

    const char* filepath = "/english/book.txt";
    if (!SD.exists(filepath)) {
        Serial.printf("文件不存在: %s\n", filepath);
        return false;
    }

    File file = SD.open(filepath, FILE_READ);
    if (!file) {
        Serial.printf("打开文件失败: %s\n", filepath);
        return false;
    }

    Serial.println("开始逐行解析单词...");
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
            Serial.printf("已解析 %d 个单词...\n", englishWordCount);
        }
    }

    file.close();
    Serial.printf("解析完成，共 %d 个单词\n", englishWordCount);
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

    const char* labels[2] = {"中文", "英文"};

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

// ========== 主循环 ==========
void loop() {
    // 英语选择界面
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

    // 英语学习界面
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

    // 游戏状态处理
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

    // --- 原有其他状态处理 ---
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
            if (selectedIndex == 1) {
                currentState = CAMERA;
                tft.fillScreen(TFT_BLACK);
                state = WAIT_SOF;
                sof = eof = idx = 0;
                ready = false;
                captureRequest = false;
            }
            else if (selectedIndex == 2) {
                currentState = STORAGE;
                currentPath = "/";
                scanSD(currentPath.c_str());
                fileSelectedIndex = 0;
                listScrollOffset = 0;
                viewingImage = false;
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
                    tft.drawString("单词加载失败", tft.width()/2, tft.height()/2);
                    delay(1500);
                    currentState = MENU;
                    screenDirty = true;
                }
            }
            else if (selectedIndex == 5) {
                currentState = TODO_PAGE;
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

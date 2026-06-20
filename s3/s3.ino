#include <LovyanGFX.hpp>
#include <TJpg_Decoder.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>
#include <SPI.h>
#include <SD.h>

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

// ========== UI 状态（新增 TODO_PAGE, NUM_GRID, MYSTERY_PAGE）==========
enum UIState { HOME, MENU, CAMERA, STORAGE,
               TODO_PAGE,       // 新增：待做页面（黑底白字“什么也没有”）
               NUM_GRID,        // 新增：3×3 数字网格（密码输入）
               MYSTERY_PAGE };  // 新增：神秘小页面
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

// ========== 菜单布局（保持不变）==========
const int boxW = 90;
const int boxH = 70;
const int boxGapX = 12;
const int boxGapY = 20;
const int boxesTotalWidth = (boxW * cols) + (boxGapX * (cols - 1));
const int startX = (320 - boxesTotalWidth) / 2;
const int boxesTotalHeight = (boxH * rows) + boxGapY;
const int startY = (240 - boxesTotalHeight) / 2;

// --- 修改：菜单文本“对话” → “待做” ---
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

#define SWAP_BYTES true
#define FULLSCREEN_CROP false

// ========== 拍照功能 ==========
int photoIndex = 0;
bool captureRequest = false;

// ========== 帧率统计 ==========
unsigned long lastFpsPrint = 0;
uint32_t frameCount = 0;
uint32_t lastFrameCount = 0;

// ---------- 视频播放全局变量（用于行缓冲回调）----------
static uint16_t* video_row_buffer = nullptr;
static int video_row_cursor = 0;
static int video_row_y = -1;
static int video_dst_w = 0, video_dst_h = 0, video_dst_x = 0, video_dst_y = 0;
static int video_src_w = 0, video_src_h = 0;

// ========== 新增：待做 / 数字网格 / 神秘页面 相关变量 ==========
int numGridSelectedIndex = 0;                // 3×3 网格当前选中索引（0~8）
int passwordSequence[6];                     // 密码输入序列
int passwordIndex = 0;                       // 已输入密码位数

// ---------- 函数声明 ----------
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

// ---------- 新增函数声明 ----------
void drawTodoPage();
void handleTodoPage();
void drawNumGrid();
void handleNumGrid();
void drawMysteryPage();
void handleMysteryPage();

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
            size_t buf_size = jpg_width * jpg_height * sizeof(uint16_t);
            img_rgb565 = (uint16_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!img_rgb565) img_rgb565 = (uint16_t*)malloc(buf_size);
            if (img_rgb565) {
                TJpgDec.drawJpg(0, 0, buf, len);
                int screen_w = tft.width();
                int screen_h = tft.height();
                float scale_w = (float)screen_w / jpg_width;
                float scale_h = (float)screen_h / jpg_height;
                float scale = (scale_w < scale_h) ? scale_w : scale_h;
                int dst_w = jpg_width * scale;
                int dst_h = jpg_height * scale;
                int dst_x = (screen_w - dst_w) / 2;
                int dst_y = (screen_h - dst_h) / 2;
                draw_scaled_image(img_rgb565, jpg_width, jpg_height, dst_x, dst_y, dst_w, dst_h);

                tft.setTextColor(TFT_WHITE, TFT_BLACK);
                tft.setCursor(5, tft.height() - 10);
                tft.printf("FPS: %u", frameCount - lastFrameCount);

                free(img_rgb565);
                img_rgb565 = nullptr;
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

// ========== 摇杆方向读取（菜单）==========
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

    // 简单排序
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

// 帧解析器静态状态（用于playOneFrame）
static uint8_t parser_state = 0;        // 0:寻找SOI, 1:收集帧
static size_t parser_bytesInFrame = 0;
static bool parser_foundFF = false;
static bool parser_prevWasFF = false;

void resetParser() {
    parser_state = 0;
    parser_bytesInFrame = 0;
    parser_foundFF = false;
    parser_prevWasFF = false;
}

// 从MJPEG文件中提取完整一帧（增强容错版）
bool playOneFrame(File &file, uint8_t *frameBuf, size_t &frameLen, bool &stopFlag) {
    if (stopFlag) return false;

    while (file.available()) {
        uint8_t b = file.read();

        if (parser_state == 0) {
            // 寻找SOI (0xFF, 0xD8)
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
            // 收集帧数据直到找到EOI (0xFF, 0xD9)
            frameBuf[parser_bytesInFrame++] = b;

            // 安全上限：如果超过200KB则放弃
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

// ---------- 视频播放专用回调：收集解码输出，按行推送（无闪烁，低内存）----------
bool video_tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!video_row_buffer) return true;
    if (video_dst_w <= 0 || video_dst_h <= 0) return true;

    for (uint16_t row = 0; row < h; row++) {
        int src_y = y + row;
        if (src_y < 0 || src_y >= video_src_h) continue;
        int dst_y = video_dst_y + (src_y * video_dst_h) / video_src_h;
        if (dst_y < 0 || dst_y >= tft.height()) continue;

        // 如果新行的y坐标与上一行不同，则先推送上一行
        if (video_row_y != dst_y && video_row_cursor > 0) {
            tft.pushImage(video_dst_x, video_row_y, video_dst_w, 1, video_row_buffer);
            video_row_cursor = 0;
        }
        video_row_y = dst_y;

        // 将当前解码块中的像素填入行缓冲区
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

// ---------- 视频播放主函数（使用行缓冲区，内存占用极小）----------
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

    // 动态分配JPEG帧缓冲区（初始64KB，不够则扩容）
    size_t maxFrameSize = 64 * 1024;
    uint8_t* jpegFrame = (uint8_t*)malloc(maxFrameSize);
    if (!jpegFrame) {
        Serial.println("无法分配JPEG帧缓冲区");
        videoFile.close();
        return;
    }

    // 分配行缓冲区（只需要一行，内存占用 = 屏幕宽度 * 2 字节）
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

        // 计算缩放参数
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

        // 重置行缓冲区状态
        video_row_cursor = 0;
        video_row_y = -1;

        // 解码并逐行推送
        if (TJpgDec.drawJpg(0, 0, jpegFrame, frameLen) != JDR_OK) {
            Serial.println("帧解码失败");
            continue;
        }

        // 推送最后一行
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

        // 帧率控制
        uint32_t elapsed = millis() - frameStart;
        if (elapsed < VIDEO_FRAME_DELAY_MS) {
            delay(VIDEO_FRAME_DELAY_MS - elapsed);
        }

        // 检测按键退出
        if (digitalRead(PIN_SW) == LOW) {
            stopPlaying = true;
            break;
        }
    }

    // 恢复原回调并释放资源
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

// ---------- 显示图片（留出底部按钮空间）----------
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

// ---------- 绘制图片查看时的底部按钮 ----------
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

    // 删除确认弹框交互
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

    // 文件列表模式
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

    // 图片查看模式
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

// ========== 新增：待做页面绘制 ==========
void drawTodoPage() {
    sprite.fillScreen(TFT_BLACK);
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_WHITE);
    sprite.setFont(&fonts::efontCN_16);
    sprite.drawString("什么也没有", tft.width() / 2, tft.height() / 2);
}

// ========== 新增：待做页面交互（短按返回菜单，长按3秒进入数字网格）==========
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
            // 长按3秒：进入3×3数字网格
            numGridSelectedIndex = 0;
            passwordIndex = 0;               // 清空密码输入序列
            currentState = NUM_GRID;
            screenDirty = true;
        } else {
            // 短按：返回菜单
            currentState = MENU;
            screenDirty = true;
        }
        wasPressed = false;
    }
}

// ========== 新增：3×3 数字网格绘制（美化版，独立尺寸，不影响菜单）==========
void drawNumGrid() {
    sprite.fillScreen(TFT_BLACK);

    // 与菜单完全一致的选中框颜色风格
    const uint16_t BOX_COLOR_LIGHT_BLUE = sprite.color565(135, 206, 235);
    const uint16_t SELECT_COLOR_YELLOW  = sprite.color565(255, 220, 0);
    const uint16_t TEXT_COLOR_BLACK     = TFT_BLACK;
    const uint16_t BORDER_WHITE         = TFT_WHITE;
    sprite.setFont(&fonts::efontCN_16);

    const int gridCols = 3;
    const int gridRows = 3;

    // 【修改】使用独立的网格尺寸，避免与菜单的 boxW/boxH 冲突，适配 320x240 屏幕
    const int gridBoxW = 72;   // 方框宽度（略小于菜单的90，保证3个不拥挤）
    const int gridBoxH = 60;   // 方框高度（适配3行 + 间隙在240高度内）
    const int gridGapX = 20;   // 水平间隙
    const int gridGapY = 16;   // 垂直间隙

    const int gridTotalWidth = (gridBoxW * gridCols) + (gridGapX * (gridCols - 1));
    const int gridStartX = (tft.width() - gridTotalWidth) / 2;
    const int gridTotalHeight = (gridBoxH * gridRows) + (gridGapY * (gridRows - 1));
    const int gridStartY = (tft.height() - gridTotalHeight) / 2;

    for (int i = 0; i < 9; i++) {
        int col = i % gridCols;
        int row = i / gridCols;
        int x = gridStartX + col * (gridBoxW + gridGapX);
        int y = gridStartY + row * (gridBoxH + gridGapY);

        // 选中框样式完全复刻菜单风格（多层发光边框）
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

// ========== 新增：3×3 数字网格交互（摇杆移动、短按记录密码、长按退出）==========
void handleNumGrid() {
    // --- 摇杆移动（使用局部延时避免卡键）---
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
            sprite.pushSprite(0, 0);       // 立即刷新
        }
    }

    // --- 按钮处理（短按记录数字，长按退出）---
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
            // 长按3秒：退出数字网格，回到待做页面
            currentState = TODO_PAGE;
            screenDirty = true;
            passwordIndex = 0;             // 清空输入序列
        } else {
            // 短按：记录当前选中的数字（1~9）
            int digit = numGridSelectedIndex + 1;
            if (passwordIndex < 6) {
                passwordSequence[passwordIndex] = digit;
                passwordIndex++;
                // 如果输入满6位，检查密码
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
                        // 密码正确 → 进入神秘页面
                        currentState = MYSTERY_PAGE;
                        screenDirty = true;
                    }
                    // 无论正确与否，清空序列以便重新输入
                    passwordIndex = 0;
                }
            }
        }
        wasPressed = false;
    }

    // 首次进入或 screenDirty 时刷新画面
    if (screenDirty) {
        drawNumGrid();
        sprite.pushSprite(0, 0);
        screenDirty = false;
    }
}

// ========== 新增：神秘页面绘制（长方形框 + 标题）==========
void drawMysteryPage() {
    sprite.fillScreen(TFT_BLACK);
    const int boxW = 200;
    const int boxH = 120;
    int boxX = (tft.width() - boxW) / 2;
    int boxY = (tft.height() - boxH) / 2;

    sprite.fillRoundRect(boxX, boxY, boxW, boxH, 12, sprite.color565(30, 30, 30));
    sprite.drawRoundRect(boxX, boxY, boxW, boxH, 12, TFT_WHITE);

    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_WHITE);
    sprite.setFont(&fonts::efontCN_16);
    sprite.drawString("神秘小页面", tft.width() / 2, tft.height() / 2);
}

// ========== 新增：神秘页面交互（短按返回菜单）==========
void handleMysteryPage() {
    static bool wasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);

    if (curPressed && !wasPressed) {
        wasPressed = true;
    } else if (!curPressed && wasPressed) {
        currentState = MENU;
        screenDirty = true;
        wasPressed = false;
    }
}

// ========== 主循环（新增状态处理，其余完全不变）==========
void loop() {
    // --- 新增状态优先处理，避免干扰原有 isSWPressed 逻辑 ---
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
        if (screenDirty) {
            drawMysteryPage();
            sprite.pushSprite(0, 0);
            screenDirty = false;
        }
        delay(10);
        return;
    }

    // --- 原有逻辑，完全不变 ---
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
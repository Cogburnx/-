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
enum UIState { HOME, MENU, CAMERA, STORAGE };  // NEW: 新增 STORAGE 状态
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

// ========== 文件浏览相关变量（NEW） ==========
#define MAX_FILES 50
#define MAX_DEPTH 5             // NEW: 目录深度限制
String currentPath = "/";       // NEW: 当前浏览路径
String fileList[MAX_FILES];
bool isDir[MAX_FILES];          // NEW: 标记是否为文件夹
int fileCount = 0;
int fileSelectedIndex = 0;
int listScrollOffset = 0;
bool viewingImage = false;

// 删除确认相关（NEW）
bool showDeleteConfirm = false;
int deleteConfirmIndex = -1;

// ========== 菜单布局（横屏 320x240）==========
const int boxW = 90;
const int boxH = 70;
const int boxGapX = 12;
const int boxGapY = 20;
const int boxesTotalWidth = (boxW * cols) + (boxGapX * (cols - 1));
const int startX = (320 - boxesTotalWidth) / 2;
const int boxesTotalHeight = (boxH * rows) + boxGapY;
const int startY = (240 - boxesTotalHeight) / 2;

const char* menuTexts[] = { "搜索", "拍摄", "存储", "语文", "英语", "对话" };

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

// ========== 录像功能 ==========
bool isRecording = false;
File recordFile;
unsigned long recordStartTime = 0;
uint32_t recordedFrames = 0;
int videoIndex = 0;

// ========== 帧率统计 ==========
unsigned long lastFpsPrint = 0;
uint32_t frameCount = 0;
uint32_t lastFrameCount = 0;

// ---------- 获取下一个照片编号 ----------
int getNextPhotoIndex() {
  int i = 0;
  while (SD.exists(String("/photo_") + i + ".jpg")) i++;
  return i;
}

// ---------- 获取下一个录像文件编号 ----------
int getNextVideoIndex() {
  int i = 0;
  while (SD.exists(String("/video_") + i + ".mjpeg")) i++;
  return i;
}

// ---------- 开始录像 ----------
void startRecording() {
  if (!SD.cardType()) {
    Serial.println("SD卡未初始化，无法录像");
    return;
  }
  if (isRecording) return;
  char name[32];
  sprintf(name, "/video_%d.mjpeg", videoIndex++);
  recordFile = SD.open(name, FILE_WRITE);
  if (!recordFile) {
    Serial.println("创建录像文件失败");
    return;
  }
  isRecording = true;
  recordedFrames = 0;
  recordStartTime = millis();
  Serial.printf("开始录像: %s\n", name);
}

// ---------- 停止录像 ----------
void stopRecording() {
  if (!isRecording) return;
  if (recordFile) {
    recordFile.flush();
    recordFile.close();
    Serial.printf("停止录像，共录 %u 帧，时长 %.2f 秒\n", 
                  recordedFrames, (millis() - recordStartTime) / 1000.0);
  }
  isRecording = false;
  recordedFrames = 0;
}

// ---------- 将当前JPEG帧写入录像文件 ----------
void recordFrame(uint8_t* data, uint32_t length) {
  if (!isRecording) return;
  if (!recordFile) {
    isRecording = false;
    return;
  }
  size_t written = recordFile.write(data, length);
  if (written == length) {
    recordedFrames++;
    if (recordedFrames % 30 == 0) {
      Serial.printf("录像中... 已录 %u 帧\n", recordedFrames);
    }
  } else {
    Serial.println("写入录像帧失败，停止录像");
    stopRecording();
  }
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

// ---------- 摄像头回调（填充 img_rgb565）----------
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

// ---------- 串口数据解析（摄像头协议）----------
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

    if (isRecording) {
      if (recordFile) {
        size_t written = recordFile.write(buf, len);
        if (written == len) {
          recordedFrames++;
          recordFile.flush();
          if (recordedFrames % 10 == 0) {
            Serial.printf("录像中... 已录 %u 帧, 最新帧大小 %u\n", recordedFrames, len);
          }
        } else {
          Serial.println("写入录像帧失败，停止录像");
          stopRecording();
        }
      } else {
        isRecording = false;
      }
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

        if (isRecording) {
          tft.fillCircle(tft.width() - 10, 10, 6, TFT_RED);
          tft.setTextColor(TFT_WHITE, TFT_BLACK);
          tft.setTextSize(1);
          tft.setCursor(5, tft.height() - 10);
          tft.printf("REC %u", recordedFrames);
          uint8_t hash = 0;
          for (int i = 0; i < 16 && i < len; i++) hash ^= buf[i];
          tft.setCursor(5, tft.height() - 20);
          tft.printf("CHK %02X", hash);
        } else {
          tft.setTextColor(TFT_WHITE, TFT_BLACK);
          tft.setCursor(5, tft.height() - 10);
          tft.printf("FPS: %u", frameCount - lastFrameCount);
        }
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
  sprite.setFont(&fonts::efontCN_16);   // 需要中文字体

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

// ========== 摇杆方向读取（用于菜单）==========
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

// ========== 相机模式下的按钮处理 ==========
void handleCameraButtons() {
  static unsigned long pressStart = 0;
  static bool exitDone = false;
  static bool recordToggleDone = false;
  static bool wasPressed = false;
  bool curPressed = (digitalRead(PIN_SW) == LOW);
  unsigned long now = millis();

  if (curPressed && !wasPressed) {
    pressStart = now;
    exitDone = false;
    recordToggleDone = false;
    wasPressed = true;
  }
  else if (curPressed && wasPressed) {
    unsigned long duration = now - pressStart;
    
    if (!exitDone && duration >= 5000) {
      exitDone = true;
      if (isRecording) stopRecording();
      currentState = MENU;
      screenDirty = true;
      while (SerialCam.available()) SerialCam.read();
      state = WAIT_SOF;
      sof = eof = idx = 0;
      ready = false;
      captureRequest = false;
      Serial.println("长按5秒退出相机");
    }
    else if (!exitDone && !recordToggleDone && duration >= 2000) {
      recordToggleDone = true;
      if (isRecording) {
        stopRecording();
        Serial.println("停止录像");
      } else {
        startRecording();
        Serial.println("开始录像");
      }
    }
  }
  else if (!curPressed && wasPressed) {
    unsigned long duration = now - pressStart;
    if (!exitDone && !recordToggleDone && duration < 2000) {
      captureRequest = true;
      Serial.println("拍照请求");
    }
    wasPressed = false;
    exitDone = false;
    recordToggleDone = false;
  }
}

// ---------- 文件浏览函数声明（NEW）----------
void scanSD(const char* path);
void drawFileList();
void handleStorage();
void displaySelectedFile(const char* filepath);
void deleteFile(const char* filepath);
void drawDeleteConfirm();  // NEW

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
    Serial.println("TF卡初始化失败，拍照/录像功能不可用");
  } else {
    Serial.println("TF卡初始化成功");
    photoIndex = getNextPhotoIndex();
    videoIndex = getNextVideoIndex();
    Serial.printf("下一个照片编号: %d, 录像编号: %d\n", photoIndex, videoIndex);
  }

  drawHome();
  sprite.pushSprite(0, 0);
}

// ========== 主循环 ==========
void loop() {
  if (currentState != CAMERA && currentState != STORAGE && isSWPressed()) { // MOD: 排除STORAGE
    if (currentState == HOME) {
      currentState = MENU;
      selectedIndex = 0;
      screenDirty = true;
    } 
    else if (currentState == MENU) {
      if (selectedIndex == 1) {   // “拍摄”
        currentState = CAMERA;
        tft.fillScreen(TFT_BLACK);
        state = WAIT_SOF;
        sof = eof = idx = 0;
        ready = false;
        captureRequest = false;
        if (isRecording) stopRecording();
      }
      // ---------- NEW: 处理“存储” ----------
      else if (selectedIndex == 2) {
        currentState = STORAGE;
        currentPath = "/";             // NEW: 重置路径
        scanSD(currentPath.c_str());   // MOD: 传入路径
        fileSelectedIndex = 0;
        listScrollOffset = 0;
        viewingImage = false;
        showDeleteConfirm = false;     // NEW
        screenDirty = true;
        tft.fillScreen(TFT_BLACK);
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
  // ---------- NEW: 处理文件浏览状态 ----------
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

// ---------- 扫描SD卡指定目录（NEW）----------
void scanSD(const char* path) {
  fileCount = 0;
  fileSelectedIndex = 0;
  listScrollOffset = 0;

  if (!SD.cardType()) {
    Serial.println("SD卡未初始化，无法扫描");
    return;
  }

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("无法打开目录: %s\n", path);
    if (dir) dir.close();
    return;
  }

  // 如果不在根目录，添加“..”返回上级
  String pathStr = String(path);
  if (pathStr != "/") {
    fileList[fileCount] = "..";
    isDir[fileCount] = true;
    fileCount++;
  }

  File entry = dir.openNextFile();
  while (entry && fileCount < MAX_FILES) {
    String name = entry.name();
    // 过滤系统隐藏文件
    if (name.startsWith(".")) {
      entry.close();
      entry = dir.openNextFile();
      continue;
    }

    if (entry.isDirectory()) {
      fileList[fileCount] = name;
      isDir[fileCount] = true;
      fileCount++;
    } else if (name.endsWith(".jpg") || name.endsWith(".JPG")) {
      fileList[fileCount] = name;
      isDir[fileCount] = false;
      fileCount++;
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  // 排序：文件夹在前，文件在后，各自按名称排序
  for (int i = 0; i < fileCount - 1; i++) {
    for (int j = i + 1; j < fileCount; j++) {
      bool swap = false;
      if (isDir[i] && !isDir[j]) {
        swap = false;
      } else if (!isDir[i] && isDir[j]) {
        swap = true;
      } else {
        if (fileList[i] > fileList[j]) swap = true;
      }
      if (swap) {
        String tmpName = fileList[i];
        fileList[i] = fileList[j];
        fileList[j] = tmpName;
        bool tmpDir = isDir[i];
        isDir[i] = isDir[j];
        isDir[j] = tmpDir;
      }
    }
  }

  Serial.printf("扫描 %s : %d 个条目\n", path, fileCount);
}

// ---------- 绘制文件列表到 sprite（NEW美化版）----------
void drawFileList() {
  sprite.fillScreen(TFT_BLACK);
  sprite.setTextSize(1);
  sprite.setFont(&fonts::Font2);

  // 显示当前路径
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.setCursor(5, 5);
  sprite.print(currentPath);

  if (fileCount == 0) {
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(TFT_WHITE);
    sprite.drawString("No File", tft.width() / 2, tft.height() / 2);
    return;
  }

  const int lineHeight = 22;
  const int listTop = 18;
  int maxVisible = (tft.height() - listTop) / lineHeight;
  int startY = listTop + 2;

  for (int i = 0; i < maxVisible; i++) {
    int idx = listScrollOffset + i;
    if (idx >= fileCount) break;

    int y = startY + i * lineHeight;
    bool selected = (idx == fileSelectedIndex);

    if (selected) {
      // 圆角高亮背景 + 多层发光边框
      sprite.fillRoundRect(2, y - 1, tft.width() - 4, lineHeight, 6, TFT_YELLOW);
      sprite.drawRoundRect(0, y - 3, tft.width(), lineHeight + 6, 8, sprite.color565(255, 200, 0));
      sprite.drawRoundRect(1, y - 2, tft.width() - 2, lineHeight + 4, 7, sprite.color565(255, 180, 0));
      sprite.drawRoundRect(2, y - 1, tft.width() - 4, lineHeight, 6, TFT_WHITE);
    }

    String prefix = "";
    uint16_t textColor;
    uint16_t bgColor = selected ? TFT_YELLOW : TFT_BLACK;

    if (isDir[idx]) {
      prefix = "[D] ";
      textColor = selected ? TFT_BLACK : sprite.color565(100, 200, 255); // 浅蓝
    } else {
      textColor = selected ? TFT_BLACK : TFT_LIGHTGREY;
    }

    String displayName = prefix + fileList[idx];
    if (displayName.length() > 24) {
      displayName = displayName.substring(0, 21) + "...";
    }

    sprite.setTextColor(textColor, bgColor);
    sprite.setCursor(8, y + 4);
    sprite.print(displayName);
  }
}

// ---------- 删除确认弹框（NEW）----------
void drawDeleteConfirm() {
  sprite.fillScreen(TFT_BLACK);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::Font2);

  // 半透明背景框
  sprite.fillRoundRect(20, 60, tft.width() - 40, 120, 12, sprite.color565(30, 30, 30));
  sprite.drawRoundRect(20, 60, tft.width() - 40, 120, 12, TFT_YELLOW);

  sprite.setTextColor(TFT_YELLOW, sprite.color565(30, 30, 30));
  sprite.drawString("确认删除？", tft.width() / 2, 95);
  sprite.setTextColor(TFT_WHITE, sprite.color565(30, 30, 30));
  sprite.drawString(fileList[deleteConfirmIndex], tft.width() / 2, 120);
  sprite.setTextColor(TFT_LIGHTGREY, sprite.color565(30, 30, 30));
  sprite.drawString("短按: 确认  长按: 取消", tft.width() / 2, 155);
}

// ---------- 存储模式事件处理（NEW增强版）----------
void handleStorage() {
  // ===== 删除确认状态 =====
  if (showDeleteConfirm) {
    static unsigned long confirmPressStart = 0;
    static bool confirmWasPressed = false;
    bool curPressed = (digitalRead(PIN_SW) == LOW);

    if (curPressed && !confirmWasPressed) {
      confirmPressStart = millis();
      confirmWasPressed = true;
    }
    else if (curPressed && confirmWasPressed) {
      if (millis() - confirmPressStart >= 1500) { // 长按取消
        showDeleteConfirm = false;
        deleteConfirmIndex = -1;
        confirmWasPressed = false;
        screenDirty = true;
        Serial.println("删除取消");
        return;
      }
    }
    else if (!curPressed && confirmWasPressed) {
      if (millis() - confirmPressStart < 1500) { // 短按确认删除
        String fullPath = currentPath;
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += fileList[deleteConfirmIndex];
        deleteFile(fullPath.c_str());
        showDeleteConfirm = false;
        deleteConfirmIndex = -1;
        scanSD(currentPath.c_str());
        screenDirty = true;
      }
      confirmWasPressed = false;
    }
    return;
  }

  // ===== 正常浏览模式 =====
  static unsigned long lastJoyTime = 0;
  const unsigned long joyDelay = 200;
  int vry = analogRead(PIN_VRY);
  bool joyMoved = false;

  if (millis() - lastJoyTime > joyDelay) {
    if (vry < 2048 - joyThreshold) {       // 上
      if (fileCount > 0) {
        fileSelectedIndex = (fileSelectedIndex - 1 + fileCount) % fileCount;
        joyMoved = true;
      }
    } else if (vry > 2048 + joyThreshold) { // 下
      if (fileCount > 0) {
        fileSelectedIndex = (fileSelectedIndex + 1) % fileCount;
        joyMoved = true;
      }
    }

    if (joyMoved) {
      lastJoyTime = millis();
      // 滚动使选中项居中
      int maxVisible = (tft.height() - 18) / 22;
      int centerOffset = maxVisible / 2;
      listScrollOffset = fileSelectedIndex - centerOffset;
      if (listScrollOffset < 0) listScrollOffset = 0;
      if (listScrollOffset > fileCount - maxVisible) listScrollOffset = fileCount - maxVisible;
      if (listScrollOffset < 0) listScrollOffset = 0;
      screenDirty = true;
    }
  }

  // 按钮处理
  static unsigned long pressStart = 0;
  static bool wasPressed = false;
  bool curPressed = (digitalRead(PIN_SW) == LOW);

  if (curPressed && !wasPressed) {
    pressStart = millis();
    wasPressed = true;
  }
  else if (curPressed && wasPressed) {
    // 等待释放判定
  }
  else if (!curPressed && wasPressed) {
    unsigned long duration = millis() - pressStart;
    if (fileCount == 0) {
      wasPressed = false;
      return;
    }

    if (duration < 1500) { // 短按
      if (isDir[fileSelectedIndex]) {       // 文件夹或".."
        String name = fileList[fileSelectedIndex];
        if (name == "..") {
          // 返回上级目录
          int lastSlash = currentPath.lastIndexOf('/');
          if (lastSlash == 0) currentPath = "/";
          else currentPath = currentPath.substring(0, lastSlash);
        } else {
          if (!currentPath.endsWith("/")) currentPath += "/";
          currentPath += name;
        }
        // 限制目录深度
        int depth = 0;
        for (int i = 0; i < currentPath.length(); i++) if (currentPath[i] == '/') depth++;
        if (depth > MAX_DEPTH) {
          int lastSlash = currentPath.lastIndexOf('/');
          if (lastSlash == 0) currentPath = "/";
          else currentPath = currentPath.substring(0, lastSlash);
          Serial.println("目录过深，禁止进入");
        }
        scanSD(currentPath.c_str());
        fileSelectedIndex = 0;
        listScrollOffset = 0;
        viewingImage = false;
        screenDirty = true;
      } else {                              // 文件：显示图片
        String fullPath = currentPath;
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += fileList[fileSelectedIndex];
        viewingImage = true;
        displaySelectedFile(fullPath.c_str());
      }
    }
    else { // 长按 (>=1500ms)
      if (!isDir[fileSelectedIndex] && fileList[fileSelectedIndex] != "..") {
        // 长按文件：弹出删除确认
        showDeleteConfirm = true;
        deleteConfirmIndex = fileSelectedIndex;
        screenDirty = true;
      } else {
        // 长按文件夹或".."：返回上级或菜单
        if (fileList[fileSelectedIndex] == ".." || currentPath != "/") {
          int lastSlash = currentPath.lastIndexOf('/');
          if (lastSlash == 0) currentPath = "/";
          else currentPath = currentPath.substring(0, lastSlash);
          scanSD(currentPath.c_str());
          fileSelectedIndex = 0;
          listScrollOffset = 0;
          viewingImage = false;
          screenDirty = true;
        } else {
          // 根目录长按文件夹：返回MENU
          viewingImage = false;
          showDeleteConfirm = false;
          currentState = MENU;
          screenDirty = true;
          Serial.println("长按返回菜单");
        }
      }
    }
    wasPressed = false;
  }
}

// ---------- 显示选中的图片（NEW参数化）----------
void displaySelectedFile(const char* filepath) {
  File imgFile = SD.open(filepath, FILE_READ);
  if (!imgFile) {
    Serial.printf("打开文件失败: %s\n", filepath);
    return;
  }

  size_t fileSize = imgFile.size();
  uint8_t* jpgBuf = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpgBuf) jpgBuf = (uint8_t*)malloc(fileSize);
  if (!jpgBuf) {
    Serial.println("内存不足，无法加载图片");
    imgFile.close();
    return;
  }

  size_t readLen = imgFile.read(jpgBuf, fileSize);
  imgFile.close();

  if (readLen != fileSize) {
    Serial.println("读取文件不完整");
    free(jpgBuf);
    return;
  }

  uint16_t jpg_w, jpg_h;
  if (TJpgDec.getJpgSize(&jpg_w, &jpg_h, jpgBuf, fileSize) != JDR_OK) {
    Serial.println("非有效JPEG");
    free(jpgBuf);
    return;
  }

  size_t rgbSize = jpg_w * jpg_h * sizeof(uint16_t);
  img_rgb565 = (uint16_t*)heap_caps_malloc(rgbSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!img_rgb565) img_rgb565 = (uint16_t*)malloc(rgbSize);
  if (!img_rgb565) {
    Serial.println("RGB缓冲分配失败");
    free(jpgBuf);
    return;
  }

  TJpgDec.drawJpg(0, 0, jpgBuf, fileSize);

  tft.fillScreen(TFT_BLACK);
  int screen_w = tft.width();
  int screen_h = tft.height();
  float scale_w = (float)screen_w / jpg_w;
  float scale_h = (float)screen_h / jpg_h;
  float scale = (scale_w < scale_h) ? scale_w : scale_h;
  int dst_w = jpg_w * scale;
  int dst_h = jpg_h * scale;
  int dst_x = (screen_w - dst_w) / 2;
  int dst_y = (screen_h - dst_h) / 2;
  draw_scaled_image(img_rgb565, jpg_w, jpg_h, dst_x, dst_y, dst_w, dst_h);

  free(img_rgb565);
  img_rgb565 = nullptr;
  free(jpgBuf);
  Serial.printf("图片显示完成: %s\n", filepath);
}

// ---------- 删除文件（NEW）----------
void deleteFile(const char* filepath) {
  if (SD.exists(filepath)) {
    if (SD.remove(filepath)) {
      Serial.printf("已删除: %s\n", filepath);
    } else {
      Serial.printf("删除失败: %s\n", filepath);
    }
  } else {
    Serial.printf("文件不存在: %s\n", filepath);
  }
}

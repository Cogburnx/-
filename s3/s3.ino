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
enum UIState { HOME, MENU, CAMERA };
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
    recordFile.flush();  // 确保所有数据写入SD卡
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
    // 每30帧打印一次，避免刷屏
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

// ---------- 摄像头主处理（增强版：显示帧校验，flush写入）----------
void handleCamera() {
  while (SerialCam.available()) {
    process(SerialCam.read());
  }

  if (ready) {
    ready = false;
    frameCount++;

    // 拍照
    if (captureRequest) {
      captureRequest = false;
      savePhotoToSD(buf, len);
      Serial.printf("拍照: 帧大小 %u 字节\n", len);
    }

    // 录像：写入帧并立即 flush（降低缓存风险）
    if (isRecording) {
      if (recordFile) {
        size_t written = recordFile.write(buf, len);
        if (written == len) {
          recordedFrames++;
          recordFile.flush();   // 强制写入SD卡，避免断电丢帧
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

    // 解码并显示
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

        // 显示录像状态和帧校验
        if (isRecording) {
          tft.fillCircle(tft.width() - 10, 10, 6, TFT_RED);
          tft.setTextColor(TFT_WHITE, TFT_BLACK);
          tft.setTextSize(1);
          tft.setCursor(5, tft.height() - 10);
          tft.printf("REC %u", recordedFrames);
          // 显示当前帧的简单校验（用于判断画面是否变化）
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

    // 帧率统计
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
  sprite.setFont(&fonts::efontCN_16);   // 需要中文字体，若没有可改为其他字体

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

// ========== 按钮按下检测（短按，用于HOME/MENU）==========
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

// ========== 相机模式下的按钮处理（修复版：长按5秒优先，录像后仍可退出）==========
void handleCameraButtons() {
  static unsigned long pressStart = 0;
  static bool exitDone = false;           // 本次按下是否已执行退出
  static bool recordToggleDone = false;   // 本次按下是否已执行录像切换
  static bool wasPressed = false;
  bool curPressed = (digitalRead(PIN_SW) == LOW);
  unsigned long now = millis();

  if (curPressed && !wasPressed) {
    // 刚按下
    pressStart = now;
    exitDone = false;
    recordToggleDone = false;
    wasPressed = true;
  }
  else if (curPressed && wasPressed) {
    // 按住过程中，持续检查时长
    unsigned long duration = now - pressStart;
    
    // 最高优先级：长按5秒退出相机
    if (!exitDone && duration >= 5000) {
      exitDone = true;
      if (isRecording) stopRecording();
      currentState = MENU;
      screenDirty = true;
      // 清空摄像头缓冲区和状态机
      while (SerialCam.available()) SerialCam.read();
      state = WAIT_SOF;
      sof = eof = idx = 0;
      ready = false;
      captureRequest = false;
      Serial.println("长按5秒退出相机");
    }
    // 其次：长按2秒切换录像（仅当未退出且未执行过录像切换时）
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
    // 释放按钮
    unsigned long duration = now - pressStart;
    if (!exitDone && !recordToggleDone && duration < 2000) {
      // 短按拍照
      captureRequest = true;
      Serial.println("拍照请求");
    }
    wasPressed = false;
    // 重置标志，为下次按下做准备
    exitDone = false;
    recordToggleDone = false;
  }
}

// ========== 初始化 ==========
void setup() {
  Serial.begin(115200);
  pinMode(PIN_SW, INPUT_PULLUP);
  analogReadResolution(12);

  tft.init();
  tft.setRotation(1);        // 横屏，宽320高240
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
  if (currentState != CAMERA && isSWPressed()) {
    if (currentState == HOME) {
      currentState = MENU;
      selectedIndex = 0;
      screenDirty = true;
    } 
    else if (currentState == MENU) {
      if (selectedIndex == 1) {   // “拍摄”项
        currentState = CAMERA;
        tft.fillScreen(TFT_BLACK);
        // 重置摄像头解析状态机
        state = WAIT_SOF;
        sof = eof = idx = 0;
        ready = false;
        captureRequest = false;
        if (isRecording) stopRecording();
      } else {
        currentState = HOME;
        screenDirty = true;
      }
    }
  }

  if (currentState == MENU) {
    readJoystick();               // 摇杆控制菜单光标
  }
  else if (currentState == CAMERA) {
    handleCameraButtons();        // 处理拍照/录像/退出
    handleCamera();               // 接收摄像头数据，显示预览，响应拍照/录像请求
    delay(1);
    return;                       // 相机模式下无需推 sprite
  }

  // HOME 或 MENU 状态使用 sprite 绘图
  if (screenDirty) {
    if (currentState == HOME) drawHome();
    else if (currentState == MENU) drawMenu();
    sprite.pushSprite(0, 0);
    screenDirty = false;
  }

  delay(10);
}

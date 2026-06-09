#include "esp_camera.h"
#include <HardwareSerial.h>

// AI Thinker 摄像头引脚
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define CAM_TX 15
#define CAM_RX 16
HardwareSerial SerialCam(2);

const uint8_t SOF[] = {0xAA,0xAA,0xAA,0x01};
const uint8_t EOF_MARK[] = {0xAA,0xAA,0xAA,0x02};

void setup() {
  Serial.begin(115200);
  SerialCam.begin(115200, SERIAL_8N1, CAM_RX, CAM_TX);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size   = FRAMESIZE_QQVGA; // ⭐降分辨率提高成功率
  config.jpeg_quality = 25;
  config.fb_count     = 1;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init FAILED");
    while(1);
  }

  Serial.println("Camera OK");
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Capture failed");
    delay(100);
    return;
  }

  Serial.printf("Send size: %d\n", fb->len);

  SerialCam.write(SOF, 4);

  uint32_t len = fb->len;
  uint8_t lenBuf[4] = {
    (uint8_t)(len >> 24),
    (uint8_t)(len >> 16),
    (uint8_t)(len >> 8),
    (uint8_t)(len)
  };

  SerialCam.write(lenBuf, 4);
  SerialCam.write(fb->buf, fb->len);
  SerialCam.write(EOF_MARK, 4);

  esp_camera_fb_return(fb);

  delay(200); // ⭐慢一点，保证稳定
}

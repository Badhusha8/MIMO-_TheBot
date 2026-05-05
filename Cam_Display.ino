#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "esp_camera.h"

// ── TFT Pins ──────────────────────────────────────────────────
#define TFT_CS    38
#define TFT_RST   45
#define TFT_DC    46
#define TFT_MOSI  47
#define TFT_SCK   21

// ── Camera Pins ───────────────────────────────────────────────
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM      16
#define Y8_GPIO_NUM      17
#define Y7_GPIO_NUM      18
#define Y6_GPIO_NUM      12
#define Y5_GPIO_NUM      10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM      11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM    13

// ── Landscape dimensions ──────────────────────────────────────
#define CAM_W   320
#define CAM_H   240
#define TFT_W   160
#define TFT_H   128

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);

uint16_t lineBuf[TFT_W];

// ── Camera Init ───────────────────────────────────────────────
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_QVGA;   // 320x240
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera FAILED: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);
  s->set_vflip(s, 0);      // change to 1 if image upside down
  s->set_hmirror(s, 0);    // change to 1 if image mirrored
  s->set_brightness(s, 1);
  s->set_saturation(s, 0);

  return true;
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Booting...");

  tft.initR(INITR_BLACKTAB);  // try INITR_GREENTAB if colors look wrong
  tft.setRotation(1);          // landscape — try 3 if flipped
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(5, 55);
  tft.print("Starting cam...");

  if (!initCamera()) {
    tft.fillScreen(ST77XX_RED);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(5, 55);
    tft.print("Cam FAILED!");
    tft.setCursor(5, 70);
    tft.print("Check Serial...");
    while (true) delay(1000);
  }

  tft.fillScreen(ST77XX_BLACK);
  Serial.println("Camera OK — streaming landscape!");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Frame grab failed");
    delay(50);
    return;
  }

  uint16_t *pixels = (uint16_t *)fb->buf;

  // Scale QVGA (320x240) down to TFT (160x128)
  for (int y = 0; y < TFT_H; y++) {
    int camY = (y * CAM_H) / TFT_H;  // scale 240 → 128
    for (int x = 0; x < TFT_W; x++) {
      int camX = (x * CAM_W) / TFT_W;  // scale 320 → 160
      uint16_t color = pixels[camY * CAM_W + camX];
      lineBuf[x] = (color >> 8) | (color << 8);  // byte swap
    }
    tft.drawRGBBitmap(0, y, lineBuf, TFT_W, 1);
  }

  esp_camera_fb_return(fb);

  // FPS counter
  static uint32_t t = 0, f = 0;
  f++;
  if (millis() - t >= 1000) {
    Serial.printf("FPS: %lu\n", f);
    f = 0;
    t = millis();
  }
}

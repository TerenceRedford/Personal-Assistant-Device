#include "esp_camera.h"
#include "img_converters.h"
#include <Arduino.h>
#include <SPI.h>
#include "driver/i2s.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "logo_boot_asset.h"

#define DBG_PRINTLN(...) do { } while (0)
#define DBG_PRINTF(...) do { } while (0)

// WiFi / laptop / audio wiring
// Set these locally before flashing.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define LAPTOP_IP "YOUR_LAPTOP_IP"
#define LAPTOP_PORT 5005

#define MIC_WS 12
#define MIC_SCK 15
#define MIC_SD 3
// Switch common is tied to an external supply rail; GPIO4 only senses the switched node.
#define SWITCH_PIN 4

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_I2S_READ_SAMPLES 256
#define AUDIO_SESSION_MS 30000
#define AUDIO_SESSION_PCM_BYTES (AUDIO_SAMPLE_RATE * 2 * AUDIO_SESSION_MS / 1000)
#define AUDIO_MAX_PACKET_PAYLOAD 65534
#define AUDIO_TCP_SEND_SLICE 2048
// Keep the I2S DMA ring modest so it fits in internal DMA-capable RAM on the ESP32-CAM.
#define AUDIO_I2S_DMA_BUF_LEN 256
#define AUDIO_I2S_DMA_BUF_COUNT 16
#define AUDIO_CAPTURE_BURST_READS 12
#define DISPLAY_FRAME_TIMEOUT_MS 120000
#define DISPLAY_FRAME_MAX_BYTES (320UL * 480UL * 2UL)

// LCD wiring
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_DC   2

class St7796Display {
public:
  void begin() {
    pinMode(TFT_DC, OUTPUT);
    digitalWrite(TFT_DC, HIGH);
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);

    delay(120);
    sendCommand(0x01);
    delay(120);
    sendCommand(0x11);
    delay(120);

    const uint8_t f0_1[] = {0xC3};
    const uint8_t f0_2[] = {0x96};
    const uint8_t madctl[] = {0x48};
    const uint8_t colmod[] = {0x55};
    const uint8_t invctl[] = {0x01};
    const uint8_t dispfn[] = {0x80, 0x02, 0x3B};
    const uint8_t outadj[] = {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33};
    const uint8_t pwr2[] = {0x06};
    const uint8_t pwr3[] = {0xA7};
    const uint8_t vcom[] = {0x18};
    const uint8_t gammaP[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B};
    const uint8_t gammaN[] = {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B};
    const uint8_t extOff1[] = {0x3C};
    const uint8_t extOff2[] = {0x69};

    sendCommand(0xF0, f0_1, sizeof(f0_1));
    sendCommand(0xF0, f0_2, sizeof(f0_2));
    sendCommand(0x36, madctl, sizeof(madctl));
    sendCommand(0x3A, colmod, sizeof(colmod));
    sendCommand(0xB4, invctl, sizeof(invctl));
    sendCommand(0xB6, dispfn, sizeof(dispfn));
    sendCommand(0xE8, outadj, sizeof(outadj));
    sendCommand(0xC1, pwr2, sizeof(pwr2));
    sendCommand(0xC2, pwr3, sizeof(pwr3));
    sendCommand(0xC5, vcom, sizeof(vcom));
    delay(120);
    sendCommand(0xE0, gammaP, sizeof(gammaP));
    sendCommand(0xE1, gammaN, sizeof(gammaN));
    delay(120);
    sendCommand(0xF0, extOff1, sizeof(extOff1));
    sendCommand(0xF0, extOff2, sizeof(extOff2));
    delay(120);
    sendCommand(0x29);
    delay(20);
  }

  int16_t width() const { return 320; }
  int16_t height() const { return 480; }

  void fillScreen(uint16_t color) {
    setAddrWindow(0, 0, 319, 479);
    writeRepeatedColor(color, 320UL * 480UL);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) {
      return;
    }
    setAddrWindow(x, y, x + w - 1, y + h - 1);
    writeRepeatedColor(color, (size_t)w * (size_t)h);
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    fillRect(x, y, 1, 1, color);
  }

  void drawChar(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale = 2) {
    const uint8_t *glyph = getGlyph(c);
    const int16_t charW = 5;
    const int16_t charH = 7;

    for (int16_t row = 0; row < charH; ++row) {
      uint8_t bits = glyph[row];
      for (int16_t col = 0; col < charW; ++col) {
        uint16_t px = (bits & (1 << (charW - 1 - col))) ? color : bg;
        fillRect(x + col * scale, y + row * scale, scale, scale, px);
      }
    }
  }

  void drawText(int16_t x, int16_t y, const char *text, uint16_t color, uint16_t bg, uint8_t scale = 2) {
    int16_t cursorX = x;
    while (*text) {
      if (*text == '\n') {
        cursorX = x;
        y += 8 * scale;
      } else {
        drawChar(cursorX, y, *text, color, bg, scale);
        cursorX += 6 * scale;
      }
      ++text;
    }
  }

  void drawRGB565Frame(const uint16_t *pixels, int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!pixels || w <= 0 || h <= 0) {
      return;
    }
    setAddrWindow(x, y, x + w - 1, y + h - 1);
    writePixels(pixels, (size_t)w * (size_t)h);
  }

private:
  SPISettings _settings = SPISettings(40000000, MSBFIRST, SPI_MODE0);

  void sendCommand(uint8_t cmd, const uint8_t *data = nullptr, size_t len = 0) {
    SPI.beginTransaction(_settings);
    digitalWrite(TFT_DC, LOW);
    SPI.transfer(cmd);
    if (len > 0 && data != nullptr) {
      digitalWrite(TFT_DC, HIGH);
      SPI.writeBytes(data, len);
    }
    SPI.endTransaction();
  }

  void setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t data[4];
    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;
    sendCommand(0x2A, data, sizeof(data));
    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;
    sendCommand(0x2B, data, sizeof(data));
    sendCommand(0x2C);
  }

  void writePixels(const uint16_t *pixels, size_t count) {
    SPI.beginTransaction(_settings);
    digitalWrite(TFT_DC, HIGH);
    uint8_t buffer[512];
    while (count > 0) {
      size_t chunkPixels = count < 256 ? count : 256;
      for (size_t i = 0; i < chunkPixels; ++i) {
        uint16_t c = pixels[i];
        buffer[i * 2] = c >> 8;
        buffer[i * 2 + 1] = c & 0xFF;
      }
      SPI.writeBytes(buffer, chunkPixels * 2);
      pixels += chunkPixels;
      count -= chunkPixels;
    }
    SPI.endTransaction();
  }

  void writeRepeatedColor(uint16_t color, size_t count) {
    SPI.beginTransaction(_settings);
    digitalWrite(TFT_DC, HIGH);
    uint8_t buffer[512];
    for (size_t i = 0; i < sizeof(buffer); i += 2) {
      buffer[i] = color >> 8;
      buffer[i + 1] = color & 0xFF;
    }
    while (count > 0) {
      size_t chunkPixels = count < 256 ? count : 256;
      SPI.writeBytes(buffer, chunkPixels * 2);
      count -= chunkPixels;
    }
    SPI.endTransaction();
  }

  const uint8_t *getGlyph(char c) const {
    static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t uppercaseA[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t uppercaseB[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static const uint8_t uppercaseC[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static const uint8_t uppercaseD[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t uppercaseE[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const uint8_t uppercaseF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t uppercaseG[7] = {0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0E};
    static const uint8_t uppercaseH[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t uppercaseI[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static const uint8_t uppercaseJ[7] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    static const uint8_t uppercaseK[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t uppercaseL[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static const uint8_t uppercaseM[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t uppercaseN[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t uppercaseO[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t uppercaseP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t uppercaseQ[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    static const uint8_t uppercaseR[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static const uint8_t uppercaseS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t uppercaseT[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t uppercaseU[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t uppercaseV[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static const uint8_t uppercaseW[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    static const uint8_t uppercaseY[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};

    static const uint8_t digit0[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t digit1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t digit2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t digit3[7] = {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E};
    static const uint8_t digit4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t digit5[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
    static const uint8_t digit6[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t digit7[7] = {0x1F,0x11,0x02,0x04,0x04,0x04,0x04};
    static const uint8_t digit8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t digit9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C};

    static const uint8_t colon_g[7] = {0x00,0x00,0x04,0x00,0x04,0x00,0x00};
    static const uint8_t dash_g[7]  = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t eq_g[7]    = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00};
    static const uint8_t dot_g[7]   = {0x00,0x00,0x00,0x00,0x00,0x04,0x00};

    switch (c) {
      case 'A': return uppercaseA;
      case 'B': return uppercaseB;
      case 'C': return uppercaseC;
      case 'D': return uppercaseD;
      case 'E': return uppercaseE;
      case 'F': return uppercaseF;
      case 'G': return uppercaseG;
      case 'H': return uppercaseH;
      case 'I': return uppercaseI;
      case 'J': return uppercaseJ;
      case 'K': return uppercaseK;
      case 'L': return uppercaseL;
      case 'M': return uppercaseM;
      case 'N': return uppercaseN;
      case 'O': return uppercaseO;
      case 'P': return uppercaseP;
      case 'Q': return uppercaseQ;
      case 'R': return uppercaseR;
      case 'S': return uppercaseS;
      case 'T': return uppercaseT;
      case 'U': return uppercaseU;
      case 'V': return uppercaseV;
      case 'W': return uppercaseW;
      case 'Y': return uppercaseY;
      case '0': return digit0;
      case '1': return digit1;
      case '2': return digit2;
      case '3': return digit3;
      case '4': return digit4;
      case '5': return digit5;
      case '6': return digit6;
      case '7': return digit7;
      case '8': return digit8;
      case '9': return digit9;
      case ':': return colon_g;
      case '-': return dash_g;
      case '=': return eq_g;
      case '.': return dot_g;
      case ' ': return space;
      default:  return space;
    }
  }
};

St7796Display display;

static bool wifiInitialized = false;
static bool wifiStarted = false;
static bool wifiConnected = false;
static int laptopSocket = -1;
static bool wifiWasConnected = false;
static bool tcpWasConnected = false;
static bool micWasRunning = false;
static bool switchWasHigh = false;
static bool micReady = false;
static uint8_t switchLowStreak = 0;
static uint8_t *audioSessionBuffer = nullptr;
static size_t audioSessionFill = 0;
static bool audioSessionCapHit = false;
static bool audioSessionLocked = false;
static bool displayHasReturnedImage = false;

static const uint8_t packetMagic[4] = {'A', 'U', 'D', '0'};
static const uint8_t packetStart = 1;
static const uint8_t packetEnd = 2;
static const uint8_t packetAudio = 3;
static const uint8_t imageMagic[4] = {'I', 'M', 'G', '0'};

bool ensureAudioSessionBuffer();
void resetAudioSessionBuffer();
bool flushAudioSession();
void appendPcmToSession(const uint8_t *pcm, size_t length);
bool readMicIntoSession(TickType_t timeoutMs);
bool recvAll(uint8_t *buffer, size_t length, uint32_t timeoutMs);
bool receiveDisplayFrame(uint32_t timeoutMs);
bool shouldSuppressStatusRedraw();

void closeLaptopSocket() {
  if (laptopSocket >= 0) {
    shutdown(laptopSocket, SHUT_RDWR);
    close(laptopSocket);
    laptopSocket = -1;
  }
}

bool shouldSuppressStatusRedraw() {
  return displayHasReturnedImage && !switchWasHigh;
}

void drawStartupLogo() {
  display.fillScreen(0x0000);

  int16_t x = 0;
  int16_t y = 0;
  for (uint32_t i = 0; i < LOGO_BOOT_RUN_COUNT; ++i) {
    const LogoBootRun &run = LOGO_BOOT_RUNS[i];
    if (run.color != 0x0000) {
      display.fillRect(x, y, (int16_t)run.count, 1, run.color);
    }

    x += (int16_t)run.count;
    while (x >= LOGO_BOOT_WIDTH) {
      x -= LOGO_BOOT_WIDTH;
      ++y;
    }
  }
}

void showRecordingStatus() {
  if (shouldSuppressStatusRedraw()) {
    return;
  }
  display.fillScreen(0x0000);
  display.drawText(6, 6, "RECORDING AUDIO...", 0xFFFF, 0x0000, 2);
  display.drawText(6, 34, "SPEAK NOW", 0xFFFF, 0x0000, 2);
}

void showLoadingStatus() {
  if (shouldSuppressStatusRedraw()) {
    return;
  }
  display.fillScreen(0xFFE0);
  display.drawText(6, 6, "BUILDING CALENDAR...", 0x0000, 0xFFE0, 2);
  display.drawText(6, 34, "PLEASE WAIT", 0x0000, 0xFFE0, 2);
}

void wifiEventHandler(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  (void)arg;
  (void)eventData;

  if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
    DBG_PRINTLN("WIFI: START");
    esp_wifi_connect();
    return;
  }

  if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
    wifiConnected = false;
    DBG_PRINTLN("WIFI: DISCONNECTED");
    closeLaptopSocket();
    return;
  }

  if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
    wifiConnected = true;
    DBG_PRINTLN("WIFI: GOT IP");
    return;
  }
}

bool startWifiStack() {
  if (wifiInitialized) {
    return true;
  }

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    DBG_PRINTF("NVS FAIL %d\n", (int)err);
    return false;
  }

  err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    DBG_PRINTF("NETIF FAIL %d\n", (int)err);
    return false;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    DBG_PRINTF("EVENT FAIL %d\n", (int)err);
    return false;
  }

  esp_netif_create_default_wifi_sta();

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&config);
  if (err != ESP_OK) {
    DBG_PRINTF("WIFI INIT FAIL %d\n", (int)err);
    return false;
  }

  esp_event_handler_instance_t wifiHandlerInstance;
  esp_event_handler_instance_t ipHandlerInstance;
  err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, &wifiHandlerInstance);
  if (err != ESP_OK) {
    DBG_PRINTF("WIFI EVT FAIL %d\n", (int)err);
    return false;
  }

  err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr, &ipHandlerInstance);
  if (err != ESP_OK) {
    DBG_PRINTF("IP EVT FAIL %d\n", (int)err);
    return false;
  }

  wifi_config_t wifiConfig = {};
  strlcpy(reinterpret_cast<char *>(wifiConfig.sta.ssid), WIFI_SSID, sizeof(wifiConfig.sta.ssid));
  strlcpy(reinterpret_cast<char *>(wifiConfig.sta.password), WIFI_PASSWORD, sizeof(wifiConfig.sta.password));
  wifiConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifiConfig.sta.pmf_cfg.capable = true;
  wifiConfig.sta.pmf_cfg.required = false;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    DBG_PRINTF("MODE FAIL %d\n", (int)err);
    return false;
  }

  err = esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
  if (err != ESP_OK) {
    DBG_PRINTF("CFG FAIL %d\n", (int)err);
    return false;
  }

  err = esp_wifi_start();
  if (err != ESP_OK) {
    DBG_PRINTF("START FAIL %d\n", (int)err);
    return false;
  }

  esp_wifi_set_ps(WIFI_PS_NONE);

  wifiInitialized = true;
  wifiStarted = true;
  esp_wifi_connect();
  return true;
}

void showStatus(const char *line1, const char *line2, uint16_t bg) {
  if (shouldSuppressStatusRedraw()) {
    return;
  }
  display.fillScreen(bg);
  display.drawText(6, 6, line1, 0xFFFF, bg, 2);
  if (line2 && *line2) {
    display.drawText(6, 34, line2, 0xFFFF, bg, 2);
  }
}

void showStatusSingle(const char *line, uint16_t bg) {
  showStatus(line, "", bg);
}

void setIdleStatus() {
  showStatus("AUDIO: IDLE", "SWITCH LOW", 0x001F);
}

void setWifiStatus(bool connected) {
  if (connected != wifiWasConnected) {
    wifiWasConnected = connected;
    if (connected) {
      DBG_PRINTLN("WIFI: CONNECTED");
    } else {
      DBG_PRINTLN("WIFI: DISCONNECTED");
    }
  }
}

void setTcpStatus(bool connected) {
  if (connected != tcpWasConnected) {
    tcpWasConnected = connected;
    if (connected) {
      DBG_PRINTLN("TCP: CONNECTED");
    } else {
      DBG_PRINTLN("TCP: DISCONNECTED");
    }
  }
}

void setMicStatus(bool running) {
  if (running != micWasRunning) {
    micWasRunning = running;
    if (running) {
      DBG_PRINTLN("AUDIO: STREAMING");
      showRecordingStatus();
    } else {
      DBG_PRINTLN("AUDIO: STOPPED");
    }
  }
}

bool sendAll(const uint8_t *buffer, size_t length) {
  if (laptopSocket < 0) {
    return false;
  }

  size_t offset = 0;
  while (offset < length) {
    size_t slice = length - offset;
    if (slice > AUDIO_TCP_SEND_SLICE) {
      slice = AUDIO_TCP_SEND_SLICE;
    }

    ssize_t written = send(laptopSocket, buffer + offset, slice, 0);
    if (written < 0) {
      closeLaptopSocket();
      return false;
    }
    if (written == 0) {
      closeLaptopSocket();
      return false;
    }
    offset += (size_t)written;
  }

  return true;
}

bool recvAll(uint8_t *buffer, size_t length, uint32_t timeoutMs) {
  if (laptopSocket < 0 || buffer == nullptr) {
    return false;
  }

  timeval timeout = {};
  timeout.tv_sec = (int)(timeoutMs / 1000);
  timeout.tv_usec = (int)((timeoutMs % 1000) * 1000);
  setsockopt(laptopSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  size_t offset = 0;
  while (offset < length) {
    ssize_t received = recv(laptopSocket, buffer + offset, length - offset, 0);
    if (received > 0) {
      offset += (size_t)received;
      continue;
    }
    if (received == 0) {
      closeLaptopSocket();
      return false;
    }
    return false;
  }

  return true;
}

bool receiveDisplayFrame(uint32_t timeoutMs) {
  if (laptopSocket < 0) {
    return false;
  }

  uint8_t header[12];
  if (!recvAll(header, sizeof(header), timeoutMs)) {
    return false;
  }

  if (memcmp(header, imageMagic, 4) != 0) {
    return false;
  }

  const uint16_t width = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
  const uint16_t height = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
  const uint32_t payloadLength =
      (uint32_t)header[8] |
      ((uint32_t)header[9] << 8) |
      ((uint32_t)header[10] << 16) |
      ((uint32_t)header[11] << 24);

  const uint32_t expectedLength = (uint32_t)width * (uint32_t)height * 2U;
  if (width == 0 || height == 0 || width > 320 || height > 480) {
    return false;
  }
  if (payloadLength == 0 || payloadLength != expectedLength || payloadLength > DISPLAY_FRAME_MAX_BYTES) {
    return false;
  }

  uint8_t *frameBuffer = static_cast<uint8_t *>(
      heap_caps_malloc(payloadLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (frameBuffer == nullptr) {
    frameBuffer = static_cast<uint8_t *>(malloc(payloadLength));
  }
  if (frameBuffer == nullptr) {
    return false;
  }

  bool gotPayload = recvAll(frameBuffer, payloadLength, timeoutMs);
  if (gotPayload) {
    display.drawRGB565Frame(reinterpret_cast<const uint16_t *>(frameBuffer), 0, 0, (int16_t)width, (int16_t)height);
  }

  free(frameBuffer);
  return gotPayload;
}

void sendPacket(uint8_t type, const uint8_t *payload, uint16_t length) {
  if (laptopSocket < 0) {
    return;
  }

  uint8_t header[8];
  header[0] = packetMagic[0];
  header[1] = packetMagic[1];
  header[2] = packetMagic[2];
  header[3] = packetMagic[3];
  header[4] = type;
  header[5] = 0;
  header[6] = (uint8_t)(length & 0xFF);
  header[7] = (uint8_t)(length >> 8);

  if (!sendAll(header, sizeof(header))) {
    return;
  }

  if (length > 0 && payload != nullptr) {
    sendAll(payload, length);
  }
}

bool ensureWifi() {
  if (!wifiInitialized && !startWifiStack()) {
    return false;
  }

  if (wifiConnected) {
    setWifiStatus(true);
    return true;
  }

  setWifiStatus(false);
  return false;
}

bool ensureLaptopConnection() {
  if (laptopSocket >= 0) {
    setTcpStatus(true);
    return true;
  }

  closeLaptopSocket();
  setTcpStatus(false);

  if (!wifiConnected) {
    showStatusSingle("TCP WAITING", 0x001F);
    return false;
  }

  showStatusSingle("TCP CONNECTING", 0x07E0);
  laptopSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (laptopSocket < 0) {
    showStatusSingle("TCP SOCKET FAIL", 0xF800);
    return false;
  }

  sockaddr_in destinationAddress = {};
  destinationAddress.sin_family = AF_INET;
  destinationAddress.sin_port = htons(LAPTOP_PORT);
  if (inet_pton(AF_INET, LAPTOP_IP, &destinationAddress.sin_addr) != 1) {
    showStatusSingle("TCP ADDR FAIL", 0xF800);
    closeLaptopSocket();
    return false;
  }

  if (connect(laptopSocket, reinterpret_cast<sockaddr *>(&destinationAddress), sizeof(destinationAddress)) != 0) {
    showStatusSingle("TCP CONNECT FAIL", 0xF800);
    closeLaptopSocket();
    return false;
  }

  int flag = 1;
  setsockopt(laptopSocket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  setTcpStatus(true);
  showStatusSingle("TCP CONNECTED", 0x07E0);
  return true;
}

bool startMic() {
  if (micReady) {
    return true;
  }

  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = AUDIO_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = AUDIO_I2S_DMA_BUF_COUNT;
  config.dma_buf_len = AUDIO_I2S_DMA_BUF_LEN;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = MIC_SCK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD;

  esp_err_t err = i2s_driver_install(AUDIO_I2S_PORT, &config, 0, nullptr);
  if (err != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "I2S INSTALL FAIL %d", (int)err);
    showStatusSingle(buf, 0xF800);
    DBG_PRINTLN(buf);
    return false;
  }

  err = i2s_set_pin(AUDIO_I2S_PORT, &pins);
  if (err != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "I2S PIN FAIL %d", (int)err);
    showStatusSingle(buf, 0xF800);
    DBG_PRINTLN(buf);
    i2s_driver_uninstall(AUDIO_I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(AUDIO_I2S_PORT);
  micReady = true;
  DBG_PRINTLN("I2S: READY");
  return true;
}

void stopMic() {
  if (!micReady) {
    return;
  }

  i2s_driver_uninstall(AUDIO_I2S_PORT);
  micReady = false;
}

void sendStartMarker() {
  sendPacket(packetStart, nullptr, 0);
  DBG_PRINTLN("UTTERANCE: START");
}

void sendEndMarker() {
  sendPacket(packetEnd, nullptr, 0);
  DBG_PRINTLN("UTTERANCE: END");
}

bool ensureAudioSessionBuffer() {
  if (audioSessionBuffer != nullptr) {
    return true;
  }

  if (audioSessionBuffer == nullptr) {
    audioSessionBuffer = static_cast<uint8_t *>(
        heap_caps_malloc(AUDIO_SESSION_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (audioSessionBuffer == nullptr) {
      audioSessionBuffer = static_cast<uint8_t *>(malloc(AUDIO_SESSION_PCM_BYTES));
    }
    if (audioSessionBuffer == nullptr) {
      showStatusSingle("AUDIO BUF FAIL", 0xF800);
      DBG_PRINTLN("AUDIO BUF FAIL");
      return false;
    }
  }

  audioSessionFill = 0;
  audioSessionCapHit = false;
  DBG_PRINTF("AUDIO: SESSION %u bytes\n", (unsigned)AUDIO_SESSION_PCM_BYTES);
  return true;
}

void resetAudioSessionBuffer() {
  audioSessionFill = 0;
  audioSessionCapHit = false;
}

bool flushAudioSession() {
  if (audioSessionFill == 0) {
    return true;
  }

  if (laptopSocket < 0) {
    audioSessionFill = 0;
    return false;
  }

  size_t offset = 0;
  while (offset < audioSessionFill) {
    size_t toSend = audioSessionFill - offset;
    if (toSend > AUDIO_MAX_PACKET_PAYLOAD) {
      toSend = AUDIO_MAX_PACKET_PAYLOAD;
    }

    sendPacket(packetAudio, audioSessionBuffer + offset, (uint16_t)toSend);
    if (laptopSocket < 0) {
      audioSessionFill = 0;
      return false;
    }
    offset += toSend;
  }

  audioSessionFill = 0;
  return true;
}

void appendPcmToSession(const uint8_t *pcm, size_t length) {
  if (audioSessionBuffer == nullptr || pcm == nullptr || length == 0 || audioSessionCapHit) {
    return;
  }

  while (length > 0) {
    if (audioSessionFill >= AUDIO_SESSION_PCM_BYTES) {
      audioSessionCapHit = true;
      return;
    }

    size_t space = AUDIO_SESSION_PCM_BYTES - audioSessionFill;
    size_t copyLen = length < space ? length : space;
    memcpy(audioSessionBuffer + audioSessionFill, pcm, copyLen);
    audioSessionFill += copyLen;
    pcm += copyLen;
    length -= copyLen;

    if (audioSessionFill >= AUDIO_SESSION_PCM_BYTES) {
      audioSessionCapHit = true;
      return;
    }
  }
}

bool readMicIntoSession(TickType_t timeoutMs) {
  if (!micReady) {
    return false;
  }

  int32_t i2sBuffer[AUDIO_I2S_READ_SAMPLES];
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(AUDIO_I2S_PORT, i2sBuffer, sizeof(i2sBuffer), &bytesRead, pdMS_TO_TICKS(timeoutMs));
  if (err != ESP_OK || bytesRead == 0) {
    return false;
  }

  const size_t samplesRead = bytesRead / sizeof(int32_t);
  static uint8_t pcmBuffer[AUDIO_I2S_READ_SAMPLES * 2];
  size_t outIndex = 0;
  for (size_t i = 0; i < samplesRead; ++i) {
    const int16_t sample16 = (int16_t)(i2sBuffer[i] >> 16);
    pcmBuffer[outIndex++] = (uint8_t)(sample16 & 0xFF);
    pcmBuffer[outIndex++] = (uint8_t)((sample16 >> 8) & 0xFF);
  }

  appendPcmToSession(pcmBuffer, outIndex);
  return true;
}

void setupCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.sccb_i2c_port = 0;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 10;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  display.fillScreen(0x001F);
  display.drawText(6, 6, "CAM INIT: JPEG QVGA", 0xFFFF, 0x001F, 2);

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    char ebuf[64];
    snprintf(ebuf, sizeof(ebuf), "CAM INIT FAIL %d", (int)err);
    display.drawText(6, 80, ebuf, 0xFFFF, 0x001F, 2);
    while (true) {
      delay(1000);
    }
  }

  // Keep the sensor configuration aligned with a JPEG capture workflow.
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, -2);
    }
  }

  display.fillScreen(0x07E0);
  display.drawText(10, 10, "CAMERA INIT OK", 0x0000, 0x07E0, 2);
}

void setup() {
  delay(500);

  pinMode(SWITCH_PIN, INPUT_PULLDOWN);

  display.begin();
  drawStartupLogo();

  if (!startWifiStack()) {
    DBG_PRINTLN("WIFI INIT FAIL");
  }
}

void loop() {
  bool drewReturnedFrame = false;
  const bool rawSwitchHigh = digitalRead(SWITCH_PIN) == HIGH;
  if (rawSwitchHigh) {
    switchLowStreak = 0;
  } else if (switchLowStreak < 5) {
    ++switchLowStreak;
  }

  const bool switchHigh = rawSwitchHigh || switchLowStreak < 3;

  if (switchHigh != switchWasHigh) {
    switchWasHigh = switchHigh;
    if (switchHigh) {
      displayHasReturnedImage = false;
    } else {
    }
  }

  if (!switchHigh) {
    if (micWasRunning) {
      if (!flushAudioSession()) {
        DBG_PRINTLN("AUDIO: FINAL FLUSH FAIL");
      }
      sendEndMarker();
      showLoadingStatus();
      drewReturnedFrame = receiveDisplayFrame(DISPLAY_FRAME_TIMEOUT_MS);
      if (drewReturnedFrame) {
        displayHasReturnedImage = true;
      }
      setMicStatus(false);
    }
    resetAudioSessionBuffer();
    audioSessionLocked = false;
    stopMic();
    if (laptopSocket >= 0) {
      closeLaptopSocket();
      setTcpStatus(false);
    }
    delay(100);
    return;
  }

  if (audioSessionLocked) {
    delay(50);
    return;
  }

  if (!ensureWifi()) {
    delay(500);
    return;
  }

  if (!ensureLaptopConnection()) {
    delay(250);
    return;
  }

  if (!startMic()) {
    delay(250);
    return;
  }

  if (!ensureAudioSessionBuffer()) {
    delay(250);
    return;
  }

  if (!micWasRunning) {
    resetAudioSessionBuffer();
    sendStartMarker();
    setMicStatus(true);
  }

  bool capturedAudio = false;
  for (int burst = 0; burst < AUDIO_CAPTURE_BURST_READS; ++burst) {
    if (!readMicIntoSession(0)) {
      break;
    }
    capturedAudio = true;
  }
  if (!capturedAudio) {
    readMicIntoSession(2);
  }

  if (audioSessionCapHit) {
    if (!flushAudioSession()) {
      DBG_PRINTLN("AUDIO: SESSION FLUSH FAIL");
    }
    sendEndMarker();
    showLoadingStatus();
    drewReturnedFrame = receiveDisplayFrame(DISPLAY_FRAME_TIMEOUT_MS);
    if (drewReturnedFrame) {
      displayHasReturnedImage = true;
    }
    setMicStatus(false);
    stopMic();
    audioSessionLocked = true;
    DBG_PRINTLN("AUDIO: 30S CAP");
    delay(100);
    return;
  }

  if (laptopSocket < 0) {
    setTcpStatus(false);
    setMicStatus(false);
    resetAudioSessionBuffer();
    sendEndMarker();
    stopMic();
  }
}

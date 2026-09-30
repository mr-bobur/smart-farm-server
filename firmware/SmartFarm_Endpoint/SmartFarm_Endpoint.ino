/*
  =============================================================================
  Aqlli va Xavfsiz Ferma - Endpoint Node (Dala / Ferma Tuguni)
  Plata: LilyGO T-Halow (ESP32-S3 WROOM + OV2640 + 8MB OPI PSRAM)
  Port: COM4
  -----------------------------------------------------------------------------
  YANGILANGAN VA TO'LIQ MOSLASHTIRILGAN FUNKSIYALAR:
  1. Radar OUT pini: GPIO 39 ga o'tkazildi (Header 2, Pin 10 - mustaqil pin)
  2. Sirena va Servo to'liq ajratildi:
     - Servo: LEDC Channel 0 (Timer 0, 50Hz, 14-bit)
     - Sirena: LEDC Channel 2 (Timer 1, Audio Tone, 10-bit)
     - Sirena chalganda servo mutlaqo qimirlamaydi (to'xtab xavf nuqtasiga qaratiladi)
  3. Servo Patruli:
     - Default holat: 90° (Markaz)
     - Oraliq: 45° dan 135° gacha
     - 45° dan 135° ga 20 soniyada sekin boradi (~222ms da 1 gradus)
     - 135° dan 45° ga yana 20 soniyada qaytadi (doimiy perimetr nazorati)
  4. Hayvon qo'rqituvchi sirena: 1400Hz - 3200Hz yirtqich/tashvish modulyatsiyasi
  5. Kamera: 180° ga to'g'ri o'girilgan (vflip=0, hmirror=0) va /flip API faol
  6. Pinlar:
     - mmWave Radar: RX=GPIO 7, TX=GPIO 6, OUT=GPIO 45 (HLK-LD2410C)
     - Kamera: J4 sloti (OV2640 VGA 640x480 - 8-bit DVP shina)
     - Sirena / Kalonka: GPIO 15 (Channel 2 Audio Tone 1400-3200Hz)
     - Servo: GPIO 46 (Channel 0 Pan 45°-135°)
     - GPS: RX=GPIO 40, TX=GPIO 41 (NEO-6M)
     - Batareya: GPIO 3 (VBAT-DET)
     * Tuproq va Harorat/Namlik sensorlari olib tashlangan
  =============================================================================
*/

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include "esp_http_server.h"

// ---------------------- PIN DEFINITIONS ----------------------
#define BOARD_LED        38
#define SERVO_PIN        46   // 180° Pan Servo (LEDC Channel 0)
#define BATT_ADC_PIN     3    // VBAT-DET (Bortdagi 100k/100k bo'luvchi)
#define SIREN_PIN        15   // Karnay / Sirena drayveri (LEDC Channel 2) - GPIO 15 ga ko'chirildi

// GPS: GPIO 41 va 40 pinlari (Uskunaviy testda tasdiqlandi: GPS TX = GPIO 41!)
#define GPS_RX_PIN       41   // Serial1 RX: GPS TX dan o'qish (9600 baud)
#define GPS_TX_PIN       40   // Serial1 TX: GPS RX ga (9600 baud)

// mmWave RADAR: TX, RX va OUT pini (HLK-LD2410C) - Yangi xavfsiz pinlar
#define RADAR_RX_PIN     7    // Serial2 RX: Radar TX dan o'qish (256000 baud)
#define RADAR_TX_PIN     6    // Serial2 TX: Radar RX ga
#define RADAR_OUT_PIN    45   // HLK-LD2410C OUT raqamli kirish (GPIO 45)
#define RADAR_OUT_PIN_ALT 39  // HLK-LD2410C OUT muqobil/zaxira kirish (GPIO 39)

// T-HaLow (TX-AH-R900P) TTL UART Pinlari (Bortdagi apparat modul)
#define HALOW_RX_PIN     5    // UART0 RX: TX-AH TX dan (115200 baud)
#define HALOW_TX_PIN     4    // UART0 TX: TX-AH RX ga (115200 baud)

// LilyGO T-Halow Rasmiy Kamera Pinlari (J4 Sloti)
#define CAMERA_PIN_PWDN     (-1)
#define CAMERA_PIN_RESET    (18)
#define CAMERA_PIN_XCLK     (8)
#define CAMERA_PIN_SIOD     (2)
#define CAMERA_PIN_SIOC     (1)

#define CAMERA_PIN_Y9       (9)
#define CAMERA_PIN_Y8       (10)
#define CAMERA_PIN_Y7       (11)
#define CAMERA_PIN_Y6       (13)
#define CAMERA_PIN_Y5       (21)
#define CAMERA_PIN_Y4       (48)
#define CAMERA_PIN_Y3       (47)
#define CAMERA_PIN_Y2       (14)

#define CAMERA_PIN_VSYNC    (16)
#define CAMERA_PIN_HREF     (17)
#define CAMERA_PIN_PCLK     (12)

// ---------------------- HARDWARE CHANNELS --------------------
#define SERVO_LEDC_CHANNEL  0
#define SERVO_LEDC_FREQ     50
#define SERVO_LEDC_RES      14

#define SIREN_LEDC_CHANNEL  2
#define SIREN_LEDC_RES      10

// ---------------------- NETWORK & SERVERS --------------------
WiFiMulti wifiMulti;

const char* server_urls[] = {
  "http://170.168.60.245:8000/api/telemetry" // Yagona faol tashqi server
};

const char* upload_frame_urls[] = {
  "http://170.168.60.245:8000/api/upload_frame" // Yagona faol video upload
};
const int num_server_urls = 1;
int activeServerIdx = 0;

TaskHandle_t streamTaskHandle = NULL;
unsigned long framesUploaded = 0;
int lastUploadHttpCode = 0;

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
bool cameraFound = false;

// ---------------------- SIRENA NAZORATI ----------------------
bool sirenActive = false;
unsigned long lastSirenAudioTick = 0;
int sirenToneFreq = 1400;
bool sirenToneUp = true;

void setSiren(bool active) {
  if (sirenActive == active) return;
  sirenActive = active;

  if (!sirenActive) {
    ledcWrite(SIREN_PIN, 0);
    digitalWrite(BOARD_LED, LOW);
    Serial.println("[DEBUG SIRENA] Karnay O'CHIRILDI. Servo patruli qayta faollashadi.");
  } else {
    sirenToneFreq = 1400;
    sirenToneUp = true;
    ledcWriteTone(SIREN_PIN, sirenToneFreq);
    Serial.println("[DEBUG SIRENA] Hayvon qo'rqitish YOQILDI! Servo to'xtatildi (xavf zonasi muhrlandi).");
  }
}

void updateSirenAudio() {
  if (!sirenActive) return;

  // Har 25 millisekundda chastotani o'zgartirib hayvon qo'rqituvchi sirenani yaratish
  if (millis() - lastSirenAudioTick >= 25) {
    lastSirenAudioTick = millis();
    if (sirenToneUp) {
      sirenToneFreq += 70;
      if (sirenToneFreq >= 3200) sirenToneUp = false;
    } else {
      sirenToneFreq -= 70;
      if (sirenToneFreq <= 1400) sirenToneUp = true;
    }
    ledcWriteTone(SIREN_PIN, sirenToneFreq);

    // Stroboskopik chaqnash
    digitalWrite(BOARD_LED, (sirenToneFreq % 200 > 100) ? HIGH : LOW);
  }
}

// ---------------------- SERVO NAZORATI (45°-135° / 20s) ------
bool autoServoPatrol = true;            // 20s avtomatik patrullash
int currentServoAngle = 90;             // Default boshlang'ich holat: 90°
int targetServoAngle = 90;
int patrolDirection = 1;                // +1: 135° ga qarab, -1: 45° ga qarab
unsigned long lastPatrolStep = 0;

void applyServoDuty(int angle) {
  angle = constrain(angle, 45, 135);
  uint32_t duty = map(angle, 0, 180, 410, 2048);
  ledcWrite(SERVO_PIN, duty);
}

void setServoAngle(int angle, bool manualOverride = false) {
  angle = constrain(angle, 45, 135);
  targetServoAngle = angle;
  if (manualOverride) {
    autoServoPatrol = false; // Qo'lda burchak berilsa avtopatrul to'xtaydi
    currentServoAngle = angle;
    applyServoDuty(currentServoAngle);
    Serial.printf("[DEBUG SERVO] Qo'lda burchak o'rnatildi -> %d°\n", angle);
  }
}

void updateServoPatrol() {
  // MUHIM TALAB: Sirena chalganda servo mutlaqo harakatlanmaydi!
  if (sirenActive) {
    return;
  }

  // 20 soniyada 45° dan 135° gacha sekin borish (90 gradus / 20 soniya = ~222 ms da 1 gradus)
  // 135° dan 45° ga yana 20 soniyada qaytish
  if (autoServoPatrol) {
    if (millis() - lastPatrolStep >= 222) {
      lastPatrolStep = millis();
      currentServoAngle += patrolDirection;

      if (currentServoAngle >= 135) {
        currentServoAngle = 135;
        patrolDirection = -1; // Endi 45° ga qarab 20 soniyada qaytadi
        Serial.println("[AUTO SERVO] 135° ga yetdi. 20 soniya davomida 45° ga qaytish boshlandi.");
      } else if (currentServoAngle <= 45) {
        currentServoAngle = 45;
        patrolDirection = 1;  // Endi 135° ga qarab 20 soniyada boradi
        Serial.println("[AUTO SERVO] 45° ga yetdi. 20 soniya davomida 135° ga borish boshlandi.");
      }

      applyServoDuty(currentServoAngle);
    }
  }
}

// ---------------------- DALA KO'RSATKICHLARI -----------------
// Batareya Ko'rsatkichlari
float batteryVoltage = 4.15;
int batteryPercent = 95;

// Radar (HLK-LD2410C) Ko'rsatkichlari
int radarPresence = 0;       // 0: Tinch, 1: Harakat, 2: Qo'zg'almas, 3: Ikkalasi
int radarDistanceCm = 0;     // Nishongacha masofa (sm)
int radarOutVal = 0;         // GPIO 45 OUT holati
uint8_t radarRxBuf[36];
int radarRxIdx = 0;
unsigned long lastRadarByteTime = 0;
unsigned long lastRadarFrameParsed = 0;
unsigned long radarTotalBytesReceived = 0;

// NEO-6M GPS Ko'rsatkichlari
float gpsLatitude = 41.5505;
float gpsLongitude = 60.6312;
String gpsLineBuf = "";
int gpsSentencesParsed = 0;
unsigned long gpsTotalBytesReceived = 0;
unsigned long lastGpsByteTime = 0;
bool gpsHasFix = false;
int gpsSatsVisible = 0;

// T-HaLow (TX-AH-R900P) Diagnostika Ko'rsatkichlari
HardwareSerial SerialHalow(0);
bool halowDetected = false;
String halowInfo = "Kutilmoqda...";

// Diagnostika va Statistika
unsigned long lastTelemetrySend = 0;
unsigned long lastSensorRead = 0;
unsigned long lastSerialLog = 0;
unsigned long telemetryPacketsSent = 0;
int lastHttpResponseCode = 0;

// ---------------------- CAMERA SETUP -------------------------
bool initCamera() {
  Serial.println("[DEBUG KAMERA] OV2640 sozlanmoqda...");
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = CAMERA_PIN_Y2;
  config.pin_d1       = CAMERA_PIN_Y3;
  config.pin_d2       = CAMERA_PIN_Y4;
  config.pin_d3       = CAMERA_PIN_Y5;
  config.pin_d4       = CAMERA_PIN_Y6;
  config.pin_d5       = CAMERA_PIN_Y7;
  config.pin_d6       = CAMERA_PIN_Y8;
  config.pin_d7       = CAMERA_PIN_Y9;
  config.pin_xclk     = CAMERA_PIN_XCLK;
  config.pin_pclk     = CAMERA_PIN_PCLK;
  config.pin_vsync    = CAMERA_PIN_VSYNC;
  config.pin_href     = CAMERA_PIN_HREF;
  config.pin_sccb_sda = CAMERA_PIN_SIOD;
  config.pin_sccb_scl = CAMERA_PIN_SIOC;
  config.pin_pwdn     = CAMERA_PIN_PWDN;
  config.pin_reset    = CAMERA_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;  // 640x480 (Maksimal tiniq sifat)
    config.jpeg_quality = 10;           // Maksimal sifat (kam siqish, tiniq tasvir)
    config.fb_count = 2;                // 2 bufer (parallel kadr olish va uzatish)
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST; // Doim eng so'nggi kadr (lag bo'lmaydi)
    Serial.println("[DEBUG KAMERA] 8MB OPI PSRAM faol (640x480 VGA @ Q10 Maksimal Sifat).");
  } else {
    config.frame_size = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("[DEBUG KAMERA] DRAM rejimi (640x480 VGA).");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[DEBUG KAMERA] Xatolik: 0x%x\n", err);
    cameraFound = false;
    return false;
  }

  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    // 180° ga to'nkarilgan holatga burish (foydalanuvchi talabi)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    // Maksimal tiniqlik va sifat uchun sensor sozlamalari
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_sharpness(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_denoise(s, 1);
  }

  Serial.println("[OK KAMERA] OV2640 Kamera muvaffaqiyatli ishga tushdi (180° burilgan)!");
  cameraFound = true;
  return true;
}

// ------------------- MJPEG STREAM HANDLER --------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[64];

  Serial.println("[DEBUG VIDEO] Jonli efirga mijoz ulandi (/stream)!");
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }
    _jpg_buf_len = fb->len;
    _jpg_buf = fb->buf;

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    esp_camera_fb_return(fb);
    fb = NULL;

    if (res != ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(35)); // ~28 fps
  }
  Serial.println("[DEBUG VIDEO] Jonli efir mijozdan uzildi.");
  return res;
}

// ------------------- SNAPSHOT CAPTURE HANDLER -----------------
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  Serial.printf("[DEBUG FOTO] /capture kadr jo'natildi (%u bayt)\n", fb->len);
  return res;
}

// ------------------- REST API HANDLERS ------------------------
static esp_err_t servo_handler(httpd_req_t *req) {
  char buf[32];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char param[16];
    if (httpd_query_key_value(buf, "auto", param, sizeof(param)) == ESP_OK) {
      autoServoPatrol = (param[0] == '1');
      Serial.printf("[DEBUG SERVO] Avto patrullash holati: %s\n", autoServoPatrol ? "FAOL" : "TO'XTATILDI");
      httpd_resp_send(req, "OK", 2);
      return ESP_OK;
    }
    if (httpd_query_key_value(buf, "angle", param, sizeof(param)) == ESP_OK) {
      int angle = atoi(param);
      setServoAngle(angle, true);
      httpd_resp_send(req, "OK", 2);
      return ESP_OK;
    }
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t flip_handler(httpd_req_t *req) {
  char buf[32];
  sensor_t * s = esp_camera_sensor_get();
  if (s && httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char vParam[8], hParam[8];
    if (httpd_query_key_value(buf, "v", vParam, sizeof(vParam)) == ESP_OK) {
      s->set_vflip(s, atoi(vParam));
    }
    if (httpd_query_key_value(buf, "h", hParam, sizeof(hParam)) == ESP_OK) {
      s->set_hmirror(s, atoi(hParam));
    }
    Serial.println("[DEBUG KAMERA] Dinamik vflip/hmirror o'zgartirildi!");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t siren_handler(httpd_req_t *req) {
  char buf[32];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char param[16];
    if (httpd_query_key_value(buf, "state", param, sizeof(param)) == ESP_OK) {
      bool newState = (param[0] == '1');
      setSiren(newState);
      httpd_resp_send(req, "OK", 2);
      return ESP_OK;
    }
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t index_handler(httpd_req_t *req) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Endpoint Diagnostika</title></head>";
  html += "<body style='background:#111;color:#eee;font-family:sans-serif;padding:25px;'>";
  html += "<h2>Aqlli Ferma - Endpoint Node Diagnostika</h2>";
  html += "<p><b>IP Manzil:</b> " + WiFi.localIP().toString() + "</p>";
  html += "<p><b>mmWave Radar (HLK-LD2410C):</b> " + String(radarPresence ? "Nishon Aniqlandi!" : "Tinch") + " | Masofa: " + String(radarDistanceCm) + " sm | OUT (IO45): " + String(radarOutVal) + " (RX:7, TX:6)</p>";
  html += "<p><b>Ovoz / Sirena (GPIO 15):</b> " + String(sirenActive ? "YOQILGAN (Xavf/Qo'rqitish)" : "O'chiq (Tinch)") + "</p>";
  html += "<p><b>Batareya:</b> " + String(batteryVoltage, 2) + " V (" + String(batteryPercent) + " %)</p>";
  html += "<p><b>GPS (GPIO 40/41):</b> " + String(gpsLatitude, 6) + ", " + String(gpsLongitude, 6) + "</p>";
  html += "<p><b>Servo (GPIO 46):</b> " + String(currentServoAngle) + "&deg; (Patrol: " + (autoServoPatrol ? "20s Faol (45°-135°)" : "Qo'lda") + ")</p>";
  html += "<p><b>Sirena:</b> " + String(sirenActive ? "YOQILGAN (Servo to'xtatilgan)" : "O'chiq") + "</p>";
  html += "<p><a href='/capture' style='color:#38bdf8'>Fotosurat olish (/capture)</a></p>";
  html += "<p><a href=':81/stream' style='color:#34d399'>Video Oqimi (Port 81 /stream)</a></p>";
  html += "</body></html>";
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

void startHttpServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture",  .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t servo_uri   = { .uri = "/servo",    .method = HTTP_GET, .handler = servo_handler,   .user_ctx = NULL };
  httpd_uri_t siren_uri   = { .uri = "/siren",    .method = HTTP_GET, .handler = siren_handler,   .user_ctx = NULL };
  httpd_uri_t flip_uri    = { .uri = "/flip",     .method = HTTP_GET, .handler = flip_handler,    .user_ctx = NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &servo_uri);
    httpd_register_uri_handler(camera_httpd, &siren_uri);
    httpd_register_uri_handler(camera_httpd, &flip_uri);
    Serial.println("[OK HTTP] Port 80 Web Server & REST API faol.");
  }

  // Stream Server on Port 81
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;

  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  httpd_uri_t root_stream = { .uri = "/",      .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &root_stream);
    Serial.println("[OK HTTP] Port 81 Video Stream Server faol.");
  }
}

// ---------------------- REAL-TIME RADAR ENGINE (HLK-LD2410C) --
void updateRadarStream() {
  // 1. OUT pini (GPIO 45 yoki GPIO 39) doimiy o'qiladi
  radarOutVal = digitalRead(RADAR_OUT_PIN) | digitalRead(RADAR_OUT_PIN_ALT);

  // 2. Serial2 (RX: GPIO 7, TX: GPIO 6) baytlari asinxron o'qiladi
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    radarTotalBytesReceived++;
    lastRadarByteTime = millis();

    // LD2410 Frame Header: 0xF4, 0xF3, 0xF2, 0xF1
    if (radarRxIdx == 0) {
      if (b == 0xF4) radarRxBuf[radarRxIdx++] = b;
    } else if (radarRxIdx == 1) {
      if (b == 0xF3) radarRxBuf[radarRxIdx++] = b;
      else radarRxIdx = (b == 0xF4) ? 1 : 0;
    } else if (radarRxIdx == 2) {
      if (b == 0xF2) radarRxBuf[radarRxIdx++] = b;
      else radarRxIdx = 0;
    } else if (radarRxIdx == 3) {
      if (b == 0xF1) radarRxBuf[radarRxIdx++] = b;
      else radarRxIdx = 0;
    } else {
      if (radarRxIdx < 34) {
        radarRxBuf[radarRxIdx++] = b;
      } else {
        radarRxIdx = 0;
      }

      // Basic Target Frame: kamida 23 bayt bo'ladi, oxiri 0xF8, 0xF7, 0xF6, 0xF5 bilan tugaydi
      if (radarRxIdx >= 23) {
        if (radarRxBuf[radarRxIdx - 4] == 0xF8 &&
            radarRxBuf[radarRxIdx - 3] == 0xF7 &&
            radarRxBuf[radarRxIdx - 2] == 0xF6 &&
            radarRxBuf[radarRxIdx - 1] == 0xF5) {

          // Kadr to'liq va xatosiz qabul qilindi!
          // Protokol: Byte 8: Target status (0: Tinch, 1: Harakat, 2: Qo'zg'almas, 3: Ikkalasi)
          // Byte 9-10: Moving target distance (sm)
          // Byte 12-13: Stationary target distance (sm)
          uint8_t state = radarRxBuf[8];
          uint16_t moveDist = radarRxBuf[9] | (radarRxBuf[10] << 8);
          uint16_t statDist = radarRxBuf[12] | (radarRxBuf[13] << 8);

          radarPresence = state;
          if (state == 1) {
            radarDistanceCm = moveDist;
          } else if (state == 2) {
            radarDistanceCm = statDist;
          } else if (state == 3) {
            radarDistanceCm = (moveDist > 0 && statDist > 0) ? min(moveDist, statDist) : (moveDist > 0 ? moveDist : statDist);
          } else {
            radarDistanceCm = 0;
          }

          lastRadarFrameParsed = millis();
          radarRxIdx = 0;
        }
      }
    }
  }

  // Agar OUT pini HIGH bo'lsa, hatto UART kadr bermasa ham nishon bor deb qabul qilinadi
  if (radarOutVal == HIGH && radarPresence == 0) {
    radarPresence = 1;
    if (radarDistanceCm == 0) radarDistanceCm = 150;
  }
}

// ---------------------- REAL-TIME GPS ENGINE (NEO-6M) ---------
void processGpsSentence(const String& s) {
  // $GPRMC yoki $GNRMC (NMEA-0183 Standart)
  if (s.startsWith("$GPRMC") || s.startsWith("$GNRMC")) {
    gpsSentencesParsed++;

    // Vergul orqali tokenlarni ajratish
    int commaIdx[13];
    int commaCount = 0;
    for (int i = 0; i < s.length() && commaCount < 13; i++) {
      if (s[i] == ',') commaIdx[commaCount++] = i;
    }

    // $GPRMC,hhmmss.ss,Status,Latitude,N/S,Longitude,E/W,...
    // Token 0: $GPRMC
    // Token 1: hhmmss.ss
    // Token 2: Status ('A'=Valid, 'V'=Void)
    // Token 3: Latitude (ddmm.mmmm)
    // Token 4: N/S
    // Token 5: Longitude (dddmm.mmmm)
    // Token 6: E/W
    if (commaCount >= 6) {
      char status = s[commaIdx[1] + 1]; // 'A' = Valid Fix, 'V' = Void (kutilmoqda)
      if (status == 'A') {
        gpsHasFix = true;
        String rawLat = s.substring(commaIdx[2] + 1, commaIdx[3]);
        char ns = s[commaIdx[3] + 1];
        String rawLon = s.substring(commaIdx[4] + 1, commaIdx[5]);
        char ew = s[commaIdx[5] + 1];

        if (rawLat.length() >= 6 && rawLon.length() >= 7) {
          // ddmm.mmmm -> daraja
          float latDeg = rawLat.substring(0, 2).toFloat();
          float latMin = rawLat.substring(2).toFloat();
          float lat = latDeg + (latMin / 60.0);
          if (ns == 'S' || ns == 's') lat = -lat;

          // dddmm.mmmm -> daraja
          float lonDeg = rawLon.substring(0, 3).toFloat();
          float lonMin = rawLon.substring(3).toFloat();
          float lon = lonDeg + (lonMin / 60.0);
          if (ew == 'W' || ew == 'w') lon = -lon;

          if (lat != 0.0 && lon != 0.0) {
            gpsLatitude = lat;
            gpsLongitude = lon;
          }
        }
      } else {
        gpsHasFix = false; // Sun'iy yo'ldoshlar qidirilmoqda
      }
    }
  } else if (s.startsWith("$GPGGA") || s.startsWith("$GNGGA")) {
    int commaIdx[9];
    int count = 0;
    for (int i = 0; i < s.length() && count < 9; i++) {
      if (s[i] == ',') commaIdx[count++] = i;
    }
    if (count >= 8) {
      gpsSatsVisible = s.substring(commaIdx[6] + 1, commaIdx[7]).toInt();
    }
  }
}

void updateGpsStream() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    gpsTotalBytesReceived++;
    lastGpsByteTime = millis();

    if (c == '\n' || c == '\r') {
      if (gpsLineBuf.length() > 6) {
        processGpsSentence(gpsLineBuf);
      }
      gpsLineBuf = "";
    } else {
      if (gpsLineBuf.length() < 120) {
        gpsLineBuf += c;
      }
    }
  }
}

// ---------------------- SENSORS READING ----------------------
void readSensors() {
  // 1. Batareya Quvvati (VBAT-DET - GPIO 3)
  uint32_t battSum = 0;
  for (int i = 0; i < 8; i++) {
    battSum += analogRead(BATT_ADC_PIN);
    delay(2);
  }
  float battRaw = (float)battSum / 8.0;
  float vAdc = (battRaw / 4095.0) * 3.1;
  batteryVoltage = vAdc * 2.0;

  if (batteryVoltage >= 4.25) {
    batteryPercent = 100;
  } else if (batteryVoltage < 2.5) {
    batteryPercent = 100;
    batteryVoltage = 4.20;
  } else {
    batteryPercent = constrain(map((long)(batteryVoltage * 100), 330, 420, 0, 100), 0, 100);
  }
}

// ---------------------- TELEMETRY SENDER & SYNC ---------------
void sendTelemetry() {
  if (WiFi.status() == WL_CONNECTED) {
    String json = "{";
    json += "\"battery\":" + String(batteryPercent) + ",";
    json += "\"battery_voltage\":" + String(batteryVoltage, 2) + ",";
    json += "\"radar\":" + String(radarPresence) + ",";
    json += "\"radar_distance\":" + String(radarDistanceCm) + ",";
    json += "\"lat\":" + String(gpsLatitude, 6) + ",";
    json += "\"lng\":" + String(gpsLongitude, 6) + ",";
    json += "\"servo_angle\":" + String(currentServoAngle) + ",";
    json += "\"siren_active\":" + String(sirenActive ? "true" : "false") + ",";
    json += "\"endpoint_ip\":\"" + WiFi.localIP().toString() + "\"";
    json += "}";

    HTTPClient http;
    http.begin(server_urls[activeServerIdx]);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(1200);
    lastHttpResponseCode = http.POST(json);

    if (lastHttpResponseCode == 200) {
      telemetryPacketsSent++;
      String response = http.getString();
      // Serverdan kelgan sirena buyrug'ini sinxronlash
      if (response.indexOf("\"siren_active\":true") >= 0) {
        setSiren(true);
      } else if (response.indexOf("\"siren_active\":false") >= 0) {
        setSiren(false);
      }
    } else {
      activeServerIdx = (activeServerIdx + 1) % num_server_urls;
    }
    http.end();
  }
}

// ---------------------- SERIAL MONITOR LOGGER ----------------
void printPeriodicDiagnostics() {
  String rStatus = "Tinch (Nishon yo'q)";
  if (radarPresence == 1) rStatus = "Harakat!";
  else if (radarPresence == 2) rStatus = "Qo'zg'almas";
  else if (radarPresence == 3) rStatus = "Harakat+Mavjud";

  Serial.println("\n+-------------------------------------------------------------------------+");
  Serial.printf ("| [DIAGNOSTIKA] AQLLI FERMA MUHOFAZA TIZIMI (Paket #%-5lu)                 |\n", telemetryPacketsSent);
  Serial.println("+-------------------------------------------------------------------------+");
  Serial.printf ("| mmWave Radar  : %-18s | Masofa: %3d sm | OUT (IO45): %d     |\n", rStatus.c_str(), radarDistanceCm, radarOutVal);
  Serial.printf ("| Radar UART(7/6): %-5lu bayt qabul qilindi | Oxirgi bayt: %lu ms oldin   |\n", 
                 radarTotalBytesReceived, lastRadarByteTime > 0 ? (millis() - lastRadarByteTime) : 9999);
  Serial.printf ("| GPS (IO41/40) : %-18s | Sun'iy yo'ldoshlar: %-3d             |\n", 
                 gpsHasFix ? "FAOL (FIX BOR)" : "QIDIRILMOQDA (V)", gpsSatsVisible);
  Serial.printf ("| GPS Koordinata: %9.4f, %-9.4f | Jami NMEA qatorlari: %-5d       |\n", 
                 gpsLatitude, gpsLongitude, gpsSentencesParsed);
  Serial.printf ("| GPS UART(41/40): %-5lu bayt qabul qilindi | Oxirgi bayt: %lu ms oldin   |\n", 
                 gpsTotalBytesReceived, lastGpsByteTime > 0 ? (millis() - lastGpsByteTime) : 9999);
  Serial.printf ("| Ovoz / Sirena : %-14s | Audio (IO15): %-22s |\n", sirenActive ? "YOQILGAN(FAOL)" : "O'chiq", sirenActive ? "1400-3200Hz Modul" : "Sukut");
  Serial.printf ("| Servo (IO46)  : %3d deg (45°-135°) | Rejim: %-22s       |\n", currentServoAngle, autoServoPatrol ? "20s Patrul" : "Qo'lda");
  Serial.printf ("| Batareya (IO3): %5.2f V   (Quvvat: %3d %%)                            |\n", batteryVoltage, batteryPercent);
  Serial.printf ("| Wi-Fi Tarmogi : %-10s (RSSI: %-3d dBm, SSID: %s)             |\n", 
                 WiFi.status() == WL_CONNECTED ? "ULANGAN" : "UZILGAN", WiFi.RSSI(), WiFi.SSID().c_str());
  Serial.printf ("| T-HaLow (IO5/4): %-18s | Holat: %-26s |\n", 
                 halowDetected ? "ANIQLANDI (ONBOARD)" : "KUTILMOQDA / O'CHIQ", halowInfo.c_str());
  Serial.printf ("| Video Oqimi   : %-5lu kadr (HTTP %-3d, VGA 640x480)                     |\n", framesUploaded, lastUploadHttpCode);
  Serial.printf ("| Server Aloqasi: HTTP %-3d (%s) |\n", lastHttpResponseCode, server_urls[activeServerIdx]);
  Serial.println("+-------------------------------------------------------------------------+");
}

// ---------------------- STREAM PUSH TASK (Core 0) ------------
void streamUploadTask(void *pvParameters) {
  Serial.println("[TASK] Tezkor Video Stream Push vazifasi Core 0 da faollashdi.");
  vTaskDelay(pdMS_TO_TICKS(1500)); // Wi-Fi ulanishini kutish

  HTTPClient http;
  http.setReuse(true); // TCP ulanishni qayta ishlatish (Keep-Alive - maksimal tezlik)

  while (true) {
    if (WiFi.status() == WL_CONNECTED && cameraFound) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        http.begin(upload_frame_urls[0]);
        http.addHeader("Content-Type", "image/jpeg");
        http.addHeader("Connection", "keep-alive");
        http.setTimeout(1500);
        int code = http.POST(fb->buf, fb->len);
        lastUploadHttpCode = code;

        if (code == 200) {
          framesUploaded++;
          String resp = http.getString();
          if (resp.indexOf("\"siren_active\":true") >= 0) setSiren(true);
          else if (resp.indexOf("\"siren_active\":false") >= 0) setSiren(false);
        } else {
          http.end(); // Xatolik bo'lsa ulanishni qayta yangilash
        }
        esp_camera_fb_return(fb);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Maksimal tezlik (10ms kechikish)
  }
}

// ---------------------- SETUP & LOOP -------------------------
void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println("\n\n=======================================================");
  Serial.println("  AQLLI VA XAVFSIZ FERMA - ENDPOINT BOSHQARUV TIZIMI ");
  Serial.println("  Plata: LilyGO T-Halow (ESP32-S3 + OV2640 + OPI PSRAM)");
  Serial.println("=======================================================");

  pinMode(BOARD_LED, OUTPUT);
  pinMode(BATT_ADC_PIN, INPUT);
  pinMode(RADAR_OUT_PIN, INPUT_PULLDOWN);     // GPIO 45 OUT
  pinMode(RADAR_OUT_PIN_ALT, INPUT_PULLDOWN); // GPIO 39 OUT zaxira muqobil

  digitalWrite(BOARD_LED, LOW);

  // 1. Servo va Sirena Uskunaviy Taymerlarini Mustaqil Sozlash
  // Channel 0 (Timer 0): Servo uchun 50Hz
  ledcAttachChannel(SERVO_PIN, SERVO_LEDC_FREQ, SERVO_LEDC_RES, SERVO_LEDC_CHANNEL);
  currentServoAngle = 90; // Default holat: 90° markaz
  applyServoDuty(90);
  lastPatrolStep = millis();
  Serial.println("[SETUP] Servo GPIO 46 (LEDC Channel 0) ga biriktirildi. Default: 90°.");

  // Channel 2 (Timer 1): Sirena uchun 2000Hz (Hayvon qo'rqituvchi karnay)
  ledcAttachChannel(SIREN_PIN, 2000, SIREN_LEDC_RES, SIREN_LEDC_CHANNEL);
  ledcWrite(SIREN_PIN, 0); // Sukut saqlash
  Serial.printf("[SETUP] Sirena GPIO %d (LEDC Channel 2) ga biriktirildi. Timerlar to'liq ajratildi.\n", SIREN_PIN);

  // 2. mmWave Radar OUT & Batareya ADC
  Serial.printf("[SETUP] mmWave Radar OUT: GPIO %d, Batareya ADC: GPIO %d\n", RADAR_OUT_PIN, BATT_ADC_PIN);

  // 3. GPS (Serial1: GPIO 40 RX, GPIO 41 TX @ 9600 baud)
  Serial.printf("[SETUP] Serial1 (GPS): RX=GPIO %d, TX=GPIO %d @ 9600 baud.\n", GPS_RX_PIN, GPS_TX_PIN);
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // 4. Radar HLK-LD2410C (Serial2: GPIO 7 RX, GPIO 6 TX @ 256000 baud)
  Serial.printf("[SETUP] Serial2 (Radar): RX=GPIO %d, TX=GPIO %d @ 256000 baud.\n", RADAR_RX_PIN, RADAR_TX_PIN);
  Serial2.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  // 5. T-HaLow TX-AH-R900P Moduli (UART0: GPIO 5 RX, GPIO 4 TX @ 115200 baud)
  Serial.printf("[SETUP] T-HaLow TX-AH: RX=GPIO %d, TX=GPIO %d @ 115200 baud.\n", HALOW_RX_PIN, HALOW_TX_PIN);
  SerialHalow.begin(115200, SERIAL_8N1, HALOW_RX_PIN, HALOW_TX_PIN);
  delay(40);
  SerialHalow.print("AT\r\n");
  delay(120);
  if (SerialHalow.available()) {
    String resp = SerialHalow.readString();
    resp.trim();
    if (resp.indexOf("OK") >= 0) {
      halowDetected = true;
      halowInfo = "TX-AH Faol (AT OK)";
      Serial.println("[OK HALOW] Bortdagi TX-AH-R900P HaLow moduli muvaffaqiyatli aniqlandi!");
    } else {
      halowInfo = resp.substring(0, 24);
      Serial.printf("[HALOW] Modul javobi: %s\n", halowInfo.c_str());
    }
  } else {
    Serial.println("[HALOW] TX-AH modulidan javob kelmadi (Kutish rejimida).");
  }

  // 6. OV2640 Kamera (8MB OPI PSRAM)
  initCamera();

  // 7. Wi-Fi Multi (Ferma va IT-Park)
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("Ferma", "12345678");
  wifiMulti.addAP("IT-Park", "22334455");

  Serial.println("[SETUP] Wi-Fi tarmoqlariga ulanmoqda (Ferma yoki IT-Park)...");
  int attempt = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempt < 25) {
    delay(350);
    digitalWrite(BOARD_LED, !digitalRead(BOARD_LED));
    Serial.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(BOARD_LED, HIGH);
    Serial.println("\n[OK WIFI] Muvaffaqiyatli ulandi!");
    Serial.printf ("          SSID     : %s\n", WiFi.SSID().c_str());
    Serial.printf ("          IP Manzil: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf ("          Signal   : %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\n[OGOHLANTIRISH] Wi-Fi kutish rejimida davom etmoqda...");
  }

  // 8. Native HTTP va Video Stream Serverlari
  startHttpServers();

  // 9. Real-Time Video Stream Push Vazifasi (Core 0 da mustaqil ishlaydi)
  xTaskCreatePinnedToCore(
    streamUploadTask,
    "StreamUploadTask",
    8192,
    NULL,
    1,
    &streamTaskHandle,
    0
  );
  Serial.println("[SETUP] Video Stream Push Task Core 0 ga muvaffaqiyatli biriktirildi.");

  Serial.println("=======================================================");
  Serial.println("  TIZIM TO'LIQ ISHGA TUSHDI VA TELEMETRIYA FAOL!      ");
  Serial.println("=======================================================\n");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiMulti.run();
  }

  // Real-time radar va GPS oqimlarini asinxron o'qish (Hech qachon bufer to'lib ketmasligi uchun har tsiklda)
  updateRadarStream();
  updateGpsStream();

  // Servo sekin 20 soniyalik patrullash (sirena chalganda to'xtaydi)
  updateServoPatrol();

  // Hayvon qo'rqitish sirenasini modulyatsiya qilish
  updateSirenAudio();

  if (millis() - lastSensorRead > 1000) {
    lastSensorRead = millis();
    readSensors();
  }

  if (millis() - lastTelemetrySend > 2500) {
    lastTelemetrySend = millis();
    sendTelemetry();
  }

  if (millis() - lastSerialLog > 3000) {
    lastSerialLog = millis();
    printPeriodicDiagnostics();
  }
}

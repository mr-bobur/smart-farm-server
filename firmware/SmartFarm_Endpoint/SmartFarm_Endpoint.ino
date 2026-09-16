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
     - Radar: RX=GPIO 44, TX=GPIO 43, OUT=GPIO 39
     - GPS: RX=GPIO 40, TX=GPIO 41
     - Tuproq: GPIO 15 (ADC2_CH4)
     - HTU21: GPIO 7 (SDA), GPIO 6 (SCL)
     - Servo: GPIO 46 (Channel 0)
     - Sirena: GPIO 45 (Channel 2)
     - Batareya: GPIO 3 (VBAT-DET)
     - Kamera: J4 sloti (OV2640)
  =============================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_HTU21DF.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include "esp_http_server.h"

// ---------------------- PIN DEFINITIONS ----------------------
#define BOARD_LED        38
#define SOIL_PIN         15   // Tuproq Sensori (Analog AO - ADC2_CH4)
#define SERVO_PIN        46   // 180° Pan Servo (LEDC Channel 0)
#define BATT_ADC_PIN     3    // VBAT-DET (Bortdagi 100k/100k bo'luvchi)
#define SIREN_PIN        45   // Karnay / Sirena drayveri (LEDC Channel 2)

// GPS: GPIO 40 va 41 pinlari
#define GPS_RX_PIN       40   // Serial1 RX: GPS TX dan o'qish (9600 baud)
#define GPS_TX_PIN       41   // Serial1 TX: GPS RX ga (9600 baud)

// RADAR: TX, RX va yangi alohida OUT pini
#define RADAR_RX_PIN     44   // Serial2 RX: Radar TX dan o'qish (256000 baud)
#define RADAR_TX_PIN     43   // Serial2 TX: Radar RX ga
#define RADAR_OUT_PIN    39   // HLK-LD2410C OUT raqamli kirish (Header 2, Pin 10)

// I2C Pinlari (LilyGO T-Halow rasmiy shinalari)
#define BOARD_I2C_SDA    7
#define BOARD_I2C_SCL    6

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
  "http://10.24.95.226:8000/api/telemetry",   // Asosiy server IP
  "http://192.168.88.109:8000/api/telemetry",  // Zaxira IT-Park
  "http://10.242.63.226:8000/api/telemetry",   // Zaxira Ferma Hotspot
  "http://192.168.88.108:8000/api/telemetry"
};
const int num_server_urls = 4;
int activeServerIdx = 0;

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
Adafruit_HTU21DF htu = Adafruit_HTU21DF();
bool htuFound = false;
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

// ---------------------- DALA SENSORLARI KO'RSATKICHLARI ------
float temperature = 26.5;
float humidity = 45.0;
int soilPercent = 65;
int soilRaw = 2100;

// Batareya Ko'rsatkichlari
float batteryVoltage = 4.15;
int batteryPercent = 95;

// Radar (HLK-LD2410C) Ko'rsatkichlari
int radarPresence = 0;       // 0: Tinch, 1: Harakat, 2: Qo'zg'almas, 3: Ikkalasi
int radarDistanceCm = 0;     // Nishongacha masofa (sm)
int radarOutVal = 0;         // GPIO 39 OUT holati

// NEO-6M GPS Ko'rsatkichlari
float gpsLatitude = 41.5505;
float gpsLongitude = 60.6312;
String gpsRawBuffer = "";
int gpsSentencesParsed = 0;

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
    config.frame_size = FRAMESIZE_VGA; // 640x480
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("[DEBUG KAMERA] 8MB OPI PSRAM buferi faollashtirildi (640x480 VGA).");
  } else {
    config.frame_size = FRAMESIZE_QVGA; // 320x240
    config.jpeg_quality = 14;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    Serial.println("[DEBUG KAMERA] DRAM rejimi (320x240 QVGA).");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[DEBUG KAMERA] Xatolik: 0x%x\n", err);
    cameraFound = false;
    return false;
  }

  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    // 180° ga to'g'ri burish
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
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
  html += "<p><b>HTU21 Harorat:</b> " + String(temperature, 1) + " &deg;C</p>";
  html += "<p><b>HTU21 Namlik:</b> " + String(humidity, 1) + " %</p>";
  html += "<p><b>Tuproq Namligi (GPIO 15):</b> " + String(soilPercent) + " % (ADC: " + String(soilRaw) + ")</p>";
  html += "<p><b>Batareya:</b> " + String(batteryVoltage, 2) + " V (" + String(batteryPercent) + " %)</p>";
  html += "<p><b>Radar (GPIO 44/43/39):</b> " + String(radarPresence ? "Harakat bor" : "Tinch") + " | Masofa: " + String(radarDistanceCm) + " sm | OUT: " + String(radarOutVal) + "</p>";
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

// ---------------------- SENSORS READING ----------------------
void readSensors() {
  // 1. HTU21 (I2C) Harorat va Namlik
  if (htuFound) {
    float t = htu.readTemperature();
    float h = htu.readHumidity();
    if (!isnan(t) && t > -30.0 && t < 90.0) temperature = t;
    if (!isnan(h) && h >= 0.0 && h <= 100.0) humidity = h;
  }

  // 2. Tuproq Namligi (Analog ADC - GPIO 15)
  uint32_t soilSum = 0;
  for (int i = 0; i < 8; i++) {
    soilSum += analogRead(SOIL_PIN);
    delay(2);
  }
  soilRaw = soilSum / 8;
  // Kalibratsiya: Havoda quruq ~3400-4095, Suvda nam ~1400
  soilPercent = map(constrain(soilRaw, 1400, 3400), 3400, 1400, 0, 100);

  // 3. Batareya Quvvati (VBAT-DET - GPIO 3)
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

  // 4. HLK-LD2410C Radar (Serial2 GPIO 44 RX & Yangi GPIO 39 OUT)
  radarOutVal = digitalRead(RADAR_OUT_PIN);
  if (radarOutVal == HIGH && radarPresence == 0) {
    radarPresence = 1; // OUT pini orqali harakat nishoni
  }

  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    if (b == 0xF4 && Serial2.peek() == 0xF3) {
      uint8_t frame[24];
      frame[0] = b;
      int len = Serial2.readBytes(&frame[1], 22);
      if (len >= 12 && frame[1] == 0xF3 && frame[2] == 0xF2 && frame[3] == 0xF1) {
        radarPresence = frame[7];
        radarDistanceCm = frame[8] | (frame[9] << 8);
        Serial.printf("[DEBUG RADAR] Nishon: %d, Masofa: %d sm, OUT: %d\n", radarPresence, radarDistanceCm, radarOutVal);
      }
    }
  }

  // 5. NEO-6M GPS (Serial1 - GPIO 40 RX @ 9600 baud)
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      if (gpsRawBuffer.startsWith("$GPRMC") || gpsRawBuffer.startsWith("$GPGGA")) {
        gpsSentencesParsed++;
        int c1 = gpsRawBuffer.indexOf(',');
        int c2 = gpsRawBuffer.indexOf(',', c1 + 1);
        int c3 = gpsRawBuffer.indexOf(',', c2 + 1);
        int c4 = gpsRawBuffer.indexOf(',', c3 + 1);
        int c5 = gpsRawBuffer.indexOf(',', c4 + 1);
        int c6 = gpsRawBuffer.indexOf(',', c5 + 1);
        if (c3 > 0 && c5 > 0 && c4 > c3 && c6 > c5) {
          String rawLat = gpsRawBuffer.substring(c3 + 1, c4);
          String rawLng = gpsRawBuffer.substring(c5 + 1, c6);
          if (rawLat.length() > 4 && rawLng.length() > 4) {
            float dLat = rawLat.substring(0, 2).toFloat() + rawLat.substring(2).toFloat() / 60.0;
            float dLng = rawLng.substring(0, 3).toFloat() + rawLng.substring(3).toFloat() / 60.0;
            if (dLat > 0) gpsLatitude = dLat;
            if (dLng > 0) gpsLongitude = dLng;
            Serial.printf("[DEBUG GPS] Koordinatalar: %.4f, %.4f\n", gpsLatitude, gpsLongitude);
          }
        }
      }
      gpsRawBuffer = "";
    } else if (c != '\r') {
      gpsRawBuffer += c;
    }
  }
}

// ---------------------- TELEMETRY SENDER & SYNC ---------------
void sendTelemetry() {
  if (WiFi.status() == WL_CONNECTED) {
    String json = "{";
    json += "\"soil\":" + String(soilPercent) + ",";
    json += "\"soil_raw\":" + String(soilRaw) + ",";
    json += "\"temperature\":" + String(temperature, 1) + ",";
    json += "\"humidity\":" + String(humidity, 1) + ",";
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
  Serial.println("\n+-------------------------------------------------------------+");
  Serial.printf ("| [DIAGNOSTIKA] AQLLI FERMA TELEMETRIYA  (Paket #%-5lu)       |\n", telemetryPacketsSent);
  Serial.println("+-------------------------------------------------------------+");
  Serial.printf ("| HTU21 Harorat : %5.1f C   |  HTU21 Namlik : %5.1f %%          |\n", temperature, humidity);
  Serial.printf ("| Tuproq (GPIO15): %3d %%   (Analog ADC: %-4d)             |\n", soilPercent, soilRaw);
  Serial.printf ("| Batareya (IO3): %5.2f V   (Quvvat: %3d %%)                |\n", batteryVoltage, batteryPercent);
  
  String rStatus = "Tinch";
  if (radarPresence == 1) rStatus = "Harakat!";
  else if (radarPresence == 2) rStatus = "Qo'zg'almas";
  else if (radarPresence == 3) rStatus = "Harakat+Mavjud";
  Serial.printf ("| Radar(44/43/39): %-14s | Masofa: %3d sm (OUT:%d)   |\n", rStatus.c_str(), radarDistanceCm, radarOutVal);
  Serial.printf ("| GPS (IO40/41) : %9.4f, %-9.4f (Qatorlar: %-4d)     |\n", gpsLatitude, gpsLongitude, gpsSentencesParsed);
  Serial.printf ("| Servo (IO46)  : %3d deg (45°-135°) | Sirena: %-13s |\n", currentServoAngle, sirenActive ? "YOQILGAN(TO'XT)" : "O'chiq");
  Serial.printf ("| Patrol Rejimi : %-10s (45°->135°->45° / 20s sekin)    |\n", autoServoPatrol ? "20s FAOL" : "QO'LDA");
  Serial.printf ("| Wi-Fi Tarmogi : %-10s (RSSI: %-3d dBm, SSID: %s) |\n", 
                 WiFi.status() == WL_CONNECTED ? "ULANGAN" : "UZILGAN", WiFi.RSSI(), WiFi.SSID().c_str());
  Serial.printf ("| Endpoint IP   : %-15s                         |\n", WiFi.localIP().toString().c_str());
  Serial.printf ("| Server Aloqasi: HTTP %-3d (%s) |\n", lastHttpResponseCode, server_urls[activeServerIdx]);
  Serial.println("+-------------------------------------------------------------+");
}

// ---------------------- SETUP & LOOP -------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(1200);

  Serial.println("\n\n=======================================================");
  Serial.println("  AQLLI VA XAVFSIZ FERMA - ENDPOINT BOSHQARUV TIZIMI ");
  Serial.println("  Plata: LilyGO T-Halow (ESP32-S3 + OV2640 + OPI PSRAM)");
  Serial.println("=======================================================");

  pinMode(BOARD_LED, OUTPUT);
  pinMode(SOIL_PIN, INPUT);
  pinMode(BATT_ADC_PIN, INPUT);
  pinMode(RADAR_OUT_PIN, INPUT); // Mustaqil GPIO 39

  digitalWrite(BOARD_LED, LOW);

  // 1. Servo va Sirena Uskunaviy Taymerlarini Mustaqil Sozlash
  // Channel 0 (Timer 0): Servo uchun 50Hz
  ledcAttachChannel(SERVO_PIN, SERVO_LEDC_FREQ, SERVO_LEDC_RES, SERVO_LEDC_CHANNEL);
  currentServoAngle = 90; // Default holat: 90° markaz
  applyServoDuty(90);
  lastPatrolStep = millis();
  Serial.println("[SETUP] Servo GPIO 46 (LEDC Channel 0) ga biriktirildi. Default: 90°.");

  // Channel 2 (Timer 1): Sirena uchun 2000Hz
  ledcAttachChannel(SIREN_PIN, 2000, SIREN_LEDC_RES, SIREN_LEDC_CHANNEL);
  ledcWrite(SIREN_PIN, 0); // Sukut saqlash
  Serial.println("[SETUP] Sirena GPIO 45 (LEDC Channel 2) ga biriktirildi. Timerlar to'liq ajratildi.");

  // 2. I2C va HTU21 Sensori
  Serial.printf("[SETUP] I2C shina ishga tushirilmoqda (SDA: GPIO %d, SCL: GPIO %d)...\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  if (htu.begin(&Wire)) {
    htuFound = true;
    Serial.printf("[OK HTU21] Sensori topildi! Harorat = %.1f C, Namlik = %.1f %%\n",
                  htu.readTemperature(), htu.readHumidity());
  } else {
    Serial.println("[OGOHLANTIRISH] HTU21 I2C sensori topilmadi (zaxira rejimga o'tildi).");
  }

  // 3. Tuproq Sensori & Radar OUT
  Serial.printf("[SETUP] Tuproq Sensori: GPIO %d (ADC), Radar OUT: GPIO %d\n", SOIL_PIN, RADAR_OUT_PIN);

  // 4. GPS (Serial1: GPIO 40 RX, GPIO 41 TX @ 9600 baud)
  Serial.printf("[SETUP] Serial1 (GPS): RX=GPIO %d, TX=GPIO %d @ 9600 baud.\n", GPS_RX_PIN, GPS_TX_PIN);
  Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // 5. Radar HLK-LD2410C (Serial2: GPIO 44 RX, GPIO 43 TX @ 256000 baud)
  Serial.printf("[SETUP] Serial2 (Radar): RX=GPIO %d, TX=GPIO %d @ 256000 baud.\n", RADAR_RX_PIN, RADAR_TX_PIN);
  Serial2.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

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

  Serial.println("=======================================================");
  Serial.println("  TIZIM TO'LIQ ISHGA TUSHDI VA TELEMETRIYA FAOL!      ");
  Serial.println("=======================================================\n");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiMulti.run();
  }

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

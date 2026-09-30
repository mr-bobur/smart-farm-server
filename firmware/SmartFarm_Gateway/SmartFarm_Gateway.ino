/*
  =============================================================================
  Aqlli va Xavfsiz Ferma - Gateway Node (Qabul Qiluvchi Tugun)
  Plata: LilyGO T-Halow (ESP32-S3 + TX-AH Wi-Fi HaLow)
  Port: COM21
  =============================================================================
  Imkoniyatlari:
  1. Wi-Fi ("Ferma" / "IT-Park") tarmog'iga ulanish va barqaror qolish.
  2. Kamera video oqimi uchun Reverse Proxy (/stream):
     Telefon va kompyuter tarmog'idagi barcha qurilmalar Gateway orqali
     dala kamerasini (Endpoint: 10.190.174.110:81) to'g'ridan-to'g'ri ko'ra oladi!
  3. Serverga (http://170.168.60.245:8000) har 5 soniyada Heartbeat yuborish.
  4. Dala tugunidan kelgan telemetriyani serverga forward qilish (/relay_telemetry).
  5. Serverdan va Web interfacedan kelgan buyruqlarni (Servo burchagi, Sirena)
     dala tuguniga uzatish (/set_servo, /set_siren).
  6. T-HaLow TX-AH-R900P modulini sinash (/halow?cmd=...).
  =============================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

#define BOARD_LED 38

// T-HaLow (TX-AH-R900P) Hardware Serial Pinlari (LilyGO T-HaLow)
#define HALOW_RX_PIN 5
#define HALOW_TX_PIN 4

// 1. Wi-Fi Tarmoqlari
WiFiMulti wifiMulti;

// 2. Markaziy Server (Real tashqi server IP)
const char* server_host   = "170.168.60.245";
const int   server_port   = 8000;

// 3. Dala Tuguni (Endpoint) Tarmoq Sozlamalari
// Endpoint "Ferma" tarmog'ida 10.190.174.110 manzilida ishlamoqda
String endpoint_ip        = "10.190.174.110";
const int endpoint_port   = 80;
const int camera_port     = 81;

// Web Server va Uskunaviy Holatlar
WebServer gatewayServer(80);
HardwareSerial SerialHalow(0);

bool halowDetected = false;
String halowInfo = "Kutilmoqda...";
unsigned long lastHeartbeatSend = 0;
unsigned long packetsRelayed = 0;
int lastHeartbeatHttpCode = 0;

// ------------------- VIDEO STREAM REVERSE PROXY / REDIRECT ---------
void handleStreamProxy() {
  // HTTP 302 Redirect: Browser to'g'ridan-to'g'ri Endpoint video oqimiga o'tadi
  // Bu Gateway WebServerini hech qachon bloklamaydi va 30 FPS video beradi
  gatewayServer.sendHeader("Location", "http://" + endpoint_ip + ":81/stream");
  gatewayServer.send(302, "text/plain", "");
}

// ------------------- SERVERGA YURAK URISHI (HEARTBEAT) ------------
void sendGatewayHeartbeat() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + String(server_host) + ":" + String(server_port) + "/api/gateway_heartbeat";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(1500);

    String payload = "{";
    payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    payload += "\"endpoint_ip\":\"" + endpoint_ip + "\",";
    payload += "\"packets\":" + String(packetsRelayed) + ",";
    payload += "\"uptime\":" + String(millis() / 1000) + ",";
    payload += "\"halow\":\"" + halowInfo + "\"";
    payload += "}";

    lastHeartbeatHttpCode = http.POST(payload);
    http.end();
  }
}

// ------------------- T-HALOW BUYRUQ YUBORISH ----------------------
String sendHalowCommand(String cmd) {
  cmd.replace("\r", "");
  cmd.replace("\n", "");
  cmd += "\r\n";

  while (SerialHalow.available()) SerialHalow.read();
  SerialHalow.print(cmd);

  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 700) {
    while (SerialHalow.available()) {
      char c = (char)SerialHalow.read();
      resp += c;
    }
    delay(10);
  }
  if (resp.length() == 0) resp = "(Moduldan javob kelmadi)";
  return resp;
}

void setup() {
  Serial.begin(115200);
  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, LOW);
  delay(800);

  Serial.println("\n=======================================================");
  Serial.println("  Aqlli Ferma - LilyGO T-HaLow Gateway Node (COM21)    ");
  Serial.println("=======================================================");

  // 1. T-HaLow Modulini (GPIO 5 va 4 @ 115200 baud) zondlash
  Serial.printf("[SETUP] T-HaLow TX-AH zondlash (GPIO 5 va 4 @ 115200 baud)...\n");
  int hRx = HALOW_RX_PIN, hTx = HALOW_TX_PIN;
  SerialHalow.begin(115200, SERIAL_8N1, hRx, hTx);
  delay(60);
  while (SerialHalow.available()) SerialHalow.read();
  SerialHalow.print("AT+MAC_ADDR=?\r\n");
  delay(150);
  String hResp = "";
  if (SerialHalow.available()) {
    hResp = SerialHalow.readString();
  }
  if (hResp.indexOf("OK") >= 0 || hResp.indexOf("+") >= 0) {
    halowDetected = true;
    hResp.trim();
    halowInfo = "Faol (" + hResp.substring(0, 20) + ")";
    Serial.printf("[OK HALOW] RX=%d, TX=%d da HaLow moduli topildi: %s\n", hRx, hTx, hResp.c_str());
  } else {
    // Zaxira pinlar RX=4, TX=5 ni tekshirish
    SerialHalow.end();
    delay(40);
    hRx = 4; hTx = 5;
    SerialHalow.begin(115200, SERIAL_8N1, hRx, hTx);
    delay(60);
    while (SerialHalow.available()) SerialHalow.read();
    SerialHalow.print("AT+MAC_ADDR=?\r\n");
    delay(150);
    if (SerialHalow.available()) {
      hResp = SerialHalow.readString();
    }
    if (hResp.indexOf("OK") >= 0 || hResp.indexOf("+") >= 0) {
      halowDetected = true;
      hResp.trim();
      halowInfo = "Faol (" + hResp.substring(0, 20) + ")";
      Serial.printf("[OK HALOW] Zaxira RX=%d, TX=%d da HaLow topildi: %s\n", hRx, hTx, hResp.c_str());
    } else {
      Serial.println("[HALOW] Modul AT buyruqqa darhol javob bermadi (Transparent TTL / Kutilmoqda).");
    }
  }

  // 2. Wi-Fi ga Ulanish
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("Ferma", "12345678");
  wifiMulti.addAP("IT-Park", "22334455");

  Serial.print("[SETUP] Wi-Fi tarmog'iga ulanmoqda (Ferma / IT-Park)...");
  int attempt = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempt < 35) {
    delay(350);
    digitalWrite(BOARD_LED, !digitalRead(BOARD_LED));
    Serial.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(BOARD_LED, HIGH);
    Serial.println("\n[MUVAFFAQIYAT] Wi-Fi ga ulandi!");
    Serial.printf("Gateway IP manzili   : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Dala Tuguni (Endpoint): %s\n", endpoint_ip.c_str());
    Serial.printf("Markaziy Server      : http://%s:%d\n", server_host, server_port);
    Serial.printf("Kamera Stream Proxy  : http://%s/stream\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("smartfarm-gateway")) {
      Serial.println("mDNS ishga tushdi: http://smartfarm-gateway.local");
    }

    sendGatewayHeartbeat();
  } else {
    Serial.println("\n[OGOHLANTIRISH] Wi-Fi ga darhol ulanib bo'lmadi, fonda ulanish davom etmoqda...");
  }

  // ------------------- WEB SERVER ROUTES -------------------

  // 1. Asosiy Dashboard Sahifasi
  gatewayServer.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html lang='uz'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>LilyGO T-HaLow Gateway</title>";
    html += "<style>";
    html += ":root{--bg:#0f172a;--card:#1e293b;--accent:#38bdf8;--ok:#34d399;--warn:#f59e0b;--err:#f43f5e;--txt:#f8fafc;}";
    html += "body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--txt);margin:0;padding:20px;display:flex;justify-content:center;}";
    html += ".container{max-width:640px;width:100%;display:flex;flex-direction:column;gap:18px;}";
    html += ".card{background:var(--card);border:1px solid rgba(255,255,255,0.08);border-radius:14px;padding:20px;box-shadow:0 8px 24px rgba(0,0,0,0.35);}";
    html += "h1{margin:0 0 6px;font-size:22px;color:var(--accent);}";
    html += "p{margin:6px 0;font-size:14px;color:#cbd5e1;}";
    html += "b{color:#fff;}";
    html += ".btn{display:inline-block;padding:9px 15px;background:#2563eb;color:#fff;border-radius:8px;text-decoration:none;font-weight:600;font-size:13px;border:none;cursor:pointer;margin:4px;}";
    html += ".btn:hover{background:#1d4ed8;}.btn-warn{background:#d97706;}.btn-danger{background:#dc2626;}.btn-ok{background:#059669;}";
    html += "input[type='text']{padding:8px 12px;border-radius:6px;border:1px solid #475569;background:#0f172a;color:#fff;font-size:14px;width:60%;}";
    html += ".badge{padding:3px 8px;border-radius:6px;font-size:12px;font-weight:700;}";
    html += ".badge-ok{background:#065f46;color:#34d399;}.badge-warn{background:#78350f;color:#fbbf24;}";
    html += "</style></head><body><div class='container'>";

    html += "<div class='card'>";
    html += "<h1>Aqlli Ferma - LilyGO T-HaLow Gateway</h1>";
    html += "<p><b>Wi-Fi Holati:</b> <span class='badge " + String(WiFi.status() == WL_CONNECTED ? "badge-ok'>ULANGAN" : "badge-warn'>ULANMOQDA...") + "</span> (" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm)</p>";
    html += "<p><b>Gateway IP:</b> <a style='color:var(--accent);' href='http://" + WiFi.localIP().toString() + "' target='_blank'>" + WiFi.localIP().toString() + "</a></p>";
    html += "<p><b>Dala Tuguni (Endpoint) IP:</b> <b>" + endpoint_ip + "</b></p>";
    html += "<p><b>Markaziy Server:</b> <a style='color:var(--accent);' href='http://" + String(server_host) + ":" + String(server_port) + "' target='_blank'>http://" + String(server_host) + ":" + String(server_port) + "</a> (HTTP " + String(lastHeartbeatHttpCode) + ")</p>";
    html += "<p><b>T-HaLow Moduli:</b> " + halowInfo + "</p>";
    html += "<p><b>Uzatilgan Paketlar:</b> " + String(packetsRelayed) + "</p>";
    html += "</div>";

    // Video Stream bloki
    html += "<div class='card'>";
    html += "<h2 style='margin:0 0 10px;font-size:17px;color:#34d399;'>Jonli Kamera Oqimi</h2>";
    html += "<p>Gateway orqali bevosita dala kamerasini ko'rish:</p>";
    html += "<a href='/stream' class='btn btn-ok' target='_blank'> Jonli Videoni Ko'rish (/stream)</a>";
    html += "<a href='http://" + endpoint_ip + ":81/stream' class='btn' target='_blank'> To'g'ridan-to'g'ri Endpoint Video (:81)</a>";
    html += "</div>";

    // Boshqaruv tugmalari (Servo va Sirena)
    html += "<div class='card'>";
    html += "<h2 style='margin:0 0 10px;font-size:17px;color:#38bdf8;'>Dala Tugunini Boshqarish</h2>";
    html += "<p><b>Pan Servo:</b> ";
    html += "<button class='btn' onclick=\"fetch('/set_servo?angle=45').then(r=>r.text()).then(t=>alert(t))\">45° (Chap)</button>";
    html += "<button class='btn' onclick=\"fetch('/set_servo?angle=90').then(r=>r.text()).then(t=>alert(t))\">90° (Markaz)</button>";
    html += "<button class='btn' onclick=\"fetch('/set_servo?angle=135').then(r=>r.text()).then(t=>alert(t))\">135° (O'ng)</button>";
    html += "</p>";
    html += "<p><b>Hayvon Qo'rqituvchi Sirena:</b> ";
    html += "<button class='btn btn-danger' onclick=\"fetch('/set_siren?state=1').then(r=>r.text()).then(t=>alert(t))\"> Sirena YOQISH</button>";
    html += "<button class='btn btn-warn' onclick=\"fetch('/set_siren?state=0').then(r=>r.text()).then(t=>alert(t))\"> Sirena O'CHIRISH</button>";
    html += "</p>";
    html += "</div>";

    // Endpoint IP o'zgartirish
    html += "<div class='card'>";
    html += "<h2 style='margin:0 0 10px;font-size:17px;color:#cbd5e1;'>Endpoint IP Sozlash</h2>";
    html += "<form method='GET' action='/set_endpoint' style='display:flex;gap:8px;'>";
    html += "<input type='text' name='ip' value='" + endpoint_ip + "' placeholder='Masalan 10.190.174.110'>";
    html += "<button type='submit' class='btn'>Saqlash</button>";
    html += "</form>";
    html += "</div>";

    // T-HaLow AT Test Konsoli
    html += "<div class='card'>";
    html += "<h2 style='margin:0 0 10px;font-size:17px;color:#fbbf24;'>T-HaLow AT Buyruq Testi</h2>";
    html += "<p>Bortdagi TX-AH-R900P moduliga to'g'ridan-to'g'ri buyruq berish:</p>";
    html += "<form method='GET' action='/halow' target='_blank' style='display:flex;gap:8px;'>";
    html += "<input type='text' name='cmd' value='AT+MAC_ADDR=?' placeholder='AT+MAC_ADDR=?'>";
    html += "<button type='submit' class='btn btn-warn'>Yuborish</button>";
    html += "</form>";
    html += "</div>";

    html += "</div></body></html>";
    gatewayServer.send(200, "text/html", html);
  });

  // 2. Video oqimi uchun Reverse Proxy
  gatewayServer.on("/stream", HTTP_GET, handleStreamProxy);

  // 3. Endpoint IP o'zgartirish API
  gatewayServer.on("/set_endpoint", HTTP_GET, []() {
    if (gatewayServer.hasArg("ip")) {
      endpoint_ip = gatewayServer.arg("ip");
      endpoint_ip.trim();
      gatewayServer.send(200, "text/plain", "Muvaffaqiyatli: Endpoint IP = " + endpoint_ip);
    } else {
      gatewayServer.send(400, "text/plain", "Xatolik: 'ip' parametri kiritilmadi");
    }
  });

  // 4. Telemetriya qabuli va serverga forward qilish
  gatewayServer.on("/relay_telemetry", HTTP_POST, []() {
    if (gatewayServer.hasArg("plain")) {
      String jsonPayload = gatewayServer.arg("plain");
      packetsRelayed++;

      // Agar JSON ichida endpoint_ip bo'lsa, avtomatik yangilash
      int ipIdx = jsonPayload.indexOf("\"endpoint_ip\":\"");
      if (ipIdx >= 0) {
        int st = ipIdx + 15;
        int en = jsonPayload.indexOf("\"", st);
        if (en > st) {
          endpoint_ip = jsonPayload.substring(st, en);
        }
      }

      digitalWrite(BOARD_LED, LOW);
      delay(15);
      digitalWrite(BOARD_LED, HIGH);

      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "http://" + String(server_host) + ":" + String(server_port) + "/api/telemetry";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(1200);
        int httpCode = http.POST(jsonPayload);
        http.end();
      }
      gatewayServer.send(200, "application/json", "{\"status\":\"ok\",\"relayed\":true}");
    } else {
      gatewayServer.send(400, "text/plain", "Bo'sh ma'lumot");
    }
  });

  // 5. Servoni burish buyrug'i (Gateway -> Endpoint ga uzatish)
  gatewayServer.on("/set_servo", HTTP_GET, []() {
    if (gatewayServer.hasArg("angle")) {
      String angle = gatewayServer.arg("angle");
      HTTPClient http;
      http.begin("http://" + endpoint_ip + "/servo?angle=" + angle);
      http.setTimeout(1500);
      int code = http.GET();
      http.end();
      gatewayServer.send(200, "text/plain", "Servo burchagi o'rnatildi: " + angle + " (HTTP " + String(code) + ")");
    } else {
      gatewayServer.send(400, "text/plain", "Burchak kiritilmadi");
    }
  });

  // 6. Sirena buyrug'i (Gateway -> Endpoint ga uzatish)
  gatewayServer.on("/set_siren", HTTP_GET, []() {
    if (gatewayServer.hasArg("state")) {
      String state = gatewayServer.arg("state");
      HTTPClient http;
      http.begin("http://" + endpoint_ip + "/siren?state=" + state);
      http.setTimeout(1500);
      int code = http.GET();
      http.end();
      gatewayServer.send(200, "text/plain", "Sirena holati: " + state + " (HTTP " + String(code) + ")");
    } else {
      gatewayServer.send(400, "text/plain", "Holat kiritilmadi");
    }
  });

  // 7. T-HaLow AT Buyruqlarini yuborish va javobini qaytarish API
  gatewayServer.on("/halow", HTTP_GET, []() {
    String cmd = gatewayServer.hasArg("cmd") ? gatewayServer.arg("cmd") : "AT+MAC_ADDR=?";
    String resp = sendHalowCommand(cmd);
    String out = "=== LilyGO T-HaLow AT Test ===\n";
    out += "BUYRUQ: " + cmd + "\n";
    out += "JAVOB :\n" + resp + "\n";
    gatewayServer.send(200, "text/plain; charset=utf-8", out);
  });

  gatewayServer.begin();
  Serial.println("[OK HTTP] Gateway Web Serveri (Port 80) Faol.");
}

void loop() {
  gatewayServer.handleClient();

  // Har 5 soniyada serverga Heartbeat yuborish
  if (millis() - lastHeartbeatSend > 5000) {
    lastHeartbeatSend = millis();
    if (WiFi.status() == WL_CONNECTED) {
      sendGatewayHeartbeat();
      digitalWrite(BOARD_LED, HIGH);
    } else {
      digitalWrite(BOARD_LED, LOW);
      wifiMulti.run();
    }
  }
}

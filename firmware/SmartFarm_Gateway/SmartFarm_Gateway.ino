/*
  =============================================================================
  Aqlli va Xavfsiz Ferma - Gateway Node (Qabul Qiluvchi Tugun)
  Plata: LilyGO T-Halow (ESP32-S3 + TX-AH Wi-Fi HaLow)
  Port: COM3
  =============================================================================
  Imkoniyatlari:
  1. Telefon tarqatgan "Ferma" Wi-Fi Hotspotiga ulanish.
  2. Kamera video oqimi uchun Reverse Proxy (/stream):
     Telefon va Kompyuter tarmog'idagi barcha qurilmalar Gateway orqali
     dala kamerasini to'g'ridan-to'g'ri ko'ra oladi!
  3. Serverga (http://10.242.63.226:8000) har 5 soniyada Heartbeat yuborish.
  4. Dala tugunidan kelgan telemetriyani serverga forward qilish.
  5. Serverdan kelgan buyruqlarni (Servo burchagi, Sirena) dala tuguniga uzatish.
  =============================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

#define BOARD_LED 38

// 1. Telefon Tarqatgan Wi-Fi Hotspot Sozlamalari
const char* wifi_ssid     = "Ferma";
const char* wifi_password = "12345678";

// 2. Markaziy Server (Kompyuter) Sozlamalari (Real tashqi server IP)
const char* server_host   = "170.168.60.245";
const int   server_port   = 8000;

// 3. Dala Tuguni (Endpoint) HaLow IP va Portlari
const char* endpoint_ip   = "192.168.1.10";
const int   endpoint_port = 80;
const int   camera_port   = 81;

WebServer gatewayServer(80);
unsigned long lastHeartbeatSend = 0;
unsigned long packetsRelayed = 0;

// ------------------- VIDEO STREAM REVERSE PROXY -------------------
void handleStreamProxy() {
  WiFiClient endpointClient;
  if (!endpointClient.connect(endpoint_ip, camera_port)) {
    gatewayServer.send(503, "text/plain", "Kameraga ulanib bo'lmadi (192.168.1.10:81 o'chiq)");
    return;
  }

  endpointClient.print("GET / HTTP/1.1\r\nHost: " + String(endpoint_ip) + "\r\nConnection: close\r\n\r\n");

  WiFiClient client = gatewayServer.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Access-Control-Allow-Origin: *");
  client.println();

  uint8_t buffer[1024];
  while (client.connected() && endpointClient.connected()) {
    int available = endpointClient.available();
    if (available > 0) {
      int toRead = min((int)sizeof(buffer), available);
      int bytesRead = endpointClient.read(buffer, toRead);
      if (bytesRead > 0) {
        client.write(buffer, bytesRead);
      }
    }
    delay(1);
  }

  endpointClient.stop();
  client.stop();
}

// ------------------- SERVERGA YURAK URISHI (HEARTBEAT) ------------
void sendGatewayHeartbeat() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + String(server_host) + ":" + String(server_port) + "/api/gateway_heartbeat";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    String payload = "{";
    payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    payload += "\"packets\":" + String(packetsRelayed) + ",";
    payload += "\"uptime\":" + String(millis() / 1000);
    payload += "}";

    int code = http.POST(payload);
    http.end();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, LOW);
  delay(1000);

  Serial.println("\n=======================================================");
  Serial.println("  Aqlli Ferma - Gateway (Plata 2) -> 'Ferma' Hotspot   ");
  Serial.println("=======================================================");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(wifi_ssid, wifi_password);

  Serial.printf("Wi-Fi ga ulanmoqda: %s ", wifi_ssid);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 40) {
    delay(400);
    digitalWrite(BOARD_LED, !digitalRead(BOARD_LED));
    Serial.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(BOARD_LED, HIGH);
    Serial.println("\n[MUVAFFAQIYAT] 'Ferma' Wi-Fi tarmog'iga ulandi!");
    Serial.print("Gateway IP manzili  : ");
    Serial.println(WiFi.localIP());
    Serial.printf("Markaziy Server     : http://%s:%d\n", server_host, server_port);
    Serial.printf("Kamera Stream Proxy : http://%s/stream\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("smartfarm-gateway")) {
      Serial.println("mDNS ishga tushdi: http://smartfarm-gateway.local");
    }

    sendGatewayHeartbeat();
  } else {
    Serial.println("\n[OGOHLANTIRISH] Wi-Fi ga darhol ulanib bo'lmadi, fonda ulanish davom etmoqda...");
  }

  // 1. Asosiy sahifa
  gatewayServer.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Gateway</title>";
    html += "<style>body{font-family:sans-serif;background:#111827;color:#fff;padding:30px;line-height:1.6;}";
    html += ".card{background:#1f2937;padding:20px;border-radius:12px;max-width:550px;margin:auto;}";
    html += "a{color:#38bdf8;}h2{color:#34d399;}</style></head><body><div class='card'>";
    html += "<h2>Aqlli Ferma - LilyGO T-HaLow Gateway</h2>";
    html += "<p><b>Wi-Fi Status:</b> " + String(WiFi.status() == WL_CONNECTED ? "Ulangan (Ferma AP)" : "Ulanmoqda...") + "</p>";
    html += "<p><b>Gateway IP:</b> " + WiFi.localIP().toString() + "</p>";
    html += "<p><b>Markaziy Server:</b> <a href='http://" + String(server_host) + ":" + String(server_port) + "' target='_blank'>http://" + String(server_host) + ":" + String(server_port) + "</a></p>";
    html += "<p><b>Kamera Oqimi (Proxy):</b> <a href='/stream' target='_blank'>Jonli Efir (/stream)</a></p>";
    html += "<p><b>Uzatilgan paketlar:</b> " + String(packetsRelayed) + "</p>";
    html += "</div></body></html>";
    gatewayServer.send(200, "text/html", html);
  });

  // 2. Video oqimi uchun Reverse Proxy
  gatewayServer.on("/stream", HTTP_GET, handleStreamProxy);

  // 3. Telemetriya qabuli va serverga forward qilish
  gatewayServer.on("/relay_telemetry", HTTP_POST, []() {
    if (gatewayServer.hasArg("plain")) {
      String jsonPayload = gatewayServer.arg("plain");
      packetsRelayed++;

      digitalWrite(BOARD_LED, LOW);
      delay(20);
      digitalWrite(BOARD_LED, HIGH);

      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "http://" + String(server_host) + ":" + String(server_port) + "/api/telemetry";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(jsonPayload);
        http.end();
      }
      gatewayServer.send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      gatewayServer.send(400, "text/plain", "Bo'sh ma'lumot");
    }
  });

  // 4. Servoni burish buyrug'i
  gatewayServer.on("/set_servo", HTTP_GET, []() {
    if (gatewayServer.hasArg("angle")) {
      String angle = gatewayServer.arg("angle");
      HTTPClient http;
      http.begin("http://" + String(endpoint_ip) + "/servo?angle=" + angle);
      int code = http.GET();
      http.end();
      gatewayServer.send(200, "text/plain", "Servo: " + angle);
    } else {
      gatewayServer.send(400, "text/plain", "Burchak kiritilmadi");
    }
  });

  // 5. Sirena buyrug'i
  gatewayServer.on("/set_siren", HTTP_GET, []() {
    if (gatewayServer.hasArg("state")) {
      String state = gatewayServer.arg("state");
      HTTPClient http;
      http.begin("http://" + String(endpoint_ip) + "/siren?state=" + state);
      int code = http.GET();
      http.end();
      gatewayServer.send(200, "text/plain", "Sirena: " + state);
    } else {
      gatewayServer.send(400, "text/plain", "Holat kiritilmadi");
    }
  });

  gatewayServer.begin();
  Serial.println("Gateway Web Serveri (Port 80) Faol.");
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
      WiFi.reconnect();
    }
  }
}

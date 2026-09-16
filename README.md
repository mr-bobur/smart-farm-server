# 🌾 Aqlli va Xavfsiz Ferma - Node.js Backend Server & Web Dashboard

Ushbu loyiha **Aqlli va Xavfsiz Ferma** (Smart & Secure Farm) IoT tizimining **Node.js** muhitiga to'liq o'tkazilgan markaziy server qismidir.

## 🌟 Imkoniyatlar

- **Node.js + Express**: Yuqori tezlikdagi asinxron arxitektura, kam xotira va CPU sarfi.
- **MJPEG Video Oqimi & Jonli Efir**: ESP32-S3 kamerasining `/stream` portidan videoni qabul qilib `/video_feed` orqali brauzerlarga uzatish.
- **Yolg'on Xavf Filtriga Ega Harakat Tahlili**: 4.5 soniyadan ortiq davom etgan uzluksiz harakatlargagina xavf deb topish va hayvon qo'rqituvchi sirenani avtomatik faollashtirish.
- **Real-Time WebSocket (`ws`)**: Datchiklar telemetriyasi, batareya quvvati, GPS xaritasi va qurilmalar holatini real vaqtda yangilab turish.
- **PTZ Servo & Sirena Boshqaruvi**: 45° dan 135° gacha bo'lgan oraliqda boshqaruv va 20s patrul monitoringi.
- **Zamonaviy Web Dashboard**: Tailwind CSS, FontAwesome va Leaflet.js xaritasi.

---

## 🚀 Serverni O'rnatish va Ishga Tushirish

### 1. Repozitoriyani yuklab olish
```bash
git clone https://github.com/mr-bobur/smart-farm-server.git
cd smart-farm-server
```

### 2. Kutubxonalarni o'rnatish
```bash
npm install
```

### 3. Ishga tushirish
```bash
npm start
```
yoki:
```bash
node server.js
```
Server standart holatda `http://localhost:8000` (yoki tarmoqda `http://<server-ip>:8000`) portida ishga tushadi.

---

## ⚙️ Linux Serverda 24/7 Avtomatik Ishlash (systemd)

Linux serverida doimiy fonda ishlashi va server o'chib-yonsa avtomatik qayta yoqilishi uchun:

```bash
sudo nano /etc/systemd/system/smartfarm.service
```

Quyidagi konfiguratsiyani kiriting (`WorkingDirectory` va `ExecStart` yo'llarini o'zingiznikiga moslang):
```ini
[Unit]
Description=Smart Farm Node.js AI Server & IoT Dashboard
After=network.target

[Service]
User=root
WorkingDirectory=/root/smart-farm-server
ExecStart=/usr/bin/node server.js
Restart=always
RestartSec=5
Environment=PORT=8000
Environment=HOST=0.0.0.0

[Install]
WantedBy=multi-user.target
```

Servisni yoqish:
```bash
sudo systemctl daemon-reload
sudo systemctl enable smartfarm
sudo systemctl start smartfarm
sudo systemctl status smartfarm
```

---

## 📡 REST API Yo'nalishlari

| Metod | URL | Vazifasi |
| :--- | :--- | :--- |
| `GET` | `/` | Asosiy Web Dashboard boshqaruv paneli |
| `GET` | `/video_feed` | Jonli MJPEG video oqimi (kamera yoki zaxira kadr) |
| `WS`  | `/ws` | Real-time WebSocket telemetriya kanali |
| `POST`| `/api/telemetry` | Dala tugunidan (Endpoint) datchik ma'lumotlarini qabul qilish |
| `POST`| `/api/siren` | Hayvon qo'rqituvchi sirenani yoqish/o'chirish (`{"active": true/false}`) |
| `POST`| `/api/servo` | Kamerani 45°-135° oralig'ida burish (`{"angle": 90}`) |
| `POST`| `/api/connect_camera` | Kameraning `/stream` oqimiga avtomatik bog'lanish |

---

## 🔌 Mikrokontroller Proshivkalari (Firmware)

Ushbu repozitoriyaning `firmware/` papkasida **LilyGO T-Halow (ESP32-S3)** platalari uchun to'liq Arduino proshivkalari joylashgan:

* **`firmware/SmartFarm_Endpoint/SmartFarm_Endpoint.ino`** — Dala (Endpoint) tuguni:
  - OV2640 kamerasi (180° sozlangan, `/stream` port 81).
  - 45°-135° burchak ostida 20 soniyalik uzluksiz patrul aylanishiga ega Servo (LEDC Channel 0, 50Hz).
  - Yirtqich hayvonlarni qo'rqituvchi akustik sirena (LEDC Channel 2, 1400Hz-3200Hz sweep). Sirena chalayotganda servo avtomatik to'xtatiladi.
  - HLK-LD2410C 24GHz Millimetr-to'lqinli inson/harakat radari (RX: 44, TX: 43, OUT: GPIO 39).
  - HTU21 datchigi (havo harorati va namligi) va tuproq namligi sensori (GPIO 15).
  - AT6558 GPS moduli (RX: 40, TX: 41) va LiPo batareya monitoringi (GPIO 3).

* **`firmware/SmartFarm_Gateway/SmartFarm_Gateway.ino`** — Qabul qiluvchi Gateway tuguni:
  - Wi-Fi Hotspot ("Ferma") ga ulanish.
  - Video oqimi uchun Reverse Proxy (`/stream`).
  - Har 5 soniyada serverga Heartbeat va telemetriya uzatish.

---

## 🛡️ Muallif
- **Bobur Abdugafforov** ([@mr-bobur](https://github.com/mr-bobur))

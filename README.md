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

## 🛡️ Muallif
- **Bobur Abdugafforov** ([@mr-bobur](https://github.com/mr-bobur))

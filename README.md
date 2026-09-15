# 🌾 Aqlli va Xavfsiz Ferma - AI Backend Server & Web Dashboard

Ushbu loyiha **Aqlli va Xavfsiz Ferma** (Smart & Secure Farm) IoT tizimining markaziy server qismi bo'lib, quyidagi imkoniyatlarni taqdim etadi:

- **AI Video Tahlil**: OV2640 / RTSP / MJPEG kamera oqimidan real vaqtda harakat, odam va hayvonlarni aniqlash (MOG2 + temporal persistence filtri).
- **Yolg'on xavf filtri**: 4.5 soniyadan ko'p davom etgan harakatlargagina xavf deb topish va avtomatik ravishda hayvon qo'rqituvchi sirenani faollashtirish.
- **Jonli Telemetriya (IoT)**: Tuproq namligi (ADC), havo harorati va namligi (HTU21 I2C), batareya quvvati (VBAT-DET), 24GHz radar (HLK-LD2410C) va GPS koordinatalari.
- **PTZ Kamera & Sirena Boshqaruvi**: 45° dan 135° gacha bo'lgan burchakda 20 soniyalik avtomatik patrul qilish va masofadan buyruq yuborish.
- **Real-Time Web Dashboard**: Tailwind CSS, WebSockets va Leaflet.js xaritasi bilan to'liq interaktiv boshqaruv paneli.

---

## 🚀 Serverni O'rnatish va Ishga Tushirish (Linux / Ubuntu)

### 1. Repozitoriyani yuklab olish
```bash
git clone https://github.com/mr-bobur/smart-farm-server.git
cd smart-farm-server
```

### 2. Python virtual muhitini yaratish
```bash
sudo apt update && sudo apt install -y python3 python3-pip python3-venv libgl1 libglib2.0-0
python3 -m venv venv
source venv/bin/activate
```

### 3. Kutubxonalarni o'rnatish
```bash
pip install --upgrade pip
pip install -r requirements.txt
```

### 4. Serverni ishga tushirish
```bash
python3 app.py
```
Server standart holatda `http://0.0.0.0:8000` portida ishga tushadi.

---

## ⚙️ Linux Serverda 24/7 Avtomatik Ishlash (systemd)

Serverni fonda (background) uzluksiz ishlashi va server o'chib-yonsa avtomatik qayta yoqilishi uchun systemd xizmati:

```bash
sudo nano /etc/systemd/system/smartfarm.service
```

Quyidagi konfiguratsiyani kiriting (`USER` va yo'llarni o'zingiznikiga moslang):
```ini
[Unit]
Description=Smart Farm AI Server & IoT Dashboard
After=network.target

[Service]
User=root
WorkingDirectory=/path/to/smart-farm-server
ExecStart=/path/to/smart-farm-server/venv/bin/python app.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Xizmatni yoqish:
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
| `GET` | `/` | Asosiy Web Dashboard |
| `GET` | `/video_feed` | AI qayta ishlangan jonli MJPEG video oqimi |
| `WS`  | `/ws` | Real-time WebSocket telemetriya kanali |
| `POST`| `/api/telemetry` | Dala tugunidan (Endpoint) telemetriya qabul qilish |
| `POST`| `/api/siren` | Sirenani yoqish/o'chirish (`{"active": true/false}`) |
| `POST`| `/api/servo` | Kamerani 45°-135° oralig'ida burish (`{"angle": 90}`) |
| `POST`| `/api/connect_camera` | Kameraning `/stream` oqimiga bir bosishda ulanish |

---

## 🛡️ Muallif
- **Bobur Abdugafforov** ([@mr-bobur](https://github.com/mr-bobur))

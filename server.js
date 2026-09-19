const express = require('express');
const http = require('http');
const path = require('path');
const fs = require('fs');
const { WebSocketServer, WebSocket } = require('ws');
const SmartFarmDetector = require('./detector');

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

app.use(express.json());

const detector = new SmartFarmDetector();

// In-memory tizim holati (System State)
const systemState = {
  gateway: {
    online: false,
    ip: "Ulanmagan",
    rssi: 0,
    packets: 0,
    uptime: 0,
    last_seen: 0
  },
  endpoint: {
    online: false,
    ip: "10.24.95.110",
    last_seen: 0,
    camera_online: false
  },
  telemetry: {
    soil: 65,
    soil_raw: 2100,
    temperature: 26.5,
    humidity: 45.0,
    battery: 95,
    battery_voltage: 4.15,
    radar: 0,
    radar_distance: 0,
    lat: 41.5505,
    lng: 60.6312,
    endpoint_ip: "10.24.95.110"
  },
  servo_angle: 90,
  siren_active: false,
  auto_siren: true,
  ai_alert: false,
  ai_message: "Tizim Tinch (Xavfsiz)"
};

// WebSocket Broadcaster
function broadcastWs(message) {
  const data = JSON.stringify(message);
  for (const client of wss.clients) {
    if (client.readyState === WebSocket.OPEN) {
      try {
        client.send(data);
      } catch (e) {}
    }
  }
}

// AI Detector tomonidan aniqlangan xavf sirenasi
detector.sirenCallback = async (active) => {
  systemState.siren_active = active;
  systemState.ai_alert = active;
  const msg = `AI SIRENA: ${active ? 'YOQILDI' : 'O`CHIRILDI'}`;
  
  broadcastWs({
    ai_alert: active,
    ai_message: detector.alertMessage,
    log: msg,
    siren_active: active
  });

  const endpointIp = systemState.endpoint.ip;
  if (endpointIp && endpointIp !== "Ulanmagan") {
    try {
      fetch(`http://${endpointIp}/siren?state=${active ? '1' : '0'}`, {
        signal: AbortSignal.timeout(1200)
      }).catch(() => {});
    } catch (e) {}
  }
};

// WebSocket ulanishlarini qabul qilish
wss.on('connection', (ws) => {
  // Yangi mijozga to'liq boshlang'ich holatni uzatish
  ws.send(JSON.stringify({
    gateway: systemState.gateway,
    endpoint: systemState.endpoint,
    telemetry: systemState.telemetry,
    servo_angle: systemState.servo_angle,
    siren_active: systemState.siren_active,
    ai_alert: detector.alertActive,
    ai_message: detector.alertMessage,
    log: "Node.js Serverga muvaffaqiyatli ulandi."
  }));

  ws.on('message', (msg) => {
    // Mijozdan kelgan ping yoki xabarlarni qayta ishlash
  });
});

// ----------------- ROUTES -----------------

// Asosiy Dashboard sahifasi
app.get('/', (req, res) => {
  const dashboardPath = path.join(__dirname, 'templates', 'dashboard.html');
  fs.readFile(dashboardPath, 'utf8', (err, htmlContent) => {
    if (err) {
      return res.status(500).send("Dashboard fayli topilmadi");
    }
    res.type('html').send(htmlContent);
  });
});

// Jonli Video Oqimi (MJPEG)
app.get('/video_feed', (req, res) => {
  detector.handleFeedRequest(req, res);
});

// Endpoint kamerasidan kadr qabul qilish (Frame Push Rejimi - NAT/Tashqi server uchun)
app.post('/api/upload_frame', express.raw({ type: ['image/jpeg', 'application/octet-stream', '*/*'], limit: '5mb' }), (req, res) => {
  if (req.body && req.body.length > 100) {
    detector.pushFrame(req.body);
    systemState.endpoint.camera_online = true;
    systemState.endpoint.last_seen = Math.floor(Date.now() / 1000);
    
    // Javobda boshqaruv signallarini qaytarish (ultra tez sinxronizatsiya)
    return res.json({
      status: "ok",
      servo_angle: systemState.servo_angle,
      siren_active: systemState.siren_active
    });
  }
  res.status(400).json({ error: "Bo'sh yoki noto'g'ri kadr" });
});

// Gateway Heartbeat qabul qilish
app.post('/api/gateway_heartbeat', (req, res) => {
  const now = Math.floor(Date.now() / 1000);
  const data = req.body || {};

  systemState.gateway.online = true;
  systemState.gateway.ip = data.ip || systemState.gateway.ip;
  systemState.gateway.rssi = data.rssi || 0;
  systemState.gateway.packets = data.packets || 0;
  systemState.gateway.uptime = data.uptime || 0;
  systemState.gateway.last_seen = now;

  broadcastWs({ gateway: systemState.gateway });
  res.json({ status: "ok", time: now });
});

// Telemetriya qabul qilish (Endpoint yoki Gateway orqali)
app.post('/api/telemetry', (req, res) => {
  const now = Math.floor(Date.now() / 1000);
  const data = req.body || {};

  // Endpoint holatini yangilash
  systemState.endpoint.online = true;
  systemState.endpoint.last_seen = now;

  if (data.endpoint_ip) {
    systemState.endpoint.ip = data.endpoint_ip;
    systemState.telemetry.endpoint_ip = data.endpoint_ip;

    // Agar kamera o'zi kadr push qilmayotgan bo'lsa (masalan, lokal tarmoqda), pull rejimida urinib ko'rish
    const isPushing = (Date.now() - detector.lastFrameTime < 4000);
    const streamUrl = `http://${data.endpoint_ip}:81/stream`;
    if (!isPushing && (!detector.cameraUrl || detector.cameraUrl !== streamUrl)) {
      detector.startStream(streamUrl);
      systemState.endpoint.camera_online = true;
    }
  }

  // Agar Gateway orqali kelgan bo'lsa
  if (data.gateway_ip) {
    systemState.gateway.online = true;
    systemState.gateway.ip = data.gateway_ip;
    systemState.gateway.last_seen = now;
  }

  // Dala datchiklari ma'lumotlari
  if (data.soil !== undefined) systemState.telemetry.soil = data.soil;
  if (data.soil_raw !== undefined) systemState.telemetry.soil_raw = data.soil_raw;
  if (data.temperature !== undefined) systemState.telemetry.temperature = data.temperature;
  if (data.humidity !== undefined) systemState.telemetry.humidity = data.humidity;
  if (data.battery !== undefined) systemState.telemetry.battery = data.battery;
  if (data.battery_voltage !== undefined) systemState.telemetry.battery_voltage = data.battery_voltage;
  if (data.radar !== undefined) systemState.telemetry.radar = data.radar;
  if (data.radar_distance !== undefined) systemState.telemetry.radar_distance = data.radar_distance;
  if (data.lat !== undefined) systemState.telemetry.lat = data.lat;
  if (data.lng !== undefined) systemState.telemetry.lng = data.lng;
  if (data.servo_angle !== undefined) systemState.servo_angle = data.servo_angle;

  broadcastWs({
    gateway: systemState.gateway,
    endpoint: systemState.endpoint,
    telemetry: systemState.telemetry,
    servo_angle: systemState.servo_angle,
    siren_active: systemState.siren_active,
    ai_alert: detector.alertActive,
    ai_message: detector.alertMessage
  });

  res.json({
    status: "ok",
    servo_angle: systemState.servo_angle,
    siren_active: systemState.siren_active
  });
});

// Servo Burchagini Boshqarish (45° dan 135° gacha)
app.post('/api/servo', (req, res) => {
  const reqAngle = parseInt(req.body.angle) || 90;
  const angle = Math.max(45, Math.min(135, reqAngle));

  systemState.servo_angle = angle;
  broadcastWs({ servo_angle: angle, log: `Kamera burildi: ${angle}°` });

  const endpointIp = systemState.endpoint.ip;
  if (endpointIp && endpointIp !== "Ulanmagan") {
    fetch(`http://${endpointIp}/servo?angle=${angle}`, {
      signal: AbortSignal.timeout(1200)
    }).catch(() => {});
  }

  res.json({ status: "ok", angle });
});

app.get('/api/servo', (req, res) => {
  res.json({ angle: systemState.servo_angle });
});

// Sirena (Kalonka) Boshqarish
app.post('/api/siren', (req, res) => {
  const active = Boolean(req.body.active);
  systemState.siren_active = active;

  broadcastWs({
    siren_active: active,
    log: `Sirena: ${active ? 'YOQILDI' : 'O`CHIRILDI'}`
  });

  const endpointIp = systemState.endpoint.ip;
  if (endpointIp && endpointIp !== "Ulanmagan") {
    fetch(`http://${endpointIp}/siren?state=${active ? '1' : '0'}`, {
      signal: AbortSignal.timeout(1200)
    }).catch((err) => {
      console.warn(`[WARN] Endpoint /siren chaqiruvida xatolik: ${err.message}`);
    });
  }

  res.json({ status: "ok", siren_active: active });
});

app.get('/api/siren', (req, res) => {
  res.json({ siren_active: systemState.siren_active });
});

// Avtomatik Hayvon Qo'rqitishni Yoqish/O'chirish
app.post('/api/auto_siren', (req, res) => {
  const enabled = Boolean(req.body.enabled);
  detector.autoSiren = enabled;
  systemState.auto_siren = enabled;

  const logMsg = `Avtomatik hayvon qo'rqitish: ${enabled ? 'YOQILDI' : 'O`CHIRILDI'}`;
  broadcastWs({ log: logMsg });
  res.json({ status: "ok", auto_siren: enabled });
});

// To'g'ridan-to'g'ri Kameraga Bog'lanish
app.post('/api/connect_camera', (req, res) => {
  const endpointIp = systemState.endpoint.ip || "10.24.95.110";
  const streamUrl = `http://${endpointIp}:81/stream`;

  detector.startStream(streamUrl);
  systemState.endpoint.camera_online = true;

  broadcastWs({
    log: `Kameraga ulanish yo'lga qo'yildi: ${streamUrl}`,
    endpoint: systemState.endpoint
  });

  res.json({ status: "ok", camera_url: streamUrl });
});

app.post('/api/set_camera_url', (req, res) => {
  const url = req.body.url;
  if (url) {
    detector.startStream(url);
    res.json({ status: "ok", camera_url: url });
  } else {
    res.status(400).json({ error: "URL ko'rsatilmadi" });
  }
});

// Qurilmalarning Aloqada Ekanligini Kuzatuvchi Watchdog Taymeri
setInterval(() => {
  const now = Math.floor(Date.now() / 1000);
  let changed = false;

  if (systemState.gateway.online) {
    if ((now - systemState.gateway.last_seen) > 15) {
      systemState.gateway.online = false;
      changed = true;
    }
  }

  if (systemState.endpoint.online) {
    if ((now - systemState.endpoint.last_seen) > 10) {
      systemState.endpoint.online = false;
      changed = true;
    }
  }

  if (changed && wss.clients.size > 0) {
    broadcastWs({
      gateway: systemState.gateway,
      endpoint: systemState.endpoint
    });
  }
}, 2000);

// Serverni Ishga Tushirish
const PORT = process.env.PORT || 8000;
const HOST = process.env.HOST || '0.0.0.0';

server.listen(PORT, HOST, () => {
  console.log(`=======================================================`);
  console.log(`  AQLLI VA XAVFSIZ FERMA - NODE.JS SERVER FAOL!       `);
  console.log(`  Boshqaruv Paneli: http://localhost:${PORT}          `);
  console.log(`  Tarmoq Manzili   : http://${HOST}:${PORT}           `);
  console.log(`=======================================================`);
});

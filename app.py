import asyncio
import json
import time
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import HTMLResponse, StreamingResponse, JSONResponse
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel
import uvicorn
import requests

from ai_detector import SmartFarmDetector

app = FastAPI(title="Aqlli & Xavfsiz Ferma Server")
templates = Jinja2Templates(directory="templates")

detector = SmartFarmDetector()

# In-memory System State with Device Connectivity Tracking
system_state = {
    "gateway": {
        "online": False,
        "ip": "Ulanmagan",
        "rssi": 0,
        "packets": 0,
        "uptime": 0,
        "last_seen": 0
    },
    "endpoint": {
        "online": False,
        "ip": "10.24.95.110",
        "last_seen": 0,
        "camera_online": False
    },
    "telemetry": {
        "soil": 65,
        "soil_raw": 2100,
        "temperature": 26.5,
        "humidity": 45.0,
        "battery": 95,
        "battery_voltage": 4.15,
        "radar": 0,
        "radar_distance": 0,
        "lat": 41.5505,
        "lng": 60.6312,
        "endpoint_ip": "10.24.95.110"
    },
    "servo_angle": 90,
    "siren_active": False,
    "auto_siren": True,
    "ai_alert": False,
    "ai_message": "Tizim Tinch (Xavfsiz)"
}

# Active WebSocket connections
connected_clients = set()

# Siren trigger callback from AI detector
def on_ai_siren(active: bool):
    system_state["siren_active"] = active
    system_state["ai_alert"] = active
    msg = f"AI SIRENA: {'YOQILDI' if active else 'O`CHIRILDI'}"
    asyncio.run(broadcast_ws({"ai_alert": active, "ai_message": detector.alert_message, "log": msg, "siren_active": active}))
    
    endpoint_ip = system_state["telemetry"].get("endpoint_ip")
    if endpoint_ip and system_state["endpoint"]["online"]:
        try:
            requests.get(f"http://{endpoint_ip}/siren?state={'1' if active else '0'}", timeout=0.8)
        except Exception:
            pass

detector.siren_callback = on_ai_siren

# Models
class ServoModel(BaseModel):
    angle: int

class SirenModel(BaseModel):
    active: bool

class AutoSirenModel(BaseModel):
    enabled: bool

class CameraUrlModel(BaseModel):
    url: str

class GatewayHeartbeatModel(BaseModel):
    ip: str = ""
    rssi: int = 0
    packets: int = 0
    uptime: int = 0

class TelemetryModel(BaseModel):
    soil: float = 0.0
    soil_raw: int = 0
    temperature: float = 0.0
    humidity: float = 0.0
    battery: float = 100.0
    battery_voltage: float = 4.20
    radar: int = 0
    radar_distance: int = 0
    lat: float = 0.0
    lng: float = 0.0
    servo_angle: int = 90
    siren_active: bool = False
    endpoint_ip: str = ""
    gateway_ip: str = ""

# WebSocket Broadcaster
async def broadcast_ws(message: dict):
    data = json.dumps(message)
    dead_clients = []
    for client in connected_clients:
        try:
            await client.send_text(data)
        except Exception:
            dead_clients.append(client)
    for dc in dead_clients:
        connected_clients.remove(dc)

# Routes
@app.get("/", response_class=HTMLResponse)
async def get_dashboard(request: Request):
    return templates.TemplateResponse("dashboard.html", {"request": request})

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    connected_clients.add(websocket)
    # Send full initial state
    await websocket.send_text(json.dumps({
        "gateway": system_state["gateway"],
        "endpoint": system_state["endpoint"],
        "telemetry": system_state["telemetry"],
        "servo_angle": system_state["servo_angle"],
        "siren_active": system_state["siren_active"],
        "ai_alert": detector.alert_active,
        "ai_message": detector.alert_message,
        "log": "Serverga muvaffaqiyatli ulandi."
    }))
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        connected_clients.remove(websocket)

# Video Stream Generator
def generate_mjpeg():
    while True:
        frame_bytes = detector.get_jpeg_frame()
        if frame_bytes is not None:
            yield (b"--frame\r\n"
                   b"Content-Type: image/jpeg\r\n\r\n" + frame_bytes + b"\r\n")
        time.sleep(0.04) # ~25 fps

@app.get("/video_feed")
async def video_feed():
    return StreamingResponse(generate_mjpeg(), media_type="multipart/x-mixed-replace; boundary=frame")

# Gateway Heartbeat endpoint
@app.post("/api/gateway_heartbeat")
async def receive_gateway_heartbeat(data: GatewayHeartbeatModel):
    now = time.time()
    system_state["gateway"]["online"] = True
    system_state["gateway"]["ip"] = data.ip
    system_state["gateway"]["rssi"] = data.rssi
    system_state["gateway"]["packets"] = data.packets
    system_state["gateway"]["uptime"] = data.uptime
    system_state["gateway"]["last_seen"] = now

    await broadcast_ws({"gateway": system_state["gateway"]})
    return {"status": "ok", "time": now}

# Sensor Telemetry Ingestion (from Gateway or Endpoint)
@app.post("/api/telemetry")
async def receive_telemetry(data: TelemetryModel):
    now = time.time()
    
    # Update Endpoint state
    system_state["endpoint"]["online"] = True
    system_state["endpoint"]["last_seen"] = now
    if data.endpoint_ip and data.endpoint_ip != "Ulanmagan":
        system_state["endpoint"]["ip"] = data.endpoint_ip
        system_state["telemetry"]["endpoint_ip"] = data.endpoint_ip

        # Auto-connect camera stream!
        stream_url = f"http://{data.endpoint_ip}:81/stream"
        if not detector.camera_url or detector.camera_url != stream_url:
            detector.start_stream(stream_url)
            system_state["endpoint"]["camera_online"] = True

    # If sent through gateway, mark gateway online too
    if data.gateway_ip:
        system_state["gateway"]["online"] = True
        system_state["gateway"]["ip"] = data.gateway_ip
        system_state["gateway"]["last_seen"] = now

    system_state["telemetry"]["soil"] = data.soil
    system_state["telemetry"]["soil_raw"] = data.soil_raw
    system_state["telemetry"]["temperature"] = data.temperature
    system_state["telemetry"]["humidity"] = data.humidity
    system_state["telemetry"]["battery"] = data.battery
    system_state["telemetry"]["battery_voltage"] = data.battery_voltage
    system_state["telemetry"]["radar"] = data.radar
    system_state["telemetry"]["radar_distance"] = data.radar_distance
    system_state["telemetry"]["lat"] = data.lat
    system_state["telemetry"]["lng"] = data.lng
    system_state["servo_angle"] = data.servo_angle

    await broadcast_ws({
        "gateway": system_state["gateway"],
        "endpoint": system_state["endpoint"],
        "telemetry": system_state["telemetry"],
        "servo_angle": system_state["servo_angle"],
        "siren_active": system_state["siren_active"],
        "ai_alert": detector.alert_active,
        "ai_message": detector.alert_message
    })
    return {"status": "ok", "servo_angle": system_state["servo_angle"], "siren_active": system_state["siren_active"]}

# Servo Angle Control
@app.post("/api/servo")
async def set_servo(data: ServoModel):
    angle = max(45, min(135, data.angle))
    system_state["servo_angle"] = angle
    await broadcast_ws({"servo_angle": angle, "log": f"Kamera burildi: {angle}°"})
    
    endpoint_ip = system_state["endpoint"].get("ip")
    if endpoint_ip and endpoint_ip != "Ulanmagan":
        async def send_endpoint_servo(ip, ang):
            try:
                loop = asyncio.get_event_loop()
                await loop.run_in_executor(
                    None,
                    lambda: requests.get(f"http://{ip}/servo?angle={ang}", timeout=1.0)
                )
            except Exception:
                pass
        asyncio.create_task(send_endpoint_servo(endpoint_ip, angle))
        
    return {"status": "ok", "angle": angle}

@app.get("/api/servo")
async def get_servo():
    return {"angle": system_state["servo_angle"]}

# Siren (Hayvon Qo'rqituvchi Kalonka) Control
@app.post("/api/siren")
async def set_siren(data: SirenModel):
    system_state["siren_active"] = data.active
    await broadcast_ws({
        "siren_active": data.active,
        "log": f"Sirena: {'YOQILDI' if data.active else 'O`CHIRILDI'}"
    })
    
    endpoint_ip = system_state["endpoint"].get("ip")
    if endpoint_ip and endpoint_ip != "Ulanmagan":
        async def send_endpoint_siren(ip, active_state):
            try:
                loop = asyncio.get_event_loop()
                await loop.run_in_executor(
                    None,
                    lambda: requests.get(f"http://{ip}/siren?state={'1' if active_state else '0'}", timeout=1.2)
                )
            except Exception as e:
                print(f"[WARN] Endpoint /siren chaqiruvida xatolik: {e}")
        asyncio.create_task(send_endpoint_siren(endpoint_ip, data.active))

    return {"status": "ok", "siren_active": data.active}

@app.get("/api/siren")
async def get_siren():
    return {"siren_active": system_state["siren_active"]}

@app.post("/api/auto_siren")
async def set_auto_siren(data: AutoSirenModel):
    detector.auto_siren = data.enabled
    system_state["auto_siren"] = data.enabled
    log_msg = f"Avtomatik hayvon qo'rqitish: {'YOQILDI' if data.enabled else 'O`CHIRILDI'}"
    await broadcast_ws({"log": log_msg})
    return {"status": "ok", "auto_siren": data.enabled}

# Direct Camera Stream Connection API
@app.post("/api/connect_camera")
async def connect_camera():
    endpoint_ip = system_state["endpoint"].get("ip", "10.24.95.110")
    stream_url = f"http://{endpoint_ip}:81/stream"
    detector.start_stream(stream_url)
    system_state["endpoint"]["camera_online"] = True
    await broadcast_ws({
        "log": f"Kameraga ulanish yo'lga qo'yildi: {stream_url}",
        "endpoint": system_state["endpoint"]
    })
    return {"status": "ok", "camera_url": stream_url}

@app.post("/api/set_camera_url")
async def set_camera_url(data: CameraUrlModel):
    detector.start_stream(data.url)
    return {"status": "ok", "camera_url": data.url}

# Periodic Device Liveness Watchdog Task
@app.on_event("startup")
async def startup_event():
    async def device_watchdog():
        while True:
            await asyncio.sleep(2)
            now = time.time()
            changed = False

            if system_state["gateway"]["online"]:
                if (now - system_state["gateway"]["last_seen"]) > 15:
                    system_state["gateway"]["online"] = False
                    changed = True

            if system_state["endpoint"]["online"]:
                if (now - system_state["endpoint"]["last_seen"]) > 10:
                    system_state["endpoint"]["online"] = False
                    changed = True

            if changed and connected_clients:
                await broadcast_ws({
                    "gateway": system_state["gateway"],
                    "endpoint": system_state["endpoint"]
                })
    asyncio.create_task(device_watchdog())

if __name__ == "__main__":
    uvicorn.run("app:app", host="0.0.0.0", port=8000, reload=False)

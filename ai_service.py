#!/usr/bin/env python3
"""
Smart Farm AI Animal and Motion Detector Service
OpenCV MOG2 + Morfologik filtr + 3.5s Temporal Persistence Filter
"""
import sys
import time
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
import cv2
import numpy as np

class SmartFarmAIDetector:
    def __init__(self):
        # MOG2 Background Subtractor
        self.bg_subtractor = cv2.createBackgroundSubtractorMOG2(history=350, varThreshold=26, detectShadows=True)
        
        # State
        self.alert_active = False
        self.alert_message = "Tizim Tinch (Xavfsiz)"
        self.detected_objects = []
        self.auto_siren = True
        self.last_alert_time = 0
        
        # 3.5 soniyalik harakat tasdiqlash filtri
        self.motion_start_time = 0.0
        self.motion_last_seen = 0.0
        self.motion_duration = 0.0
        self.CONFIRM_THRESHOLD_SEC = 3.5 # 3.5 soniya uzluksiz harakat tasdiqlanishi

    def process_frame(self, jpeg_bytes):
        # 1. JPEG ni NumPy massiviga dekodlash
        nparr = np.frombuffer(jpeg_bytes, np.uint8)
        frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
        if frame is None:
            return jpeg_bytes, False, self.alert_message, [], False

        h, w = frame.shape[:2]
        total_frame_area = w * h
        display_frame = frame.copy()
        detected = []
        raw_motion = False
        total_motion_area = 0
        now = time.time()

        # 2. MOG2 orqa fon ajratgich
        fg_mask = self.bg_subtractor.apply(frame)

        # 3. Morfologik shovqin filtri
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
        thresh = cv2.morphologyEx(fg_mask, cv2.MORPH_OPEN, kernel)
        thresh = cv2.morphologyEx(thresh, cv2.MORPH_DILATE, kernel)
        _, thresh = cv2.threshold(thresh, 180, 255, cv2.THRESH_BINARY)

        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        for c in contours:
            area = cv2.contourArea(c)
            total_motion_area += area

            # 320x240 o'lchamga moslashtirilgan o'lcham chegarasi (shovqinlarni chetlatish)
            if area > 450:
                (x, y, bw, bh) = cv2.boundingRect(c)

                # Agar bitta obyekt butun kadrning 40% dan ortig'ini egallasa (kamera burilganda) -> inkor qilish
                if area > total_frame_area * 0.40:
                    continue

                raw_motion = True
                aspect_ratio = bh / float(bw)

                # Hayvon va obyektlarni tasniflash (Classification)
                if area > 2400 and aspect_ratio > 1.2:
                    label = "ODAM / SHAXS?"
                    color = (0, 0, 240) # Qizil
                    detected.append("Odam")
                elif area > 1100:
                    label = "YIRIK HAYVON?"
                    color = (0, 140, 255) # To'q sariq (Orange)
                    detected.append("Yirik Hayvon")
                else:
                    label = "HARAKAT?"
                    color = (0, 225, 255) # Sariq
                    detected.append("Harakatlanuvchi Jism")

                # Kadrdagi obyekt atrofini chizish
                cv2.rectangle(display_frame, (x, y), (x + bw, y + bh), color, 2)
                cv2.putText(display_frame, label, (x, max(16, y - 5)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.42, color, 1, cv2.LINE_AA)

        # Agar kadrning 45% dan ortig'i bir vaqtda qimirlasa (servo patrullash payti) -> global harakat
        if total_motion_area > total_frame_area * 0.45:
            raw_motion = False
            detected = []

        # 4. --- 3.5 SONIYALIK DAVOMIYLIK VA TASDIQLASH FILTRI ---
        if raw_motion:
            self.motion_last_seen = now
            if self.motion_start_time == 0.0:
                self.motion_start_time = now
            self.motion_duration = now - self.motion_start_time
        else:
            # Agar 1.2 soniya harakat ko'rinmasa taymer qayta tiklanadi
            if now - self.motion_last_seen > 1.2:
                self.motion_start_time = 0.0
                self.motion_duration = 0.0

        is_confirmed_alert = (self.motion_duration >= self.CONFIRM_THRESHOLD_SEC)
        siren_trigger = False

        self.alert_active = is_confirmed_alert
        self.detected_objects = list(set(detected))

        if is_confirmed_alert:
            det_str = ", ".join(self.detected_objects) if self.detected_objects else "Harakat / Hayvon"
            self.alert_message = f"XAVF TASDIQLANDI ({self.motion_duration:.1f}s): {det_str}!"

            # Avtomatik sirena signali (har 6 soniyada bir marta)
            if self.auto_siren and (now - self.last_alert_time > 6.0):
                self.last_alert_time = now
                siren_trigger = True
        elif self.motion_duration > 0.8:
            det_str = ", ".join(self.detected_objects) if self.detected_objects else "Obyekt"
            self.alert_message = f"Harakat tekshirilmoqda... ({self.motion_duration:.1f}s / {self.CONFIRM_THRESHOLD_SEC:.1f}s) [{det_str}]"
        else:
            self.alert_message = "Tizim Tinch (Xavfsiz)"

        # 5. Kadr tepasiga AI holat sarlavhasi (Banner) chizish
        if is_confirmed_alert:
            status_color = (0, 0, 200) # Qizil
            status_text = f"[AI XAVF] TASDIQLANDI ({self.motion_duration:.1f}s) | SIRENA!"
        elif self.motion_duration > 0.8:
            status_color = (0, 130, 240) # To'q sariq (Tekshiruv)
            status_text = f"[AI TAHLIL] {self.motion_duration:.1f}s / {self.CONFIRM_THRESHOLD_SEC:.1f}s | {', '.join(detected) if detected else 'Harakat'}"
        else:
            status_color = (0, 140, 0) # Yashil (Tinch)
            status_text = f"[AI TINCH] Xavfsiz | {time.strftime('%H:%M:%S')}"

        cv2.rectangle(display_frame, (0, 0), (w, 22), status_color, -1)
        cv2.putText(display_frame, status_text, (6, 16),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.42, (255, 255, 255), 1, cv2.LINE_AA)

        # 6. Annotated kadrni JPEG ga kodlash
        ret, out_jpeg = cv2.imencode('.jpg', display_frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
        if ret:
            return out_jpeg.tobytes(), is_confirmed_alert, self.alert_message, self.detected_objects, siren_trigger
        return jpeg_bytes, is_confirmed_alert, self.alert_message, self.detected_objects, siren_trigger

detector = SmartFarmAIDetector()

class AIDetectionHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == '/process_frame':
            content_length = int(self.headers.get('Content-Length', 0))
            if content_length <= 0:
                self.send_response(400)
                self.end_headers()
                return

            body = self.rfile.read(content_length)
            annotated_jpeg, is_alert, msg, objects, siren = detector.process_frame(body)

            self.send_response(200)
            self.send_header('Content-Type', 'image/jpeg')
            self.send_header('Content-Length', str(len(annotated_jpeg)))
            self.send_header('X-AI-Alert', '1' if is_alert else '0')
            self.send_header('X-AI-Siren', '1' if siren else '0')
            self.send_header('X-AI-Duration', f'{detector.motion_duration:.1f}')
            self.send_header('X-AI-Message', urllib.parse.quote(msg))
            self.send_header('X-AI-Objects', urllib.parse.quote(",".join(objects)))
            self.end_headers()
            self.wfile.write(annotated_jpeg)
            return

        self.send_response(404)
        self.end_headers()

    def do_GET(self):
        if self.path == '/status':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            import json
            data = {
                "alert_active": detector.alert_active,
                "alert_message": detector.alert_message,
                "detected_objects": detector.detected_objects,
                "motion_duration": detector.motion_duration,
                "confirm_threshold": detector.CONFIRM_THRESHOLD_SEC
            }
            self.wfile.write(json.dumps(data).encode('utf-8'))
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, format, *args):
        return

def run(port=5001):
    server = HTTPServer(('127.0.0.1', port), AIDetectionHandler)
    print(f"Smart Farm AI Animal Detector Service faol: http://127.0.0.1:{port}", flush=True)
    server.serve_forever()

if __name__ == '__main__':
    port = 5001
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    run(port)

#!/usr/bin/env python3
"""
Smart Farm AI Animal & Intruder Detection Microservice
OpenCV MOG2 + Morfologik filtr (640x480 VGA moslashuvi) + 3.0s Temporal Persistence Filter + 5.0s Sirena Cooldown
ThreadingHTTPServer orqali yuqori tezlik va barqarorlik
"""
import sys
import time
import urllib.parse
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import cv2
import numpy as np

class SmartFarmAIDetector:
    def __init__(self):
        # MOG2 Background Subtractor
        self.bg_subtractor = cv2.createBackgroundSubtractorMOG2(history=300, varThreshold=26, detectShadows=True)
        self.current_shape = None
        
        # Holatlar
        self.alert_active = False
        self.siren_active = False
        self.alert_message = "Tizim Tinch (Xavfsiz)"
        self.detected_objects = []
        self.auto_siren = True
        self.last_alert_time = 0
        
        # 3.0 soniyalik harakat tasdiqlash va 5.0 soniyalik sirena cooldown filtri
        self.motion_start_time = 0.0
        self.motion_last_seen = 0.0
        self.motion_duration = 0.0
        self.CONFIRM_THRESHOLD_SEC = 3.0 # 3.0 soniya uzluksiz hayvon harakatida xavf tasdiqlanadi
        self.SIREN_COOLDOWN_SEC = 5.0    # Hayvon ketgandan so'ng 5.0 soniya o'tib sirena o'chadi

        # Kamera burilishi (Pan) va silkinishini aniqlash
        self.prev_gray = None
        self.prev_gray_f32 = None
        self.last_pan_time = 0.0

    def process_frame(self, jpeg_bytes):
        try:
            # 1. JPEG ni dekodlash
            nparr = np.frombuffer(jpeg_bytes, np.uint8)
            frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            if frame is None:
                return jpeg_bytes, False, self.alert_message, [], self.siren_active

            h, w = frame.shape[:2]
            total_frame_area = w * h
            display_frame = frame.copy()
            now = time.time()

            # Rezolyutsiya o'zgarganda (masalan 320x240 -> 640x480) fon modelini qayta sozlash
            if self.current_shape != (h, w):
                self.current_shape = (h, w)
                self.prev_gray = None
                self.prev_gray_f32 = None
                self.bg_subtractor = cv2.createBackgroundSubtractorMOG2(history=300, varThreshold=26, detectShadows=True)
                print(f"[AI INFO] Yangi kadr o'lchami: {w}x{h}. Fon modeli qayta sozlandi.", flush=True)

            # 2. Kamera harakatini (Servo burilishi / Pan) global siljish orqali aniqlash
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            is_camera_panning = False
            pan_shift = 0.0

            if self.prev_gray_f32 is not None:
                if self.prev_gray_f32.shape == gray.shape:
                    try:
                        shift, response = cv2.phaseCorrelate(self.prev_gray_f32, np.float32(gray))
                        pan_shift = float(np.sqrt(shift[0]**2 + shift[1]**2))
                        # Agar butun kadr 0.8px dan ko'p siljigan bo'lsa -> Kamera harakatlanmoqda
                        if pan_shift > 0.80 and response > 0.20:
                            is_camera_panning = True
                            self.last_pan_time = now
                    except Exception:
                        pan_shift = 0.0
                else:
                    self.prev_gray_f32 = None
                    self.prev_gray = None

            self.prev_gray = gray
            self.prev_gray_f32 = np.float32(gray)

            # Agar servo ayni damda qadam tashlayotgan bo'lsa
            is_pan_settling = (now - self.last_pan_time < 0.08)

            if is_camera_panning or is_pan_settling:
                # Fon modelini tezlashtirilgan tarzda moslashtirish (burilishda eski kadr qoldig'ini tozalash)
                self.bg_subtractor.apply(frame, learningRate=0.15)
                # Kamera aylanayotganda soxta signal bermaslik uchun taymerni pauzaga olish
                self.motion_start_time = 0.0
                self.motion_duration = 0.0

            # 3. MOG2 orqa fon ajratish
            fg_mask = self.bg_subtractor.apply(frame)

            # 4. Kengaytirilgan morfologik shovqin filtri (640x480 da 7x7, kichik o'lchamda 5x5)
            k_size = 7 if w >= 640 else 5
            kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (k_size, k_size))
            thresh = cv2.morphologyEx(fg_mask, cv2.MORPH_OPEN, kernel)
            thresh = cv2.morphologyEx(thresh, cv2.MORPH_DILATE, kernel)
            _, thresh = cv2.threshold(thresh, 180, 255, cv2.THRESH_BINARY)

            contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

            valid_contours = []
            total_motion_area = 0

            # Kadr o'lchamiga mos adaptiv miqyos (QVGA 320x240 -> scale=1.0, VGA 640x480 -> scale=4.0)
            scale = max(1.0, total_frame_area / 76800.0)
            th_min = int(600 * scale)          # 640x480 da ~2400 px
            th_large = int(1200 * scale)       # 640x480 da ~4800 px
            th_human = int(2200 * scale)       # 640x480 da ~8800 px
            min_dim = int(16 * np.sqrt(scale)) # 640x480 da ~32 px

            for c in contours:
                area = cv2.contourArea(c)
                total_motion_area += area

                if area >= th_min:
                    (x, y, bw, bh) = cv2.boundingRect(c)
                    solidity = area / float(bw * bh) if (bw * bh) > 0 else 0
                    aspect_ratio = bh / float(bw) if bw > 0 else 0

                    # Shovqinlarni chetlatish filtrlari:
                    # 1. Juda kichik o'lchamlar (< min_dim)
                    if bw < min_dim or bh < min_dim:
                        continue
                    # 2. Solidity past bo'lgan holatlar (daraxt shoxlari, mayda tarqoq barglar < 0.26)
                    if solidity < 0.26:
                        continue
                    # 3. Vertikal devor/ustunlar (AR > 3.2) va gorizontal sim/yer chiziqlari (AR < 0.35)
                    # Haqiqiy hayvonlar (qo'y, qoramol, it, bo'ri) AR odatda 0.4 .. 2.8 oralig'ida bo'ladi!
                    if aspect_ratio > 3.2 or aspect_ratio < 0.35:
                        continue

                    # Tasniflash (Classification)
                    if area >= th_human and aspect_ratio > 1.25:
                        label = "ODAM / SHAXS"
                        color = (0, 0, 240) # Qizil
                        tag = "Odam"
                    elif area >= th_large:
                        label = "YIRIK HAYVON"
                        color = (0, 140, 255) # To'q sariq (Orange)
                        tag = "Yirik Hayvon"
                    else:
                        label = "HAYVON / JISM"
                        color = (0, 220, 255) # Sariq
                        tag = "Hayvon"

                    valid_contours.append((x, y, bw, bh, color, label, tag, area))

            # Hayvon harakatini tekshirish:
            # Hayvon 1-3 ta jamlangan butun kontur bo'ladi va maydoni kadrning 25% dan oshmaydi
            raw_motion = False
            detected = []

            if 0 < len(valid_contours) <= 3 and total_motion_area <= total_frame_area * 0.25 and not is_camera_panning:
                raw_motion = True
                for (x, y, bw, bh, color, label, tag, c_area) in valid_contours:
                    detected.append(tag)
                    cv2.rectangle(display_frame, (x, y), (x + bw, y + bh), color, 2)
                    cv2.putText(display_frame, f"{label} ({int(c_area)}px)", (x, max(18, y - 6)),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.48 if w >= 640 else 0.40, color, 1, cv2.LINE_AA)

            # 5. --- HAYVON HARAKATI DAVOMIYLIGI VA 5s SIRENA TA'XIR (COOLDOWN) FILTRI ---
            if raw_motion:
                self.motion_last_seen = now
                if self.motion_start_time == 0.0:
                    self.motion_start_time = now
                self.motion_duration = now - self.motion_start_time
                self.detected_objects = list(set(detected))

                if self.motion_duration >= self.CONFIRM_THRESHOLD_SEC:
                    self.siren_active = True
                    self.alert_active = True
                    det_str = ", ".join(self.detected_objects) if self.detected_objects else "Hayvon"
                    self.alert_message = f"XAVF TASDIQLANDI ({self.motion_duration:.1f}s): {det_str}!"
                    status_color = (0, 0, 200) # Qizil
                    status_text = f"[AI XAVF] {det_str.upper()} ({self.motion_duration:.1f}s) | SIRENA FAOL!"
                elif self.motion_duration > 0.5:
                    det_str = ", ".join(self.detected_objects) if self.detected_objects else "Hayvon"
                    self.alert_message = f"Hayvon tahlil qilinmoqda... ({self.motion_duration:.1f}s / {self.CONFIRM_THRESHOLD_SEC:.1f}s) [{det_str}]"
                    status_color = (0, 130, 240) # To'q sariq
                    status_text = f"[AI TAHLIL] {self.motion_duration:.1f}s / {self.CONFIRM_THRESHOLD_SEC:.1f}s | {det_str}"
                else:
                    self.alert_message = "Tizim Tinch (Xavfsiz)"
                    status_color = (0, 140, 0) # Yashil
                    status_text = f"[AI TINCH] Xavfsiz ({w}x{h} VGA) | {time.strftime('%H:%M:%S')}"
            else:
                time_since_motion = now - self.motion_last_seen if self.motion_last_seen > 0 else 999.0

                if self.siren_active:
                    if time_since_motion < self.SIREN_COOLDOWN_SEC:
                        remaining = self.SIREN_COOLDOWN_SEC - time_since_motion
                        self.siren_active = True
                        self.alert_active = True
                        self.alert_message = f"Hayvon ketdi. Sirena {remaining:.1f}s dan so'ng o'chadi..."
                        status_color = (0, 140, 255) # Amber
                        status_text = f"[AI KUZATUV] Hayvon ketdi | Sirena o'chishi: {remaining:.1f}s"
                    else:
                        self.siren_active = False
                        self.alert_active = False
                        self.motion_start_time = 0.0
                        self.motion_duration = 0.0
                        self.detected_objects = []
                        self.alert_message = "Tizim Tinch (Xavfsiz)"
                        status_color = (0, 140, 0) # Yashil
                        status_text = f"[AI TINCH] Xavfsiz ({w}x{h} VGA) | {time.strftime('%H:%M:%S')}"
                else:
                    if time_since_motion > 1.0:
                        self.motion_start_time = 0.0
                        self.motion_duration = 0.0
                        self.detected_objects = []
                        self.alert_active = False
                        self.alert_message = "Tizim Tinch (Xavfsiz)"
                    status_color = (0, 140, 0) # Yashil
                    status_text = f"[AI TINCH] Xavfsiz ({w}x{h} VGA) | {time.strftime('%H:%M:%S')}"

            # 6. Agar kadr 640 dan kichik bo'lsa 640x480 ga kengaytirish; 640 bo'lsa original saqlash
            out_w, out_h = w, h
            if w < 640:
                display_frame = cv2.resize(display_frame, (640, 480), interpolation=cv2.INTER_LANCZOS4)
                out_w, out_h = 640, 480

            # 7. Kadr tepasiga AI holat sarlavhasi (Banner) chizish
            banner_h = 28 if out_w >= 640 else 22
            font_scale = 0.52 if out_w >= 640 else 0.42
            cv2.rectangle(display_frame, (0, 0), (out_w, banner_h), status_color, -1)
            cv2.putText(display_frame, status_text, (8, 20 if out_w >= 640 else 16),
                        cv2.FONT_HERSHEY_SIMPLEX, font_scale, (255, 255, 255), 1, cv2.LINE_AA)

            # 8. Annotated kadrni JPEG ga kodlash (Yuqori sifat: Q85)
            ret, out_jpeg = cv2.imencode('.jpg', display_frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            if ret:
                return out_jpeg.tobytes(), self.alert_active, self.alert_message, self.detected_objects, self.siren_active
            return jpeg_bytes, self.alert_active, self.alert_message, self.detected_objects, self.siren_active

        except Exception as e:
            print(f"[AI ERROR] process_frame xatolik: {e}", flush=True)
            return jpeg_bytes, self.alert_active, self.alert_message, self.detected_objects, self.siren_active

detector = SmartFarmAIDetector()

class AIDetectionHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        if self.path == '/process_frame':
            content_length = int(self.headers.get('Content-Length', 0))
            if content_length <= 0:
                self.send_response(400)
                self.send_header('Connection', 'close')
                self.end_headers()
                return

            body = self.rfile.read(content_length)
            annotated_jpeg, is_alert, msg, objects, siren = detector.process_frame(body)

            self.send_response(200)
            self.send_header('Content-Type', 'image/jpeg')
            self.send_header('Content-Length', str(len(annotated_jpeg)))
            self.send_header('Connection', 'close')
            self.send_header('X-AI-Alert', '1' if is_alert else '0')
            self.send_header('X-AI-Siren', '1' if siren else '0')
            self.send_header('X-AI-Duration', f'{detector.motion_duration:.1f}')
            self.send_header('X-AI-Message', urllib.parse.quote(msg))
            self.send_header('X-AI-Objects', urllib.parse.quote(",".join(objects)))
            self.end_headers()
            self.wfile.write(annotated_jpeg)
            return

        self.send_response(404)
        self.send_header('Connection', 'close')
        self.end_headers()

    def do_GET(self):
        if self.path == '/status':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Connection', 'close')
            self.end_headers()
            import json
            data = {
                "alert_active": detector.alert_active,
                "siren_active": detector.siren_active,
                "alert_message": detector.alert_message,
                "detected_objects": detector.detected_objects,
                "motion_duration": detector.motion_duration,
                "confirm_threshold": detector.CONFIRM_THRESHOLD_SEC,
                "siren_cooldown": detector.SIREN_COOLDOWN_SEC
            }
            self.wfile.write(json.dumps(data).encode('utf-8'))
            return

        self.send_response(404)
        self.send_header('Connection', 'close')
        self.end_headers()

    def log_message(self, format, *args):
        return

def run(port=5001):
    server = ThreadingHTTPServer(('127.0.0.1', port), AIDetectionHandler)
    print(f"Smart Farm AI Animal Detector Service faol: http://127.0.0.1:{port}", flush=True)
    server.serve_forever()

if __name__ == '__main__':
    port = 5001
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    run(port)

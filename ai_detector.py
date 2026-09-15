import cv2
import numpy as np
import time
import threading

class SmartFarmDetector:
    def __init__(self):
        # MOG2 Background Subtractor for motion and intruder/animal detection
        self.bg_subtractor = cv2.createBackgroundSubtractorMOG2(history=400, varThreshold=30, detectShadows=True)

        self.last_frame = None
        self.annotated_frame = None
        self.lock = threading.Lock()
        
        # State
        self.alert_active = False
        self.alert_message = "Tizim Tinch (Xavfsiz)"
        self.detected_objects = []
        self.auto_siren = True
        self.siren_callback = None
        self.last_alert_time = 0

        # Temporal Motion Accumulator (4-5 soniyalik filtr)
        self.motion_start_time = 0.0
        self.motion_last_seen = 0.0
        self.motion_duration = 0.0
        self.CONFIRM_THRESHOLD_SEC = 4.5 # Kamida 4.5 soniya uzluksiz harakat bo'lgandagina xavf deb topiladi

        # Stream capture thread
        self.camera_url = None
        self.running = True
        self.stream_thread = None

    def start_stream(self, camera_url):
        self.camera_url = camera_url
        if self.stream_thread is None or not self.stream_thread.is_alive():
            self.stream_thread = threading.Thread(target=self._capture_loop, daemon=True)
            self.stream_thread.start()

    def _capture_loop(self):
        while self.running:
            if not self.camera_url:
                time.sleep(1)
                continue

            try:
                cap = cv2.VideoCapture(self.camera_url)
                if not cap.isOpened():
                    time.sleep(2)
                    continue

                while self.running and cap.isOpened():
                    ret, frame = cap.read()
                    if not ret or frame is None:
                        break

                    self.process_frame(frame)
                    time.sleep(0.03) # ~30 fps cap
                cap.release()
            except Exception as e:
                print(f"Stream error: {e}")
                time.sleep(2)

    def process_frame(self, frame):
        h, w = frame.shape[:2]
        if w > 640:
            scale = 640.0 / w
            frame = cv2.resize(frame, (640, int(h * scale)))
            h, w = frame.shape[:2]

        total_frame_area = w * h
        display_frame = frame.copy()
        detected = []
        raw_motion = False
        total_motion_area = 0
        now = time.time()

        # Apply MOG2 background subtractor
        fg_mask = self.bg_subtractor.apply(frame)
        
        # Morphological operations to filter noise and consolidate blobs
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 5))
        thresh = cv2.morphologyEx(fg_mask, cv2.MORPH_OPEN, kernel)
        thresh = cv2.morphologyEx(thresh, cv2.MORPH_DILATE, kernel)
        _, thresh = cv2.threshold(thresh, 180, 255, cv2.THRESH_BINARY)

        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        for c in contours:
            area = cv2.contourArea(c)
            total_motion_area += area

            # Kichik shovqinlarni va o't-o'lan tebranishini hisobga olmaslik (area > 2200)
            if area > 2200:
                (x, y, bw, bh) = cv2.boundingRect(c)
                
                # Kamera butun fon bo'yicha burilganda (pan qilganda) global harakatni inkor qilish
                if area > total_frame_area * 0.40:
                    continue

                raw_motion = True
                aspect_ratio = bh / float(bw)

                # Dastlabki sinflash
                if area > 9000 and aspect_ratio > 1.2:
                    label = "ODAM / SHAXS?"
                    color = (0, 0, 255) # Red
                    detected.append("Odam")
                elif area > 4500:
                    label = "YIRIK HAYVON?"
                    color = (0, 165, 255) # Orange
                    detected.append("Yirik Hayvon")
                else:
                    label = "HARAKAT?"
                    color = (0, 215, 255) # Yellow
                    detected.append("Harakatlanuvchi Jism")

                # Chiziq chizish
                cv2.rectangle(display_frame, (x, y), (x + bw, y + bh), color, 2)
                cv2.putText(display_frame, label, (x, max(20, y - 8)),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2)

        # Agar kadrning 50% dan ortig'i bir vaqtda qimirlasa, bu kamera burilishidir
        if total_motion_area > total_frame_area * 0.45:
            raw_motion = False

        # --- 4-5 SONIYALIK DAVOMIYLIK FILTRI (TEMPORAL PERSISTENCE FILTER) ---
        if raw_motion:
            self.motion_last_seen = now
            if self.motion_start_time == 0.0:
                self.motion_start_time = now
            self.motion_duration = now - self.motion_start_time
        else:
            # Agar 1.2 soniya davomida harakat bo'lmasa, taymer nollanadi
            if now - self.motion_last_seen > 1.2:
                self.motion_start_time = 0.0
                self.motion_duration = 0.0

        # Haqiqiy xavf faqat va faqat harakat 4.5 soniyadan ko'p davom etgandagina tasdiqlanadi!
        is_confirmed_alert = (self.motion_duration >= self.CONFIRM_THRESHOLD_SEC)

        with self.lock:
            self.alert_active = is_confirmed_alert
            self.detected_objects = list(set(detected))

            if is_confirmed_alert:
                det_str = ", ".join(self.detected_objects) if self.detected_objects else "Hayvon / Odam"
                self.alert_message = f"XAVF ANIQLANDI ({self.motion_duration:.1f}s): {det_str}!"
                
                # Sirena ishga tushirish (kamida 6 soniya interval bilan)
                if now - self.last_alert_time > 6 and self.auto_siren and self.siren_callback:
                    self.last_alert_time = now
                    try:
                        self.siren_callback(True)
                    except Exception as e:
                        print(f"Siren trigger error: {e}")
            elif self.motion_duration > 0.8:
                # Harakat bor, ammo hali 4-5 soniya to'lmadi (tekshiruv jarayoni)
                self.alert_message = f"Harakat tekshirilmoqda... ({self.motion_duration:.1f}s / {self.CONFIRM_THRESHOLD_SEC:.1f}s)"
            else:
                self.alert_message = "Tizim Tinch (Xavfsiz)"

            # Status banner overlay
            if is_confirmed_alert:
                status_color = (0, 0, 200) # Qizil
                status_text = f"XAVF TASDIQLANDI! ({self.motion_duration:.1f}s)"
            elif self.motion_duration > 0.8:
                status_color = (0, 140, 255) # To'q sariq (Tekshiruv)
                status_text = f"TAHLIL QILINMOQDA ({self.motion_duration:.1f}s / {self.CONFIRM_THRESHOLD_SEC:.1f}s)"
            else:
                status_color = (0, 160, 0) # Yashil
                status_text = "XAVFSIZLIK: NORMAL (Tinch)"

            cv2.rectangle(display_frame, (0, 0), (display_frame.shape[1], 36), status_color, -1)
            cv2.putText(display_frame, f"[AI NAZORAT] {status_text} | {time.strftime('%H:%M:%S')}",
                        (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

            self.annotated_frame = display_frame

    def get_jpeg_frame(self):
        with self.lock:
            if self.annotated_frame is None:
                placeholder = np.zeros((480, 640, 3), dtype=np.uint8)
                placeholder[:] = (20, 26, 32)

                for y in range(0, 480, 40):
                    cv2.line(placeholder, (0, y), (640, y), (30, 38, 46), 1)
                for x in range(0, 640, 40):
                    cv2.line(placeholder, (x, 0), (x, 480), (30, 38, 46), 1)

                cv2.putText(placeholder, "AQLLI FERMA - KAMERA KUTILMOQDA...", (60, 200),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 215, 255), 2)
                cv2.putText(placeholder, f"Kamera Manzili: {self.camera_url or '10.24.95.110:81/stream'}", (60, 240),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.55, (160, 160, 160), 1)
                cv2.putText(placeholder, f"Server Vaqti : {time.strftime('%Y-%m-%d %H:%M:%S')}", (60, 280),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (100, 200, 100), 1)
                cv2.putText(placeholder, "Filtr: 4.5 soniyalik harakat tasdiqlanishi", (60, 320),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (120, 120, 120), 1)
                frame = placeholder
            else:
                frame = self.annotated_frame

            ret, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            if ret:
                return jpeg.tobytes()
            return None

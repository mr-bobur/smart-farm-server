const http = require('http');
const jpeg = require('jpeg-js');

class SmartFarmDetector {
  constructor() {
    this.cameraUrl = null;
    this.running = true;
    this.activeFeedClients = new Set();
    
    // Status & AI State
    this.alertActive = false;
    this.alertMessage = "Tizim Tinch (Xavfsiz)";
    this.detectedObjects = [];
    this.autoSiren = true;
    this.sirenCallback = null;
    this.lastAlertTime = 0;
    
    // 4.5 soniyalik harakat tasdiqlash filtri
    this.motionStartTime = 0;
    this.motionLastSeen = 0;
    this.motionDuration = 0;
    this.CONFIRM_THRESHOLD_SEC = 4.5;

    // Oxirgi kadr va tahlil panjarasi (Grid)
    this.lastFrameBuffer = null;
    this.lastFrameTime = 0;
    this.lastGrid = null;
    this.lastAnalysisTime = 0;

    // HTTP Stream Client
    this.streamReq = null;
    this.reconnectTimeout = null;

    // Zaxira (Placeholder) kadrini tayyorlash
    this.placeholderBuffer = this._generatePlaceholderFrame();
    this.lastFrameBuffer = this.placeholderBuffer;

    // Agar kamera kadr yubormayotgan bo'lsa brauzerlarga standby kadrini uzatib turish
    setInterval(() => {
      const now = Date.now();
      if (!this.streamReq && (now - this.lastFrameTime > 4000)) {
        this._broadcastFrame(this.placeholderBuffer);
      }
    }, 1500);
  }

  pushFrame(jpegBuffer) {
    if (!jpegBuffer || jpegBuffer.length < 100) return false;
    this.lastFrameTime = Date.now();
    // Agar pull rejimi ishlab turgan bo'lsa, uni to'xtatish (chunki kadrlar push qilinmoqda)
    if (this.streamReq || this.reconnectTimeout) {
      this.stopStream();
      this.cameraUrl = null;
    }
    this._handleNewFrame(jpegBuffer);
    return true;
  }

  startStream(url) {
    if (!url) return;
    if (this.cameraUrl === url && this.streamReq) return;

    this.cameraUrl = url;
    this.stopStream();

    console.log(`[STREAM] Kameraga ulanish boshlandi: ${url}`);
    this._connectToMjpeg(url);
  }

  stopStream() {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
      this.reconnectTimeout = null;
    }
    if (this.streamReq) {
      try {
        this.streamReq.destroy();
      } catch (e) {}
      this.streamReq = null;
    }
  }

  _connectToMjpeg(urlStr) {
    try {
      const url = new URL(urlStr);
      const options = {
        hostname: url.hostname,
        port: url.port || 80,
        path: url.pathname + url.search,
        method: 'GET',
        headers: { 'Connection': 'keep-alive' },
        timeout: 8000
      };

      let buffer = Buffer.alloc(0);
      const SOI = Buffer.from([0xff, 0xd8]); // Start of JPEG
      const EOI = Buffer.from([0xff, 0xd9]); // End of JPEG

      this.streamReq = http.request(options, (res) => {
        if (res.statusCode !== 200) {
          console.warn(`[STREAM] Kamera noto'g'ri status qaytardi: ${res.statusCode}`);
          this._scheduleReconnect();
          return;
        }

        console.log(`[STREAM OK] Kamera video oqimi ulandi (${urlStr})`);

        res.on('data', (chunk) => {
          buffer = Buffer.concat([buffer, chunk]);

          while (true) {
            const startIndex = buffer.indexOf(SOI);
            if (startIndex === -1) {
              // Agar SOI topilmasa, eski ma'lumotni tozalash
              if (buffer.length > 500000) buffer = Buffer.alloc(0);
              break;
            }

            const endIndex = buffer.indexOf(EOI, startIndex + 2);
            if (endIndex === -1) {
              // Kadr hali to'liq yuklanmadi, keyingi chunk'ni kutamiz
              if (startIndex > 0) {
                buffer = buffer.subarray(startIndex);
              }
              break;
            }

            // To'liq bitta JPEG kadri ajratildi
            const frame = buffer.subarray(startIndex, endIndex + 2);
            buffer = buffer.subarray(endIndex + 2);

            this._handleNewFrame(frame);
          }
        });

        res.on('end', () => {
          console.warn('[STREAM] Kamera aloqasi uzildi.');
          this._scheduleReconnect();
        });

        res.on('error', (err) => {
          console.warn(`[STREAM XATO] ${err.message}`);
          this._scheduleReconnect();
        });
      });

      this.streamReq.on('timeout', () => {
        console.warn('[STREAM] Kamera ulanish vaqti tugadi (timeout).');
        if (this.streamReq) this.streamReq.destroy();
      });

      this.streamReq.on('error', (err) => {
        console.warn(`[STREAM] Ulanishda xatolik: ${err.message}`);
        this._scheduleReconnect();
      });

      this.streamReq.end();
    } catch (err) {
      console.warn(`[STREAM URL XATOSI] ${err.message}`);
      this._scheduleReconnect();
    }
  }

  _scheduleReconnect() {
    this.stopStream();
    if (this.cameraUrl && !this.reconnectTimeout) {
      this.reconnectTimeout = setTimeout(() => {
        if (this.cameraUrl) this._connectToMjpeg(this.cameraUrl);
      }, 3500);
    }
  }

  _handleNewFrame(frameBuffer) {
    this.lastFrameBuffer = frameBuffer;
    this.lastFrameTime = Date.now();
    this._broadcastFrame(frameBuffer);

    // Harakat tahlili (~8 fps da bir marta)
    const now = Date.now();
    if (now - this.lastAnalysisTime >= 120) {
      this.lastAnalysisTime = now;
      this._analyzeMotion(frameBuffer);
    }
  }

  _analyzeMotion(jpegBuffer) {
    try {
      // JPEG kadrini dekodlash (useTArray tezkor o'qish uchun)
      const decoded = jpeg.decode(jpegBuffer, { useTArray: true, formatAsRGBA: false });
      const { width, height, data } = decoded;

      // 32x24 o'lchamli past aniqlikdagi yorug'lik (luminance) panjarasiga siqish
      const cols = 32;
      const rows = 24;
      const currentGrid = new Uint8Array(cols * rows);

      const stepX = Math.floor(width / cols);
      const stepY = Math.floor(height / rows);

      for (let r = 0; r < rows; r++) {
        for (let c = 0; c < cols; c++) {
          const pxX = c * stepX + Math.floor(stepX / 2);
          const pxY = r * stepY + Math.floor(stepY / 2);
          const idx = (pxY * width + pxX) * 3;
          // Luminance = 0.299*R + 0.587*G + 0.114*B
          const lum = Math.floor(data[idx] * 0.299 + data[idx + 1] * 0.587 + data[idx + 2] * 0.114);
          currentGrid[r * cols + c] = lum;
        }
      }

      if (!this.lastGrid) {
        this.lastGrid = currentGrid;
        return;
      }

      // Oldingi kadr bilan farqni solishtirish
      let diffCells = 0;
      const totalCells = cols * rows; // 768

      for (let i = 0; i < totalCells; i++) {
        if (Math.abs(currentGrid[i] - this.lastGrid[i]) > 26) {
          diffCells++;
        }
      }
      this.lastGrid = currentGrid;

      // Global harakat (masalan, kamera burilganda butun fon qimirlasa) -> filtrlaymiz
      const diffRatio = diffCells / totalCells;
      let rawMotion = false;

      if (diffRatio >= 0.04 && diffRatio <= 0.40) {
        rawMotion = true; // Lokal obyekt / hayvon harakati!
      }

      // --- 4.5 SONIYALIK DAVOMIYLIK FILTRI ---
      const now = Date.now();
      if (rawMotion) {
        this.motionLastSeen = now;
        if (this.motionStartTime === 0) {
          this.motionStartTime = now;
        }
        this.motionDuration = (now - this.motionStartTime) / 1000;
      } else {
        // Agar 1.2 soniya davomida harakat bo'lmasa, taymer qaytariladi
        if (now - this.motionLastSeen > 1200) {
          this.motionStartTime = 0;
          this.motionDuration = 0;
        }
      }

      // Tasdiqlangan xavf holati
      const isConfirmedAlert = (this.motionDuration >= this.CONFIRM_THRESHOLD_SEC);
      this.alertActive = isConfirmedAlert;

      if (isConfirmedAlert) {
        this.alertMessage = `XAVF TASDIQLANDI (${this.motionDuration.toFixed(1)}s): Harakat / Begona Jism!`;
        this.detectedObjects = ["Harakatlanuvchi Obyekt / Hayvon"];

        // Avtomatik sirena callback (kamida 6 soniya interval bilan)
        if (this.autoSiren && this.sirenCallback && (now - this.lastAlertTime > 6000)) {
          this.lastAlertTime = now;
          this.sirenCallback(true);
        }
      } else if (this.motionDuration > 0.8) {
        this.alertMessage = `Harakat tahlil qilinmoqda... (${this.motionDuration.toFixed(1)}s / ${this.CONFIRM_THRESHOLD_SEC.toFixed(1)}s)`;
        this.detectedObjects = [];
      } else {
        this.alertMessage = "Tizim Tinch (Xavfsiz)";
        this.detectedObjects = [];
      }

    } catch (e) {
      // Dekodlashda xato bo'lsa (chala kadr) chetlab o'tish
    }
  }

  _broadcastFrame(frameBuffer) {
    if (this.activeFeedClients.size === 0) return;

    const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${frameBuffer.length}\r\n\r\n`;
    const footer = `\r\n`;
    const chunk = Buffer.concat([Buffer.from(header), frameBuffer, Buffer.from(footer)]);

    for (const res of this.activeFeedClients) {
      try {
        res.write(chunk);
      } catch (e) {
        this.activeFeedClients.delete(res);
      }
    }
  }

  handleFeedRequest(req, res) {
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
      'Cache-Control': 'no-cache, no-store, must-revalidate',
      'Connection': 'close',
      'Pragma': 'no-cache'
    });

    this.activeFeedClients.add(res);

    // Ulanishi bilan oxirgi kadrni darhol jo'natish
    if (this.lastFrameBuffer) {
      const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${this.lastFrameBuffer.length}\r\n\r\n`;
      res.write(Buffer.concat([Buffer.from(header), this.lastFrameBuffer, Buffer.from('\r\n')]));
    }

    req.on('close', () => {
      this.activeFeedClients.delete(res);
    });
  }

  _generatePlaceholderFrame() {
    const width = 640;
    const height = 480;
    const buffer = Buffer.alloc(width * height * 4);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const offset = (y * width + x) * 4;
        const isGrid = (y % 40 === 0) || (x % 40 === 0);
        buffer[offset] = isGrid ? 35 : 18;     // R
        buffer[offset + 1] = isGrid ? 45 : 24; // G
        buffer[offset + 2] = isGrid ? 55 : 32; // B
        buffer[offset + 3] = 255;              // A
      }
    }

    const rawImageData = { data: buffer, width, height };
    return jpeg.encode(rawImageData, 65).data;
  }
}

module.exports = SmartFarmDetector;

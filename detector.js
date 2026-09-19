const http = require('http');
const jpeg = require('jpeg-js');

const aiAgent = new http.Agent({
  keepAlive: true,
  maxSockets: 5,
  keepAliveMsecs: 10000
});

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
    this.onStatusChange = null;
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
    this.lastFrameTime = Date.now();

    // Agar Python AI hozir boshqa kadrni tahlil qilayotgan bo'lsa, navbat kutmaymiz!
    // Brauzerlarga uzluksiz oqimni beramiz (video hech qachon to'xtab qolmasligi uchun)
    if (this.aiProcessing) {
      this._broadcastFrame(this.lastFrameBuffer || frameBuffer);
      return;
    }

    this.aiProcessing = true;
    this._sendToAiService(frameBuffer);
  }

  _sendToAiService(frameBuffer) {
    const req = http.request({
      hostname: '127.0.0.1',
      port: 5001,
      path: '/process_frame',
      method: 'POST',
      agent: false,
      headers: {
        'Content-Type': 'image/jpeg',
        'Content-Length': frameBuffer.length,
        'Connection': 'close'
      },
      timeout: 600
    }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => {
        this.aiProcessing = false;
        if (res.statusCode === 200 && chunks.length > 0) {
          const annotatedBuffer = Buffer.concat(chunks);
          this.lastFrameBuffer = annotatedBuffer;
          this._broadcastFrame(annotatedBuffer);

          const isAlert = res.headers['x-ai-alert'] === '1';
          const isSiren = res.headers['x-ai-siren'] === '1';
          const msg = decodeURIComponent(res.headers['x-ai-message'] || '');
          const objStr = decodeURIComponent(res.headers['x-ai-objects'] || '');
          const objects = objStr ? objStr.split(',') : [];
          const duration = parseFloat(res.headers['x-ai-duration'] || '0');

          this.alertActive = isAlert;
          this.alertMessage = msg;
          this.detectedObjects = objects;

          if (this.onStatusChange) {
            this.onStatusChange({
              alertActive: isAlert,
              alertMessage: msg,
              detectedObjects: objects,
              motionDuration: duration,
              sirenActive: isSiren
            });
          }

          if (this.autoSiren && this.sirenCallback) {
            this.sirenCallback(isSiren);
          }
        } else {
          this.lastFrameBuffer = frameBuffer;
          this._broadcastFrame(frameBuffer);
        }
      });
    });

    req.on('error', () => {
      this.aiProcessing = false;
      this.lastFrameBuffer = frameBuffer;
      this._broadcastFrame(frameBuffer);
    });

    req.on('timeout', () => {
      this.aiProcessing = false;
      try { req.destroy(); } catch (e) {}
      this.lastFrameBuffer = frameBuffer;
      this._broadcastFrame(frameBuffer);
    });

    req.write(frameBuffer);
    req.end();
  }

  _broadcastFrame(frameBuffer) {
    if (this.activeFeedClients.size === 0) return;

    const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${frameBuffer.length}\r\n\r\n`;
    const footer = `\r\n`;
    const chunk = Buffer.concat([Buffer.from(header), frameBuffer, Buffer.from(footer)]);

    for (const res of this.activeFeedClients) {
      try {
        if (!res.writableEnded && !res.destroyed) {
          res.write(chunk);
        } else {
          this.activeFeedClients.delete(res);
        }
      } catch (e) {
        this.activeFeedClients.delete(res);
      }
    }
  }

  handleFeedRequest(req, res) {
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
      'Cache-Control': 'no-cache, no-store, must-revalidate, pre-check=0, post-check=0, max-age=0',
      'Pragma': 'no-cache',
      'Expires': '0',
      'Connection': 'keep-alive'
    });

    this.activeFeedClients.add(res);

    // Ulanishi bilan oxirgi kadrni darhol jo'natish
    if (this.lastFrameBuffer) {
      const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${this.lastFrameBuffer.length}\r\n\r\n`;
      try {
        res.write(Buffer.concat([Buffer.from(header), this.lastFrameBuffer, Buffer.from('\r\n')]));
      } catch (e) {
        this.activeFeedClients.delete(res);
        return;
      }
    }

    const cleanup = () => {
      this.activeFeedClients.delete(res);
    };

    req.on('close', cleanup);
    res.on('close', cleanup);
    res.on('finish', cleanup);
    res.on('error', cleanup);
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

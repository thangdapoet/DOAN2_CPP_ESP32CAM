// ESP32_CAM_with_album_and_sorted_events.ino
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <FS.h>

// ==================== CONFIGURATION ====================
// Camera Pinout (giữ nguyên từ bạn)
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM    5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
#define LED_GPIO_NUM   4

// WiFi Settings
const char* ssid = "THANGDAPOET";
const char* password = "15112004";

// Server Setup
WebServer server(80);
WiFiServer streamServer(81);

// Camera Settings
bool flashState = false;
int brightness = 0;      // Range: -2 to 2
int saturation = 0;      // Range: -2 to 2
const int SENSOR_BRIGHTNESS = 1;

// Stream Settings
WiFiClient streamClient;
bool streamActive = false;
unsigned long lastFrame = 0;
const unsigned long FRAME_INTERVAL = 80; // ~12.5 FPS

// ---------------- Photo storage (circular buffer) ----------------
const int MAX_PHOTOS = 10;
unsigned long photoSeqs[MAX_PHOTOS]; // store seq number for each stored file
int photoCount = 0;                  // number stored (<= MAX_PHOTOS)
int photoStart = 0;                  // index of oldest (circular buffer)
unsigned long lastEventSeq = 0;
String lastEventStatus = "";
String lastEventId = "";

// Helper: build path for seq
String photoPathForSeq(unsigned long seq) {
  return String("/photo_") + String(seq) + ".jpg";
}

void addCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}
// Add a new photo entry (save seq into circular buffer); delete overwritten file if any
void addPhotoSeq(unsigned long seq) {
  // if not full, append at (photoStart + photoCount)
  if (photoCount < MAX_PHOTOS) {
    int idx = (photoStart + photoCount) % MAX_PHOTOS;
    photoSeqs[idx] = seq;
    photoCount++;
  } else {
    // overwrite oldest at photoStart -> delete corresponding file, then replace
    unsigned long oldSeq = photoSeqs[photoStart];
    String oldPath = photoPathForSeq(oldSeq);
    if (SPIFFS.exists(oldPath)) SPIFFS.remove(oldPath);
    photoSeqs[photoStart] = seq;
    photoStart = (photoStart + 1) % MAX_PHOTOS; // move start to next oldest
  }
}

// Produce JSON array of photo seqs ordered from oldest -> newest
String buildPhotoListJson() {
  String json = "[";
  for (int i = 0; i < photoCount; ++i) {
    int idx = (photoStart + i) % MAX_PHOTOS;
    json += String(photoSeqs[idx]);
    if (i < photoCount - 1) json += ",";
  }
  json += "]";
  return json;
}

// ==================== HTML INTERFACE (kept stream + controls) ====================
// We modify HTML to: (1) eventLog append at bottom (ascending order), (2) add album area collapsible and show thumbnails
const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>ESP32-CAM Control Panel</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { background: #000; color: #fff; font-family: sans-serif; margin: 0; padding: 20px 0; 
               display: flex; flex-direction: column; align-items: center; justify-content: flex-start; 
               gap: 12px; min-height: 100vh; }
        img { width: 320px; max-width: 90vw; border-radius: 8px; box-shadow: 0 0 12px rgba(255,255,255,0.05); }
        button { padding: 10px 18px; border: none; border-radius: 6px; background: #1e88e5; 
                 color: #fff; font-size: 16px; cursor: pointer; }
        button.off { background: #555; }
        .controls { display: flex; flex-direction: column; gap: 12px; align-items: center; width: 320px; max-width: 90vw; }
        .control-group { background: #1a1a1a; padding: 15px; border-radius: 8px; width: 100%; 
                         display: flex; flex-direction: column; gap: 8px; }
        .control-row { display: flex; justify-content: space-between; align-items: center; gap: 10px; }
        .slider-container { display: flex; align-items: center; gap: 8px; flex: 1; }
        .slider { flex: 1; height: 6px; background: #555; border-radius: 3px; outline: none; }
        .slider::-webkit-slider-thumb { width: 18px; height: 18px; background: #1e88e5; border-radius: 50%; cursor: pointer; }
        .value-display { min-width: 30px; text-align: center; font-size: 14px; color: #ccc; font-weight: bold; }
        .status { color: #ccc; font-size: 14px; }
        .reset-btn { padding: 6px 12px; font-size: 12px; background: #e53935; margin-left: 10px; }
        .control-label { font-size: 14px; color: #fff; min-width: 80px; }
        .range-labels { display: flex; justify-content: space-between; width: 100%; font-size: 11px; color: #666; margin-top: -5px; }
        .plus-minus-btn { width: 32px; height: 32px; padding: 0; border-radius: 50%; background: #333; 
                          color: #fff; font-size: 16px; font-weight: bold; cursor: pointer; border: 1px solid #555; 
                          display: flex; align-items: center; justify-content: center; }
        .plus-minus-btn:hover { background: #444; }
        .plus-minus-btn:active { background: #555; }

        /* event log: ascending order (oldest top, newest bottom) */
        #eventLog { width: 320px; max-width: 90vw; background: rgba(255,255,255,0.03); padding: 10px; border-radius: 8px; 
                    font-size: 14px; color: #cfcfcf; min-height: 64px; max-height: 220px; overflow:auto; display:flex; flex-direction:column; gap:6px; }
        .eventItem { padding: 4px 0; border-bottom: 1px solid rgba(255,255,255,0.03); }

        /* album (collapsible) */
        #albumHeader { width: 320px; max-width: 90vw; cursor: pointer; color:#fff; background: rgba(255,255,255,0.02); padding:10px; border-radius:8px; text-align:center; }
        #albumContent { width: 320px; max-width: 90vw; background: rgba(255,255,255,0.02); padding:8px; border-radius:8px; display:none; gap:8px; flex-direction:column; }
        .thumbRow { display:flex; flex-wrap:wrap; gap:8px; justify-content:flex-start; }
        .thumb { width:100px; height:75px; object-fit:cover; border-radius:6px; border:1px solid rgba(255,255,255,0.06); }
        .thumbItem { display:flex; flex-direction:column; align-items:center; gap:4px; width:100px; }
        .thumbTime { font-size:12px; color:#ccc; text-align:center; }
    </style>
</head>
<body>
    <img id="stream" src="http://%HOST%:81/stream">
    
    <div class="controls">
        <!-- Flash Control -->
        <div class="control-group">
            <div class="control-row">
                <button id="flashBtn">Loading...</button>
                <span class="status" id="status">Checking flash...</span>
            </div>
        </div>
        
        <!-- Camera Settings -->
        <div class="control-group">
            <!-- Brightness Control (unchanged) -->
            <div class="control-row">
                <span class="control-label">Độ sáng:</span>
                <div class="slider-container">
                    <button class="plus-minus-btn" onclick="changeBrightness(-1)">-</button>
                    <input type="range" id="brightnessSlider" class="slider" min="-2" max="2" value="0" step="1">
                    <button class="plus-minus-btn" onclick="changeBrightness(1)">+</button>
                    <span id="brightnessValue" class="value-display">0</span>
                </div>
                <button class="reset-btn" onclick="resetBrightness()">Reset</button>
            </div>
            <div class="range-labels">
                <span>-2 (Tối)</span>
                <span>0 (Mặc định)</span>
                <span>+2 (Sáng)</span>
            </div>
            
            <!-- Saturation Control -->
            <div class="control-row">
                <span class="control-label">Bão hòa:</span>
                <div class="slider-container">
                    <button class="plus-minus-btn" onclick="changeSaturation(-1)">-</button>
                    <input type="range" id="saturationSlider" class="slider" min="-2" max="2" value="0" step="1">
                    <button class="plus-minus-btn" onclick="changeSaturation(1)">+</button>
                    <span id="saturationValue" class="value-display">0</span>
                </div>
                <button class="reset-btn" onclick="resetSaturation()">Reset</button>
            </div>
            <div class="range-labels">
                <span>-2 (Nhạt)</span>
                <span>0 (Mặc định)</span>
                <span>+2 (Đậm)</span>
            </div>
        </div>

        <!-- Event log (ascending) -->
        <div id="eventLog" aria-live="polite"></div>

        <!-- Album: collapsed by default -->
        <div id="albumHeader">Album ảnh (nhấn để mở)</div>
        <div id="albumContent">
            <div class="thumbRow" id="thumbRow"></div>
            <div style="font-size:12px;color:#aaa;text-align:center;">(hiển thị tối đa 10 ảnh gần nhất)</div>
        </div>
    </div>

    <script>
        // DOM Elements
        const host = location.hostname;
        const brightnessSlider = document.getElementById('brightnessSlider');
        const saturationSlider = document.getElementById('saturationSlider');
        const brightnessValue = document.getElementById('brightnessValue');
        const saturationValue = document.getElementById('saturationValue');
        const flashBtn = document.getElementById('flashBtn');
        const status = document.getElementById('status');
        const streamImg = document.getElementById('stream');
        const eventLog = document.getElementById('eventLog');

        const albumHeader = document.getElementById('albumHeader');
        const albumContent = document.getElementById('albumContent');
        const thumbRow = document.getElementById('thumbRow');

        // Initialize
        streamImg.src = `http://${host}:81/stream`;

        // Local map for seq -> timestamp (client time when event was received)
        const timestampMap = {}; // { seq: "HH:MM:SS" }

        // State Management
        async function getState() {
            try {
                const response = await fetch('/camera_state');
                const data = await response.json();
                
                updateUI(data.flashState);
                brightnessSlider.value = data.brightness;
                saturationSlider.value = data.saturation;
                brightnessValue.textContent = data.brightness;
                saturationValue.textContent = data.saturation;
                status.textContent = 'Connected';
            } catch (error) {
                status.textContent = 'Reconnecting...';
                setTimeout(getState, 1000);
            }
        }

        function updateUI(flashOn) {
            flashBtn.textContent = flashOn ? 'Tắt flash' : 'Bật flash';
            flashBtn.classList.toggle('off', !flashOn);
        }

        // Controls (same as before)
        async function toggleFlash() {
            flashBtn.disabled = true;
            flashBtn.textContent = 'Processing...';
            await fetch('/toggle_flash', { method: 'POST' });
            flashBtn.disabled = false;
            getState();
        }

        function changeBrightness(delta) {
            const newValue = Math.max(-2, Math.min(2, parseInt(brightnessSlider.value) + delta));
            brightnessSlider.value = newValue;
            brightnessValue.textContent = newValue;
            updateBrightness(newValue);
        }
        brightnessSlider.oninput = function() {
            brightnessValue.textContent = this.value;
            updateBrightness(this.value);
        };
        let brightnessTimeout;
        async function updateBrightness(value) {
            clearTimeout(brightnessTimeout);
            brightnessTimeout = setTimeout(async () => {
                await fetch(`/set_brightness?value=${value}`);
            }, 200);
        }

        function changeSaturation(delta) {
            const newValue = Math.max(-2, Math.min(2, parseInt(saturationSlider.value) + delta));
            saturationSlider.value = newValue;
            saturationValue.textContent = newValue;
            updateSaturation(newValue);
        }
        saturationSlider.oninput = function() {
            saturationValue.textContent = this.value;
            updateSaturation(this.value);
        };
        let saturationTimeout;
        async function updateSaturation(value) {
            clearTimeout(saturationTimeout);
            saturationTimeout = setTimeout(async () => {
                await fetch(`/set_saturation?value=${value}`);
            }, 200);
        }

        async function resetBrightness() {
            brightnessSlider.value = 0;
            brightnessValue.textContent = '0';
            await fetch('/set_brightness?value=0');
        }
        async function resetSaturation() {
            saturationSlider.value = 0;
            saturationValue.textContent = '0';
            await fetch('/set_saturation?value=0');
        }

        flashBtn.onclick = toggleFlash;
        document.querySelectorAll('.plus-minus-btn').forEach(btn => {
            btn.addEventListener('mousedown', function(e) {
                const delta = this.textContent === '+' ? 1 : -1;
                if (this.closest('.slider-container').contains(brightnessSlider)) startBrightnessChange(delta);
                else startSaturationChange(delta);
            });
            btn.addEventListener('touchstart', function(e) {
                const delta = this.textContent === '+' ? 1 : -1;
                if (this.closest('.slider-container').contains(brightnessSlider)) startBrightnessChange(delta);
                else startSaturationChange(delta);
                e.preventDefault();
            });
        });
        let brightnessInterval, saturationInterval;
        function startBrightnessChange(delta) {
            changeBrightness(delta);
            brightnessInterval = setInterval(() => changeBrightness(delta), 100);
        }
        function startSaturationChange(delta) {
            changeSaturation(delta);
            saturationInterval = setInterval(() => changeSaturation(delta), 100);
        }
        function stopChange() {
            clearInterval(brightnessInterval);
            clearInterval(saturationInterval);
        }
        document.addEventListener('mouseup', stopChange);
        document.addEventListener('touchend', stopChange);

        // ---------------- Event polling & ordering (ascending) ----------------
        let lastSeq = 0;
        async function pollEvents() {
            try {
                const res = await fetch('/last_event');
                if (res.ok) {
                    const j = await res.json();
                    if (j.seq && j.seq !== lastSeq) {
                        // if seq increased by more than 1, it's okay - we just display received events in order received
                        lastSeq = j.seq;
                        const now = new Date();
                        const hh = String(now.getHours()).padStart(2,'0');
                        const mm = String(now.getMinutes()).padStart(2,'0');
                        const ss = String(now.getSeconds()).padStart(2,'0');
                        const timestr = `${hh}:${mm}:${ss}`;

                        // save timestamp for this seq (used later to annotate thumbnails)
                        timestampMap[j.seq] = timestr;

                        let line = "";
                        if (j.status === "ok" && j.id) {
                            line = `${timestr}: ${j.id} đã vào`;
                        } else if (j.status === "bad") {
                            line = `${timestr}: co nguoi la`;
                        } else {
                            line = `${timestr}: notify(${j.status})`;
                        }

                        // append at bottom to preserve ascending order (oldest -> newest)
                        const div = document.createElement('div');
                        div.className = 'eventItem';
                        div.textContent = line;
                        eventLog.appendChild(div);
                        // auto scroll to bottom to show newest
                        eventLog.scrollTop = eventLog.scrollHeight;

                        // when new event arrives, refresh album thumbnails (so newly captured photo appears)
                        // but only if album expanded
                        if (albumContent.style.display === 'flex') {
                            refreshAlbum();
                        }
                    }
                }
            } catch (e) {
                // ignore
            } finally {
                setTimeout(pollEvents, 700);
            }
        }

        // ---------------- Album logic ----------------
        albumHeader.addEventListener('click', () => {
            if (albumContent.style.display === 'flex') {
                albumContent.style.display = 'none';
                albumHeader.textContent = 'Album ảnh (nhấn để mở)';
            } else {
                albumContent.style.display = 'flex';
                albumContent.style.flexDirection = 'column';
                albumHeader.textContent = 'Album ảnh (nhấn để thu gọn)';
                refreshAlbum();
            }
        });

        // fetch /photos_list and render thumbnails oldest->newest (ascending)
        async function refreshAlbum() {
            try {
                const res = await fetch('/photos_list');
                if (!res.ok) return;
                const arr = await res.json(); // array of seq numbers [oldest,...,newest]
                // clear
                thumbRow.innerHTML = '';
                for (let i = 0; i < arr.length; ++i) {
                    const seq = arr[i];
                    const img = document.createElement('img');
                    img.className = 'thumb';
                    img.src = `/photo?seq=${seq}&_=${Date.now()}`; // cache buster
                    const container = document.createElement('div');
                    container.className = 'thumbItem';
                    const timeStr = timestampMap[seq] || '--:--:--';
                    const caption = document.createElement('div');
                    caption.className = 'thumbTime';
                    caption.textContent = timeStr;
                    container.appendChild(img);
                    container.appendChild(caption);
                    thumbRow.appendChild(container);
                }
            } catch (e) {
                // ignore
            }
        }

        // Initialize
        getState();
        pollEvents();
    </script>
</body>
</html>
)rawliteral";

// ==================== HTTP HANDLERS ====================
void handleRoot() {
    addCors();
    String page = FPSTR(html_page);
    page.replace("%HOST%", WiFi.localIP().toString());
    server.send(200, "text/html", page);
}

void handleCameraState() {
    addCors();
    String json = String("{\"flashState\":") + (flashState ? "true" : "false") + 
                  ",\"brightness\":" + String(brightness) + 
                  ",\"saturation\":" + String(saturation) + "}";
    server.send(200, "application/json", json);
}

void handleToggleFlash() {
    addCors();
    flashState = !flashState;
    digitalWrite(LED_GPIO_NUM, flashState ? HIGH : LOW);
    Serial.printf("Flash -> %s\n", flashState ? "ON" : "OFF");
    handleCameraState();
}

void handleSetBrightness() {
    addCors();
    if (server.hasArg("value")) {
        brightness = server.arg("value").toInt();
        sensor_t* s = esp_camera_sensor_get();
        s->set_brightness(s, brightness);
        s->set_contrast(s, brightness);
        s->set_gain_ctrl(s, 1);
        Serial.printf("Brightness set to: %d\n", brightness);
    }
    handleCameraState();
}

void handleSetSaturation() {
    addCors();
    if (server.hasArg("value")) {
        saturation = server.arg("value").toInt();
        sensor_t* s = esp_camera_sensor_get();
        s->set_saturation(s, saturation);
        Serial.printf("Saturation set to: %d\n", saturation);
    }
    handleCameraState();
}

// notify handler: read status & optional id, print Serial, store lastEvent, capture photo and save to SPIFFS
void handleNotify() {
    addCors();
    String status = "";
    String id = "";
    if (server.hasArg("status")) status = server.arg("status");
    if (server.hasArg("id")) id = server.arg("id");
    status.toLowerCase();

    if (status == "ok") {
        Serial.println("chao mung");
    } else if (status == "bad") {
        Serial.println("nguoi la");
    } else {
        Serial.printf("notify received (status=%s id=%s)\n", status.c_str(), id.c_str());
    }

    // increment seq and store event
    noInterrupts();
    lastEventSeq++;
    lastEventStatus = status;
    lastEventId = id;
    unsigned long seq = lastEventSeq;
    interrupts();

    // capture a frame synchronously (JPEG) and save to SPIFFS as /photo_<seq>.jpg
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("capture failed");
    } else {
        String path = photoPathForSeq(seq);
        File file = SPIFFS.open(path, FILE_WRITE);
        if (!file) {
            Serial.println("failed to open file to write");
        } else {
            file.write(fb->buf, fb->len);
            file.close();
            Serial.printf("Saved photo for seq %lu -> %s\n", seq, path.c_str());
            // add to circular buffer
            addPhotoSeq(seq);
        }
        esp_camera_fb_return(fb);
    }

    server.send(200, "text/plain", "OK");
}

// return last event info as JSON: { seq: N, status: "ok"/"bad", id: "..." }
void handleLastEvent() {
    addCors();
    unsigned long seq;
    String status;
    String id;
    noInterrupts();
    seq = lastEventSeq;
    status = lastEventStatus;
    id = lastEventId;
    interrupts();

    String json = "{";
    json += "\"seq\":" + String(seq) + ",";
    json += "\"status\":\"" + status + "\",";
    json += "\"id\":\"" + id + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// return photo list JSON array [seq_oldest, ..., seq_newest]
void handlePhotosList() {
    addCors();
    String listJson = buildPhotoListJson();
    server.send(200, "application/json", listJson);
}

// serve photo file for given seq: /photo?seq=<N>
void handlePhoto() {
    addCors();
    if (!server.hasArg("seq")) {
        server.send(400, "text/plain", "Missing seq");
        return;
    }
    unsigned long seq = server.arg("seq").toInt();
    String path = photoPathForSeq(seq);
    if (!SPIFFS.exists(path)) {
        server.send(404, "text/plain", "Not found");
        return;
    }
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        server.send(500, "text/plain", "Open fail");
        return;
    }
    server.streamFile(f, "image/jpeg");
    f.close();
}

// ==================== STREAM HANDLING ====================
void sendFrame() {
    if (!streamClient || !streamClient.connected()) {
        if (streamActive) {
            streamClient.stop();
            streamActive = false;
            Serial.println("Stream ended");
        }
        return;
    }

    if (millis() - lastFrame < FRAME_INTERVAL) return;
    lastFrame = millis();

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;

    String header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + 
                    String(fb->len) + "\r\n\r\n";
    streamClient.write((const uint8_t*)header.c_str(), header.length());
    streamClient.write(fb->buf, fb->len);
    streamClient.write((const uint8_t*)"\r\n", 2);
    esp_camera_fb_return(fb);
}

void handleStream() {
    if (!streamClient || !streamClient.connected()) {
        WiFiClient newClient = streamServer.available();
        if (newClient) {
            String request = "";
            unsigned long startTime = millis();
            
            while (newClient.connected() && (millis() - startTime) < 1000) {
                if (newClient.available()) {
                    request += (char)newClient.read();
                    if (request.indexOf("\r\n\r\n") != -1) break;
                }
            }
            
            if (request.indexOf("GET /stream") != -1) {
                String response =
                "HTTP/1.1 200 OK\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

                newClient.write(response.c_str(), response.length());
                streamClient = newClient;
                streamActive = true;
                lastFrame = 0;
                Serial.println("New stream client connected");
            } else {
                newClient.stop();
            }
        }
    }
}

// ==================== SETUP & LOOP ====================
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count = 2;

    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("Camera initialization failed");
        ESP.restart();
    }

    sensor_t* s = esp_camera_sensor_get();
    s->set_brightness(s, SENSOR_BRIGHTNESS);
    s->set_exposure_ctrl(s, 1);  // Auto exposure
    s->set_gain_ctrl(s, 1);      // Auto gain
    s->set_saturation(s, 0);     // Default saturation
}

void setup() {
    Serial.begin(115200);

    // init SPIFFS (format if necessary)
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed - continuing (attempting format)");
    } else {
        Serial.println("SPIFFS mounted");
    }
    
    // WiFi Connection
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Hardware Setup
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, flashState ? HIGH : LOW);
    
    // Camera Setup
    setupCamera();

    // HTTP Routes
    server.on("/", handleRoot);
    server.on("/camera_state", handleCameraState);
    server.on("/toggle_flash", HTTP_POST, handleToggleFlash);
    server.on("/set_brightness", handleSetBrightness);
    server.on("/set_saturation", handleSetSaturation);

    // notify + last_event + photos endpoints
    server.on("/notify", HTTP_GET, handleNotify);
    server.on("/last_event", HTTP_GET, handleLastEvent);
    server.on("/photos_list", HTTP_GET, handlePhotosList);
    server.on("/photo", HTTP_GET, handlePhoto);

    server.begin();
    streamServer.begin();

    Serial.println("System Ready:");
    Serial.println("- Web Interface: http://" + WiFi.localIP().toString());
    Serial.println("- Video Stream: port 81");
    Serial.println("- Controls: Flash, Brightness (-2 to 2), Saturation (-2 to 2)");
}

void loop() {
    server.handleClient();    // Handle HTTP requests
    handleStream();           // Check for new stream clients
    if (streamActive) {
        sendFrame();          // Send video frames
    }
}

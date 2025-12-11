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
// ==================== HTTP HANDLERS ====================

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
    lastEventSeq = 0;
    lastEventStatus = "";
    lastEventId = "";

    // Hardware Setup
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, flashState ? HIGH : LOW);
    
    // Camera Setup
    setupCamera();

    // HTTP Routes
    server.on("/camera_state", handleCameraState);
    server.on("/toggle_flash", HTTP_POST, handleToggleFlash);
    server.on("/set_brightness", handleSetBrightness);
    server.on("/set_saturation", handleSetSaturation);

    // notify + last_event + photos endpoints
    server.on("/notify", HTTP_POST, handleNotify);
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

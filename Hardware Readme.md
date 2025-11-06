# What this sketch does (at a glance)

1. Sets the ESP32-CAM up for the **AI-Thinker** module (pin map + camera config).
2. Joins your Wi-Fi.
3. Starts a very small **HTTP server** with two routes:

   * `/` → tiny HTML page.
   * `/capture` → takes a picture and streams the JPEG to the client with correct HTTP headers and chunking.
4. Logs capture + send timings to Serial.

---

## 1) “board_config.h” (inlined)

```cpp
#define CAMERA_MODEL_AI_THINKER
...
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
...
#define PCLK_GPIO_NUM 22
```

* This is the **pin mapping** for the OV2640 camera on the AI-Thinker ESP32-CAM board.
* GPIOs **34–39** are **input-only**; you use them for D7..D4 etc. (that’s correct).
* `RESET_GPIO_NUM = -1` → the module’s reset line isn’t wired (common on AI-Thinker).
* `XCLK_GPIO_NUM = 0` → GPIO0 drives the camera clock. (Note: GPIO0 is also the boot-mode pin. That’s fine in normal operation; just remember it must be pulled **LOW** only when you want to enter flash/bootloader, otherwise keep it pulled-up.)

---

## 2) Includes and Wi-Fi

```cpp
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

const char* WIFI_SSID = "...";
const char* WIFI_PASS = "...";
WebServer server(80);
```

* Uses the **Arduino** core `WebServer` (synchronous) rather than AsyncWebServer.
* ⚠️ **Security note:** these credentials are hardcoded. If you post the sketch anywhere, remove them or use `WiFiManager`, captive portal, or OTA provisioning.

---

## 3) Forward declarations

```cpp
void startCameraServer_Custom();
void handleCapture();
void handleRoot();
```

* These avoid name collisions with the official example’s `startCameraServer()` and keep everything in one file.

---

## 4) `setup()` — camera config & Wi-Fi

### a) Serial + banner

```cpp
Serial.begin(115200);
Serial.setDebugOutput(true);
```

* Enables verbose logs, including from `esp_camera`.

### b) `camera_config_t`

You set the LEDC channel/timer (PWM for XCLK generation) and **all GPIO pins** from the mapping above.

```cpp
config.xclk_freq_hz = 20000000;
config.pixel_format = PIXFORMAT_JPEG;
```

* **20 MHz** XCLK is the common choice for OV2640.
* **JPEG** format offloads compression to the sensor so RAM use stays manageable.

### c) Frame buffer & quality logic (PSRAM-aware)

```cpp
config.frame_size  = FRAMESIZE_UXGA;     // start ambitiously
config.jpeg_quality = 12;                // lower = better quality
config.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
config.fb_location = CAMERA_FB_IN_PSRAM; // prefer PSRAM
config.fb_count    = 1;

if (psramFound()) {
  config.jpeg_quality = 10;
  config.fb_count = 2;                   // double buffering
  config.grab_mode = CAMERA_GRAB_LATEST; // drop stale frames if slow
} else {
  config.frame_size = FRAMESIZE_SVGA;    // be conservative
  config.fb_location = CAMERA_FB_IN_DRAM;
}
```

* **PSRAM present** → you can afford **two** frame buffers and slightly better quality.
* `CAMERA_GRAB_LATEST` means if your consumer (web) is slower than the camera, you’ll still get the newest frame, not a stale one.
* **No PSRAM** → smaller frame and single buffer to avoid OOM.

> Why start with `FRAMESIZE_UXGA` then later change it?
> Because you also tweak framesize via the sensor API below (to QVGA for streaming responsiveness). The initial large size is harmless since you then set what you truly want using `s->set_framesize()`.

### d) Initialize camera

```cpp
esp_err_t err = esp_camera_init(&config);
if (err != ESP_OK) { ... }
```

### e) Sensor tuning

```cpp
sensor_t * s = esp_camera_sensor_get();
if (s->id.PID == OV3660_PID) {
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_saturation(s, -2);
}
if (config.pixel_format == PIXFORMAT_JPEG) {
  s->set_framesize(s, FRAMESIZE_QVGA);   // 320x240 for speed/bandwidth
}
```

* The OV2640 is typical on AI-Thinker; OV3660 tweaks will be skipped on yours (fine).
* **Setting framesize to QVGA** keeps `/capture` snappy and lighter on Wi-Fi. You can bump this to `VGA`/`SVGA`/`XGA` later if your network and clients can handle the bigger JPEGs.

### f) Connect Wi-Fi

```cpp
WiFi.begin(WIFI_SSID, WIFI_PASS);
WiFi.setSleep(false);
while (WiFi.status() != WL_CONNECTED) { ... }
```

* `setSleep(false)` reduces latency at the cost of a bit more power.
* Once connected, prints the assigned IP.

### g) Start HTTP server

```cpp
startCameraServer_Custom();
```

---

## 5) `loop()`

```cpp
server.handleClient();
delay(1);
```

* The synchronous server polls for requests; `delay(1)` yields to Wi-Fi/LWIP tasks.

---

## 6) HTTP handlers

### `/` → `handleRoot()`

Sends a tiny HTML page that tells users to hit `/capture`.

### `/capture` → `handleCapture()`

This is the heart of your sketch.

**Flow:**

1. **Take a picture**

   ```cpp
   unsigned long t0 = millis();
   camera_fb_t * fb = esp_camera_fb_get();
   unsigned long t1 = millis();
   if (!fb) { 500 + return; }
   ```

   * `fb->buf` points to the JPEG, `fb->len` is its size in bytes.

2. **Send manual HTTP headers**

   ```cpp
   WiFiClient client = server.client();
   String head = "HTTP/1.1 200 OK\r\n"
                 "Content-Type: image/jpeg\r\n"
                 "Content-Length: " + String(fb->len) + "\r\n"
                 "Connection: close\r\n\r\n";
   client.print(head);
   ```

   * You **bypass** `WebServer::send()` to avoid extra/duplicate headers and to keep full control over streaming. Good call—this prevents chunked-encoding surprises and lets you specify `Content-Length` upfront.

3. **Stream the JPEG in chunks**

   ```cpp
   const size_t CHUNK = 4096;
   size_t remaining = fb->len;
   uint8_t * bufptr = fb->buf;
   while (remaining > 0 && client.connected()) {
     size_t toSend = min(remaining, CHUNK);
     size_t written = client.write(bufptr, toSend);
     if (written == 0) { ... break; }
     bufptr += toSend;
     remaining -= toSend;
     client.flush();
     delay(1);
   }
   ```

   * Splitting into **4 KB** writes avoids sending a huge buffer in one go (helps with LWIP TCP windowing and prevents long blocking).
   * `client.flush()` here mostly ensures outbound buffer pushes promptly; with some cores it only flushes inbound, but the loop structure still works well.

4. **Log timings and release framebuffer**

   ```cpp
   unsigned long t2 = millis();
   Serial.printf("Capture time: %u ms, send time: %u ms, bytes: %u\n", ...);
   esp_camera_fb_return(fb);
   ```

   * Always return the frame buffer to avoid leaks. ✅

---

## How to use it

* Flash the ESP32-CAM (AI-Thinker board setting), open Serial @115200.
* After “WiFi connected, IP: x.x.x.x”, hit from a browser:

  * `http://<ip>/` to see the info page.
  * `http://<ip>/capture` to download a JPEG.
* From command-line:

  * `curl -o snap.jpg http://<ip>/capture`

---

## Performance notes

* **Timings** in Serial help you tune quality vs. responsiveness:

  * *Capture time* grows with resolution & quality (lower `jpeg_quality` = better quality = **longer**).
  * *Send time* scales with JPEG size and Wi-Fi conditions.
* **Double buffering** (`fb_count = 2`) + `CAMERA_GRAB_LATEST` reduces “stale frame” latency when the network is slower than the camera.

---

## Common pitfalls (you’re already avoiding most)

* **PSRAM instability** → if you see random reboots, try:

  * `config.frame_size = FRAMESIZE_SVGA` or smaller
  * `config.fb_count = 1`
  * Ensure 3.3V rail is solid; the ESP32-CAM can be spiky.
* **GPIO0 / boot mode** → if you suddenly can’t flash, make sure IO0 is pulled low **only** for flashing.
* **WebServer blocking** → heavy operations in handlers pause the main loop. Your handler is short and stream-oriented, which is fine.

---

## Nice upgrades you can add

1. **Resolution switch on query:**

   ```cpp
   // inside handleCapture(), before fb_get:
   sensor_t* s = esp_camera_sensor_get();
   if (server.hasArg("res")) {
     String r = server.arg("res"); // "qvga","svga","xga","uxga"
     if (r == "svga") s->set_framesize(s, FRAMESIZE_SVGA);
     else if (r == "xga") s->set_framesize(s, FRAMESIZE_XGA);
     else if (r == "uxga") s->set_framesize(s, FRAMESIZE_UXGA);
     else s->set_framesize(s, FRAMESIZE_QVGA);
   }
   ```

   Then call: `/capture?res=svga`

2. **Content-Disposition** so the browser downloads with a name:

   ```cpp
   "Content-Disposition: inline; filename=\"snapshot.jpg\"\r\n"
   ```

3. **Basic auth** or a simple token:

   ```cpp
   if (!server.hasArg("token") || server.arg("token") != "YOUR_TOKEN") {
     server.send(401, "text/plain", "Unauthorized");
     return;
   }
   ```

4. **CORS** (if you’ll fetch from a web app):

   ```cpp
   "Access-Control-Allow-Origin: *\r\n"
   ```

5. **mDNS**:

   ```cpp
   #include <ESPmDNS.h>
   MDNS.begin("smarteye");  // http://smarteye.local
   ```

6. **AsyncWebServer** for multiple concurrent clients or adding an **MJPEG** `/stream` endpoint later.

7. **Health endpoint**:

   ```cpp
   server.on("/health", HTTP_GET, [](){ server.send(200, "text/plain", "ok"); });
   ```

---

## TL;DR of the important design choices you made (and why they’re good)

* **Manual HTTP response** in `/capture` → no duplicate headers and full control of `Content-Length` + chunking. Great for reliability.
* **PSRAM-aware config** → scales quality & buffers safely across boards.
* **`CAMERA_GRAB_LATEST`** with `fb_count=2` → lower perceived latency.
* **Sensor framesize forced to QVGA** → keeps snapshots snappy unless you ask for more.

If you want, tell me how you plan to consume the image (browser page, Python client, mobile app, etc.). I can tailor a version with query-selectable resolution/quality or build a minimal HTML preview page that fetches `/capture` and shows the latest frame at an interval.

/*
  SmartEye_CameraServer_WithBoardConfig.ino
  Single-file sketch that includes AI-Thinker board pin mapping (board_config.h)
  and a robust /capture endpoint. Rename-free (no startCameraServer() conflict).
*/

// ----------------- board_config.h content (AI-Thinker) -----------------
// Select camera model
#define CAMERA_MODEL_AI_THINKER

// AI Thinker pin definition
#if defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#endif
// ----------------------------------------------------------------------

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// ======== WiFi credentials - EDIT THESE =========
const char *WIFI_SSID = "Pixel";
const char *WIFI_PASS = "123456789";
// ================================================

WebServer server(80);

// Forward declarations (no conflict with library)
void startCameraServer_Custom();
void handleCapture();
void handleRoot();

void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("SmartEye - Camera server (custom capture)");

  // camera config (same structure as official example)
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  // pins defined above inlined from board_config.h
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Start with high setting and reduce if no PSRAM
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 1;

  if (config.pixel_format == PIXFORMAT_JPEG)
  {
    if (psramFound())
    {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    }
    else
    {
      // conservative when PSRAM missing
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  }
  else
  {
    config.frame_size = FRAMESIZE_240X240;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    while (true)
    {
      delay(1000);
    }
  }

  // sensor tuning like example
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID)
  {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  if (config.pixel_format == PIXFORMAT_JPEG)
  {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

  // start WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // start server with our custom function (name unique)
  startCameraServer_Custom();

  Serial.println("Setup complete. Use '/capture' to fetch a JPEG snapshot.");
}

void loop()
{
  server.handleClient();
  delay(1);
}

// ------------------------ handlers & server init ------------------------

void handleRoot()
{
  String html = "<html><body><h3>SmartEye Snapshot Server</h3>"
                "<p>Use <code>/capture</code> to get a single JPEG snapshot.</p>"
                "</body></html>";
  server.send(200, "text/html", html);
}

// Robust /capture handler: capture, send headers (no duplication), stream in chunks
void handleCapture()
{
  unsigned long t0 = millis();
  camera_fb_t *fb = esp_camera_fb_get();
  unsigned long t1 = millis();
  if (!fb)
  {
    server.send(500, "text/plain", "Camera capture failed");
    Serial.println("Camera capture failed (fb null)");
    return;
  }

  WiFiClient client = server.client();

  // Manual headers to avoid WebServer adding default/duplicate headers
  String head = String("HTTP/1.1 200 OK\r\n") +
                "Content-Type: image/jpeg\r\n" +
                "Content-Length: " + String(fb->len) + "\r\n" +
                "Connection: close\r\n" +
                "\r\n";
  client.print(head);

  // Stream the image in chunks
  const size_t CHUNK = 4096;
  size_t remaining = fb->len;
  uint8_t *bufptr = fb->buf;
  while (remaining > 0 && client.connected())
  {
    size_t toSend = (remaining > CHUNK) ? CHUNK : remaining;
    size_t written = client.write(bufptr, toSend);
    if (written == 0)
    {
      Serial.println("Warning: wrote 0 bytes - client may have disconnected");
      break;
    }
    bufptr += toSend;
    remaining -= toSend;
    client.flush();
    delay(1);
  }

  unsigned long t2 = millis();
  Serial.printf("Capture time: %u ms, send time: %u ms, bytes: %u\n",
                (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)fb->len);

  esp_camera_fb_return(fb);
  delay(10);
}

// startCameraServer_Custom: registers routes and starts server
void startCameraServer_Custom()
{
  // Register only the routes we need (no conflict)
  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.begin();
  Serial.println("HTTP server started (custom).");
}

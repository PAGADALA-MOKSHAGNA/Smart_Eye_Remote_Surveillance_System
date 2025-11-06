#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- WiFi ----------
const char *WIFI_SSID = "Janardhana Rao";
const char *WIFI_PASS = "Madhavi#888";
WebServer server(80);

// ---------- Pins ----------
const int SERVO_PIN = 25;
const int PIR_PIN = 13;
const int BUZZER_PIN = 14;
const int IR_PIN = 27;
const int TRIG_PIN = 5;
const int ECHO_PIN = 18;

// ---------- LCD (0x27) ----------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------- Servo / Pan Config ----------
Servo myServo;
const unsigned long panDurationMs = 2100UL;
const int startAngle = 30;
const int endAngle = 180;
const unsigned long minRetriggerDelayMs = 3000UL;

// ---------- PIR Debounce ----------
const unsigned long pirDebounceMs = 500UL;
unsigned long lastPirReadChangeAt = 0;
unsigned long lastPIRAcceptedAt = 0;
int lastPirRaw = LOW;

// ---------- IR sensor (active LOW) ----------
volatile bool irObjectDetected = false;

// ---------- Ultrasonic ----------
const unsigned long echoTimeoutUs = 30000UL;
const int distanceThresholdCm = 20;

// ---------- Pan state ----------
bool panActive = false;
unsigned long panStartMillis = 0;

// ---------- Helpers ----------
void startPanSequence()
{
  if (!panActive)
  {
    panActive = true;
    panStartMillis = millis();
    Serial.println("[PAN] started");
  }
}

void updatePan()
{
  if (!panActive)
    return;

  unsigned long elapsed = millis() - panStartMillis;
  unsigned long fullCycle = panDurationMs * 2UL;

  if (elapsed >= fullCycle)
  {
    myServo.write(startAngle);
    panActive = false;
    Serial.println("[PAN] completed");
    return;
  }

  if (elapsed < panDurationMs)
  {
    float progress = (float)elapsed / (float)panDurationMs;
    int angle = startAngle + (int)((endAngle - startAngle) * progress);
    myServo.write(angle);
  }
  else
  {
    float progress = (float)(elapsed - panDurationMs) / (float)panDurationMs;
    int angle = endAngle - (int)((endAngle - startAngle) * progress);
    myServo.write(angle);
  }
}

bool isPIRTriggered()
{
  int raw = digitalRead(PIR_PIN);
  if (raw != lastPirRaw)
  {
    lastPirRaw = raw;
    lastPirReadChangeAt = millis();
    return false;
  }
  if (millis() - lastPirReadChangeAt < pirDebounceMs)
    return false;

  if (raw == HIGH && (millis() - lastPIRAcceptedAt >= minRetriggerDelayMs))
  {
    lastPIRAcceptedAt = millis();
    return true;
  }
  return false;
}

long readDistanceCm()
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long dur = pulseIn(ECHO_PIN, HIGH, echoTimeoutUs);
  if (dur == 0)
    return -1;
  return (long)(dur * 0.0343 / 2.0);
}

void beep(unsigned ms = 120)
{
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

// ---------- HTTP ----------
void handleRoot()
{
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='1'>"
                "<title>ESP32 Sensor Server</title></head><body>";
  html += "<h2>ESP32 Sensor Dashboard</h2>";
  html += "<p><a href=\"/status\">/status</a> (JSON)</p>";
  html += "<p><a href=\"/trigger\">/trigger</a> (start pan)</p>";
  html += "<p><a href=\"/stop\">/stop</a> (stop pan)</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void sendJsonStatus()
{
  int pirState = digitalRead(PIR_PIN);
  int irState = digitalRead(IR_PIN);
  long dist = readDistanceCm();
  int servoAngle = myServo.read();
  unsigned long now = millis();

  String payload = "{";
  payload += "\"panActive\":";
  payload += (panActive ? "true" : "false");
  payload += ",";
  payload += "\"pirState\":";
  payload += (pirState == HIGH ? "\"HIGH\"" : "\"LOW\"");
  payload += ",";
  payload += "\"irState\":";
  payload += (irState == LOW ? "\"DETECTED\"" : "\"CLEAR\"");
  payload += ",";
  payload += "\"distanceCm\":";
  payload += String(dist);
  payload += ",";
  payload += "\"lastPIRAcceptedAt\":";
  payload += String(lastPIRAcceptedAt);
  payload += ",";
  payload += "\"now\":";
  payload += String(now);
  payload += ",";
  payload += "\"servoAngle\":";
  payload += String(servoAngle);
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleStatus() { sendJsonStatus(); }

void handleTrigger()
{
  startPanSequence();
  beep(100);
  server.send(200, "application/json", "{\"result\":\"pan_triggered\"}");
}

void handleStop()
{
  panActive = false;
  myServo.write(startAngle);
  server.send(200, "application/json", "{\"result\":\"pan_stopped\"}");
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ---------- Setup ----------
void setup()
{
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(IR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Eye Initialized");
  delay(1000);
  lcd.clear();

  // Servo init
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN);
  myServo.write(startAngle);

  Serial.println("[SYS] Booting...");

  // Wi-Fi
  Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  const unsigned long wifiTimeout = 15000UL;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < wifiTimeout)
  {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    lcd.setCursor(0, 0);
    lcd.print("WiFi OK:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/trigger", handleTrigger);
    server.on("/stop", handleStop);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[HTTP] Server started on :80");
  }
  else
  {
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed");
    Serial.println("[WiFi] Failed; offline mode.");
  }

  delay(1000);
  lcd.clear();
  lcd.print("System Ready");
}

// ---------- Main Loop ----------
void loop()
{
  if (WiFi.status() == WL_CONNECTED)
    server.handleClient();

  // IR Sensor
  irObjectDetected = (digitalRead(IR_PIN) == LOW);

  // PIR
  bool pirTrig = isPIRTriggered();

  // Ultrasonic
  long distance = readDistanceCm();
  if (distance >= 0)
    Serial.printf("[US] %ld cm\n", distance);

  // LCD update
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("D:");
  lcd.print(distance);
  lcd.print("cm ");
  lcd.print(irObjectDetected ? "IR!" : "  ");
  lcd.setCursor(0, 1);
  lcd.print(pirTrig ? "PIR!" : "    ");
  lcd.print(panActive ? " PAN" : "    ");

  // Decision
  bool closeObject = (distance >= 0 && distance < distanceThresholdCm);
  if (pirTrig || irObjectDetected || closeObject)
  {
    Serial.println("[EVT] Anomaly Detected!");
    beep(150);
    startPanSequence();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Anomaly!");
    lcd.setCursor(0, 1);
    lcd.print("Scanning Area...");
    delay(400);
  }

  updatePan();
  delay(100);
}
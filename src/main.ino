#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// This project runs as a Wi-Fi hotspot (AP mode). Connect your phone/laptop to this SSID.
const char* apSsid = "ESP32C3-Keypad";
const char* apPassword = "12345678"; // 8+ chars

WebServer server(80);

static volatile char g_lastKey = 0;

// Flow inputs (hall sensors)
static const int FLOW_PIN_1 = 2;
static const int FLOW_PIN_2 = 3;

// Valve control (set to a real pin when you wire it)
static const int VALVE_PIN = -1; // -1 disables valve control
static const bool VALVE_ACTIVE_HIGH = true;

static volatile uint32_t g_pulses1 = 0;
static volatile uint32_t g_pulses2 = 0;

// Config persisted in LittleFS
static const char* CONFIG_PATH = "/config.json";

struct Product {
  String name;
  float pulsesPerGallon = 0.0f;
  uint32_t valveCloseTimeMs = 0;
};

struct Config {
  float calibrationPulsesPerGallon = 0.0f; // optional global calibration
  Product products[10];
  size_t productCount = 0;
};

Config g_cfg;

static IRAM_ATTR void isrFlow1() { g_pulses1++; }
static IRAM_ATTR void isrFlow2() { g_pulses2++; }

static uint32_t readAndClear(volatile uint32_t& v) {
  noInterrupts();
  uint32_t x = v;
  v = 0;
  interrupts();
  return x;
}

static uint32_t readValue(volatile uint32_t& v) {
  noInterrupts();
  uint32_t x = v;
  interrupts();
  return x;
}

static void valveWrite(bool open) {
  if (VALVE_PIN < 0) return;
  const bool level = VALVE_ACTIVE_HIGH ? open : !open;
  digitalWrite(VALVE_PIN, level ? HIGH : LOW);
}

static bool loadConfig() {
  g_cfg = Config();
  if (!LittleFS.exists(CONFIG_PATH)) {
    return false;
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("Config parse error: %s\n", err.c_str());
    return false;
  }

  g_cfg.calibrationPulsesPerGallon = doc["calibrationPulsesPerGallon"].as<float>();
  JsonArray prods = doc["products"].as<JsonArray>();
  if (!prods.isNull()) {
    for (JsonObject p : prods) {
      if (g_cfg.productCount >= 10) break;
      Product& out = g_cfg.products[g_cfg.productCount++];
      out.name = p["name"].as<String>();
      out.pulsesPerGallon = p["pulsesPerGallon"].as<float>();
      out.valveCloseTimeMs = p["valveCloseTimeMs"].as<uint32_t>();
    }
  }

  return true;
}

static bool saveConfig() {
  JsonDocument doc;
  doc["calibrationPulsesPerGallon"] = g_cfg.calibrationPulsesPerGallon;
  JsonArray prods = doc["products"].to<JsonArray>();
  for (size_t i = 0; i < g_cfg.productCount; i++) {
    JsonObject p = prods.add<JsonObject>();
    p["name"] = g_cfg.products[i].name;
    p["pulsesPerGallon"] = g_cfg.products[i].pulsesPerGallon;
    p["valveCloseTimeMs"] = g_cfg.products[i].valveCloseTimeMs;
  }

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  const size_t n = serializeJson(doc, f);
  f.close();
  return n > 0;
}

static void sendText(int code, const char* text) {
  server.send(code, "text/plain", text);
}

static void onWiFiEvent(WiFiEvent_t event) {
  Serial.printf("[WiFi event] %d\n", (int)event);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Booting ESP32-C3 keypad AP...");
  WiFi.onEvent(onWiFiEvent);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed. Did you upload the data/ folder?");
  }

  loadConfig();

  pinMode(FLOW_PIN_1, INPUT_PULLUP);
  pinMode(FLOW_PIN_2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_1), isrFlow1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_2), isrFlow2, FALLING);

  if (VALVE_PIN >= 0) {
    pinMode(VALVE_PIN, OUTPUT);
    valveWrite(false);
  }

  WiFi.disconnect(true);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);

  // Force a sane, visible AP configuration.
  // channel: 1 (common), hidden: false (broadcast SSID), max_connection: 4
  const int channel = 1;
  const int max_connection = 4;
  const bool hidden = false;
  const bool ok = WiFi.softAP(apSsid, apPassword, channel, hidden, max_connection);

  Serial.printf("AP start: %s\n", ok ? "OK" : "FAILED");
  Serial.print("AP SSID: ");
  Serial.println(apSsid);
  Serial.print("AP Password: ");
  Serial.println(apPassword);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("AP channel: ");
  Serial.println(WiFi.channel());
  Serial.print("AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());

  // Redirect root to the UI
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Location", "/index.html");
    server.send(302, "text/plain", "");
  });

  // Basic endpoint (old keypad) still supported
  server.on("/keypress", HTTP_GET, []() {
    if (!server.hasArg("value")) {
      sendText(400, "missing value");
      return;
    }
    const String v = server.arg("value");
    if (v.length() > 0) {
      g_lastKey = v[0];
      Serial.printf("Keypress: %c\n", g_lastKey);
    }
    server.send(204, "text/plain", "");
  });

  // Live status for UI
  server.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["pulses1"] = readValue(g_pulses1);
    doc["pulses2"] = readValue(g_pulses2);
    doc["calibrationPulsesPerGallon"] = g_cfg.calibrationPulsesPerGallon;
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
  });

  // Calibration: set global pulses/gal
  server.on("/api/calibration", HTTP_POST, []() {
    if (!server.hasArg("pulsesPerGallon")) {
      sendText(400, "missing pulsesPerGallon");
      return;
    }
    g_cfg.calibrationPulsesPerGallon = server.arg("pulsesPerGallon").toFloat();
    const bool ok = saveConfig();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  // Reset counters
  server.on("/api/pulses/reset", HTTP_POST, []() {
    (void)readAndClear(g_pulses1);
    (void)readAndClear(g_pulses2);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // Products CRUD (simple)
  server.on("/api/products", HTTP_GET, []() {
    JsonDocument doc;
    JsonArray arr = doc["products"].to<JsonArray>();
    for (size_t i = 0; i < g_cfg.productCount; i++) {
      JsonObject p = arr.add<JsonObject>();
      p["id"] = (uint32_t)i;
      p["name"] = g_cfg.products[i].name;
      p["pulsesPerGallon"] = g_cfg.products[i].pulsesPerGallon;
      p["valveCloseTimeMs"] = g_cfg.products[i].valveCloseTimeMs;
    }
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
  });

  server.on("/api/products", HTTP_POST, []() {
    if (!server.hasArg("name") || !server.hasArg("pulsesPerGallon") || !server.hasArg("valveCloseTimeMs")) {
      sendText(400, "missing fields");
      return;
    }
    if (g_cfg.productCount >= 10) {
      sendText(400, "too many products");
      return;
    }
    Product& p = g_cfg.products[g_cfg.productCount++];
    p.name = server.arg("name");
    p.pulsesPerGallon = server.arg("pulsesPerGallon").toFloat();
    p.valveCloseTimeMs = (uint32_t)server.arg("valveCloseTimeMs").toInt();
    const bool ok = saveConfig();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server.on("/api/products/delete", HTTP_POST, []() {
    if (!server.hasArg("id")) {
      sendText(400, "missing id");
      return;
    }
    const int id = server.arg("id").toInt();
    if (id < 0 || (size_t)id >= g_cfg.productCount) {
      sendText(400, "invalid id");
      return;
    }
    for (size_t i = (size_t)id; i + 1 < g_cfg.productCount; i++) {
      g_cfg.products[i] = g_cfg.products[i + 1];
    }
    g_cfg.productCount--;
    const bool ok = saveConfig();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  // Useful for debugging from browser
  server.on("/last", HTTP_GET, []() {
    String body = String("{\"last\":\"") + (g_lastKey ? String((char)g_lastKey) : String("")) + "\"}";
    server.send(200, "application/json", body);
  });

  // Static file serving from LittleFS (data/ uploaded to the device)
  server.serveStatic("/index.html", LittleFS, "/index.html");
  server.serveStatic("/app.js", LittleFS, "/app.js");
  server.serveStatic("/styles.css", LittleFS, "/styles.css");

  // Fallback: try to serve the requested path from filesystem
  server.onNotFound([]() {
    const String path = server.uri();
    if (LittleFS.exists(path)) {
      String contentType = "text/plain";
      if (path.endsWith(".html")) contentType = "text/html";
      else if (path.endsWith(".css")) contentType = "text/css";
      else if (path.endsWith(".js")) contentType = "application/javascript";
      else if (path.endsWith(".json")) contentType = "application/json";

      File f = LittleFS.open(path, "r");
      server.streamFile(f, contentType);
      f.close();
      return;
    }
    sendText(404, "not found");
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
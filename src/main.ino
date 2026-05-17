#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ESP-IDF GPIO driver for early pin-level control (boot-glitch mitigation)
#include <driver/gpio.h>

// --- Valve control ---
// Switched to a simple 12V 3-wire motorized valve driven by a 2-relay module.
// Relays switch +12V to either the OPEN wire or the CLOSE wire (never both).

// This project runs as a Wi-Fi hotspot (AP mode). Connect your phone/laptop to this SSID.
const char* apSsid = "ESP32C3-Keypad";
const char* apPassword = "12345678"; // 8+ chars

WebServer server(80);

// Bump this when you change anything in data/ (HTML/CSS/JS)
static const char* UI_VERSION = "2026-05-09-2";

static bool g_fsMounted = false;

static bool ensureFS() {
  if (g_fsMounted) return true;
  // Try to (re)mount without formatting during runtime.
  g_fsMounted = LittleFS.begin(false);
  return g_fsMounted;
}

static volatile char g_lastKey = 0;

// Flow inputs (hall sensors)
static const int FLOW_PIN_1 = 2;
static const int FLOW_PIN_2 = 3;

// Relay module wiring (ESP32-C3 SuperMini):
// - IN1 -> GPIO7
// - IN2 -> GPIO8
// Leave GPIO4/5/6 free for future RS485.
static const int RELAY_OPEN_PIN = 7;
static const int RELAY_CLOSE_PIN = 8;

// Valve wiring mode:
// Your valve is "power + enable" style:
// - Blue: GND
// - Brown: constant +12V power (always on)
// - Black: enable/control (+12V = OPEN, off = CLOSE)
// That means we only need ONE relay channel to switch +12V to the BLACK wire.
// RELAY_OPEN_PIN drives BLACK. RELAY_CLOSE_PIN is unused.
static const bool VALVE_SINGLE_ENABLE_RELAY = true;

// Many 2-relay boards are "active LOW" (IN=LOW energizes relay), but some are
// "active HIGH" (IN=HIGH energizes relay). With no H/L jumper, the only way to
// handle both is making it configurable.
//
// If your relay IN1 LED is ON all the time at boot, you're likely ACTIVE_HIGH
// (because we default pins HIGH to keep them from floating). We'll default to
// ACTIVE_HIGH and allow switching via API.
static bool g_relayActiveLow = false;

static inline int relayActiveLevel() {
  return g_relayActiveLow ? LOW : HIGH;
}

static inline int relayInactiveLevel() {
  return g_relayActiveLow ? HIGH : LOW;
}

static void relayBootSafeInit() {
  // Attempt to prevent a brief relay ON at boot by:
  // 1) configuring GPIO as output via IDF
  // 2) driving it to the INACTIVE level immediately
  // 3) enabling an internal pull that biases toward INACTIVE
  const int inactive = relayInactiveLevel();

  auto initPin = [&](int pin) {
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, (inactive == HIGH) ? 1 : 0);

    // Bias the pin so it doesn't float during transitions.
    // If inactive is HIGH => enable pull-up.
    // If inactive is LOW  => enable pull-down.
    gpio_set_pull_mode((gpio_num_t)pin, (inactive == HIGH) ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
  };

  initPin(RELAY_OPEN_PIN);
  initPin(RELAY_CLOSE_PIN);
}

// --- Serial debug helpers ---
// Some failures happen before WiFi starts; we want a very early heartbeat.
static const uint32_t SERIAL_BAUD = 115200;

static void serialEarlyInit() {
  // In USB-Serial/JTAG mode, Serial can take a moment. Keep it non-blocking.
  Serial.begin(SERIAL_BAUD);
  delay(50);
  Serial.println();
  Serial.println("[BOOT] ESP32C3-Keypad starting...");
  Serial.printf("[BOOT] UI_VERSION=%s\n", UI_VERSION);
}

static void serialPeriodicWiFiStatus() {
  static uint32_t last = 0;
  const uint32_t now = nowMs();
  if (now - last < 2000) return;
  last = now;

  // These prints are intentionally short and frequent.
  Serial.printf(
    "[WiFi] mode=%d status=%d AP=%d stations=%d ip=%s\n",
    (int)WiFi.getMode(),
    (int)WiFi.status(),
    (int)WiFi.softAPgetStationNum() >= 0,
    (int)WiFi.softAPgetStationNum(),
    WiFi.softAPIP().toString().c_str()
  );
}

// How long to energize a relay for an open/close command.
// Your valve label shows ~3s; we default to 3500ms.
// You reported ~4 seconds to fully open/close, so set default to 4000ms.
static const uint32_t VALVE_DRIVE_OPEN_MS_DEFAULT = 4000;
static const uint32_t VALVE_DRIVE_CLOSE_MS_DEFAULT = 4000;

static uint32_t g_valveDriveOpenMs = VALVE_DRIVE_OPEN_MS_DEFAULT;
static uint32_t g_valveDriveCloseMs = VALVE_DRIVE_CLOSE_MS_DEFAULT;
static uint32_t g_valveDriveUntilMs = 0;
static int8_t g_valveDriveDir = 0; // -1 closing, +1 opening, 0 idle

static volatile uint32_t g_pulses1 = 0;
static volatile uint32_t g_pulses2 = 0;

// Batch run state (uses Sensor1 pulses by default)
enum class BatchState : uint8_t { Idle, Running, ClosingDelay, Done, Error };
static volatile BatchState g_batchState = BatchState::Idle;
static volatile uint32_t g_batchTargetPulses = 0;
static volatile uint32_t g_batchStartPulses = 0;
static volatile uint32_t g_batchStopAtMs = 0;
static volatile uint32_t g_batchClosedAtMs = 0;
static volatile int g_batchProductId = -1;

static uint32_t nowMs() { return (uint32_t)millis(); }

// Forward declarations (Arduino's auto-prototypes can get the order wrong)
struct Product;
struct Config;
static void valveWrite(bool open);
static bool valveSetPositionDeg100(int16_t deg100);
static bool valveReadPositionDeg100(int16_t* outDeg100);
static bool valveClearError();
static bool valveReadError(uint16_t* outErr);
static void batchStopInternal();
static float productPulsesPerGallon(const Product& p);

// Config persisted in LittleFS
static const char* CONFIG_PATH = "/config.json";

struct Product {
  String name;
  float pulsesPerGallon = 0.0f;
  uint32_t valveCloseTimeMs = 0;
};

struct Config {
  float calibrationPulsesPerGallon = 0.0f; // optional global calibration
  uint32_t valveDriveOpenMs = VALVE_DRIVE_OPEN_MS_DEFAULT;
  uint32_t valveDriveCloseMs = VALVE_DRIVE_CLOSE_MS_DEFAULT;
  Product products[10];
  size_t productCount = 0;
};

Config g_cfg;

static void batchStopInternal() {
  valveWrite(false);
  g_batchState = BatchState::Idle;
  g_batchTargetPulses = 0;
  g_batchProductId = -1;
}

static void relayValveStop() {
  const int inactive = relayInactiveLevel();
  digitalWrite(RELAY_OPEN_PIN, inactive);
  digitalWrite(RELAY_CLOSE_PIN, inactive);
  g_valveDriveUntilMs = 0;
  g_valveDriveDir = 0;
}

static float productPulsesPerGallon(const Product& p) {
  if (p.pulsesPerGallon > 0.0f) return p.pulsesPerGallon;
  return g_cfg.calibrationPulsesPerGallon;
}

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
  const int inactive = relayInactiveLevel();
  const int active = relayActiveLevel();

  // Always start from a known safe state.
  digitalWrite(RELAY_OPEN_PIN, inactive);
  digitalWrite(RELAY_CLOSE_PIN, inactive);

  if (VALVE_SINGLE_ENABLE_RELAY) {
    // Single-enable valve behavior (BLACK enable wire):
    // - OPEN  => energize relay continuously
    // - CLOSE => de-energize relay continuously
    // No timed auto-stop here (it causes the "click again" reopen behavior).
    g_valveDriveUntilMs = 0;
    g_valveDriveDir = open ? 1 : -1;
    digitalWrite(RELAY_OPEN_PIN, open ? active : inactive);
    return;
  }

  // Two-relay directional mode (legacy): drive for a fixed period.
  const uint32_t driveMs = open ? g_valveDriveOpenMs : g_valveDriveCloseMs;
  g_valveDriveUntilMs = nowMs() + driveMs;
  g_valveDriveDir = open ? 1 : -1;
  if (open) digitalWrite(RELAY_OPEN_PIN, active);
  else digitalWrite(RELAY_CLOSE_PIN, active);
}

// Relay valve "status" is inferred from last command.
static bool valveReadPositionDeg100(int16_t* outDeg100) {
  if (!outDeg100) return true;
  // Unknown actual angle; return 0 (open) or 9000 (closed) based on last direction.
  if (g_valveDriveDir > 0) *outDeg100 = 0;
  else if (g_valveDriveDir < 0) *outDeg100 = 9000;
  else *outDeg100 = -32768;
  return true;
}

static bool valveReadError(uint16_t* outErr) {
  if (outErr) *outErr = 0;
  return true;
}

static bool valveClearError() {
  return true;
}

static bool valveSetPositionDeg100(int16_t deg100) {
  // Maintain compatibility with UI/API: <=4500 => open, else close
  valveWrite(deg100 <= 4500);
  return true;
}

static bool loadConfig() {
  g_cfg = Config();
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("Config: /config.json not found (starting fresh)");
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
  g_cfg.valveDriveOpenMs = doc["valveDriveOpenMs"] | VALVE_DRIVE_OPEN_MS_DEFAULT;
  g_cfg.valveDriveCloseMs = doc["valveDriveCloseMs"] | VALVE_DRIVE_CLOSE_MS_DEFAULT;
  // Relay polarity (default: ACTIVE_HIGH)
  g_relayActiveLow = doc["relayActiveLow"] | false;

  // Apply to runtime variables
  g_valveDriveOpenMs = g_cfg.valveDriveOpenMs;
  g_valveDriveCloseMs = g_cfg.valveDriveCloseMs;
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
  doc["valveDriveOpenMs"] = g_cfg.valveDriveOpenMs;
  doc["valveDriveCloseMs"] = g_cfg.valveDriveCloseMs;
  doc["relayActiveLow"] = g_relayActiveLow;
  JsonArray prods = doc["products"].to<JsonArray>();
  for (size_t i = 0; i < g_cfg.productCount; i++) {
    JsonObject p = prods.add<JsonObject>();
    p["name"] = g_cfg.products[i].name;
    p["pulsesPerGallon"] = g_cfg.products[i].pulsesPerGallon;
    p["valveCloseTimeMs"] = g_cfg.products[i].valveCloseTimeMs;
  }

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println("Config: failed to open /config.json for write");
    return false;
  }
  const size_t n = serializeJson(doc, f);
  f.flush();
  f.close();
  if (n == 0) Serial.println("Config: serializeJson wrote 0 bytes");

  // Verify write by checking size and attempting to parse back.
  File rf = LittleFS.open(CONFIG_PATH, "r");
  if (!rf) {
    Serial.println("Config: verify failed to reopen /config.json");
    return false;
  }
  const size_t size = (size_t)rf.size();
  JsonDocument check;
  const DeserializationError err = deserializeJson(check, rf);
  rf.close();
  if (err) {
    Serial.printf("Config: verify parse error: %s\n", err.c_str());
    return false;
  }
  if (size == 0) {
    Serial.println("Config: verify size is 0");
    return false;
  }
  return n > 0;
}

static void sendText(int code, const char* text) {
  server.send(code, "text/plain", text);
}

static void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

static void sendShortCacheHeaders() {
  // Improves perceived speed when switching pages: allow caching for a bit.
  server.sendHeader("Cache-Control", "public, max-age=300");
}

static void onWiFiEvent(WiFiEvent_t event) {
  Serial.printf("[WiFi event] %d\n", (int)event);
}

void setup() {
  serialEarlyInit();
  WiFi.onEvent(onWiFiEvent);

  // Relay boot safety: drive relay pins to INACTIVE as early as possible.
  relayBootSafeInit();

  // Relay valve setup (Arduino layer)
  pinMode(RELAY_OPEN_PIN, OUTPUT);
  pinMode(RELAY_CLOSE_PIN, OUTPUT);
  relayValveStop();

  // LittleFS mount. If it fails (e.g., fresh chip or corrupted FS), format and retry.
  g_fsMounted = LittleFS.begin(false);
  if (!g_fsMounted) {
    Serial.println("LittleFS mount failed. Formatting...");
    g_fsMounted = LittleFS.begin(true);
    if (!g_fsMounted) {
      Serial.println("LittleFS mount failed even after format.");
    } else {
      Serial.println("LittleFS formatted and mounted.");
    }
  }

  loadConfig();

  // Re-apply stop after loading polarity from config.
  relayValveStop();

  pinMode(FLOW_PIN_1, INPUT_PULLUP);
  pinMode(FLOW_PIN_2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_1), isrFlow1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_2), isrFlow2, FALLING);
  // Ensure valve is commanded closed at boot (best-effort)
  valveWrite(false);

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

    // Valve diagnostics (best-effort; if read fails we omit values)
    int16_t pos = 0;
    uint16_t err = 0;
    if (valveReadPositionDeg100(&pos)) {
      doc["valve"]["positionDeg100"] = (int)pos;
    }
    if (valveReadError(&err)) {
      doc["valve"]["error"] = (uint32_t)err;
    }

    doc["valve"]["driveOpenMs"] = (uint32_t)g_valveDriveOpenMs;
    doc["valve"]["driveCloseMs"] = (uint32_t)g_valveDriveCloseMs;

    const BatchState st = g_batchState;
    const uint32_t cur = readValue(g_pulses1);
    doc["batch"] ["state"] = (uint32_t)st;
    doc["batch"] ["productId"] = (int)g_batchProductId;
    doc["batch"] ["targetPulses"] = (uint32_t)g_batchTargetPulses;
    doc["batch"] ["startPulses"] = (uint32_t)g_batchStartPulses;
    doc["batch"] ["currentPulses"] = (uint32_t)cur;
    doc["batch"] ["stopAtMs"] = (uint32_t)g_batchStopAtMs;
    doc["batch"] ["closedAtMs"] = (uint32_t)g_batchClosedAtMs;

    // products (so the Run page can compute remaining gallons without a second request)
    {
      JsonArray arr = doc["products"].to<JsonArray>();
      for (size_t i = 0; i < g_cfg.productCount; i++) {
        JsonObject p = arr.add<JsonObject>();
        p["id"] = (uint32_t)i;
        p["name"] = g_cfg.products[i].name;
        p["pulsesPerGallon"] = g_cfg.products[i].pulsesPerGallon;
        p["valveCloseTimeMs"] = g_cfg.products[i].valveCloseTimeMs;
      }
    }

    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
  });

  // Valve API (optional diagnostics / manual control)
  server.on("/api/valve/position", HTTP_GET, []() {
    int16_t pos = 0;
    if (!valveReadPositionDeg100(&pos)) {
      sendText(500, "valve read failed");
      return;
    }
    JsonDocument doc;
    doc["positionDeg100"] = (int)pos;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/valve/error", HTTP_GET, []() {
    uint16_t err = 0;
    if (!valveReadError(&err)) {
      sendText(500, "valve read failed");
      return;
    }
    JsonDocument doc;
    doc["error"] = (uint32_t)err;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/valve/error/clear", HTTP_POST, []() {
    if (!valveClearError()) {
      sendText(500, "valve clear failed");
      return;
    }
    server.send(204, "text/plain", "");
  });

  // Set valve position (deg*100) e.g. 0=open, 9000=closed
  server.on("/api/valve/set", HTTP_POST, []() {
    if (!server.hasArg("deg100")) {
      sendText(400, "missing deg100");
      return;
    }
    const int deg100 = server.arg("deg100").toInt();
    if (!valveSetPositionDeg100((int16_t)deg100)) {
      sendText(500, "valve write failed");
      return;
    }
    server.send(204, "text/plain", "");
  });

  // Immediately stop driving the valve (de-energize relays)
  server.on("/api/valve/stop", HTTP_POST, []() {
    relayValveStop();
    server.send(204, "text/plain", "");
  });

  // Valve drive tuning (ms)
  server.on("/api/valve/drive_ms", HTTP_GET, []() {
    JsonDocument doc;
    doc["openMs"] = (uint32_t)g_valveDriveOpenMs;
    doc["closeMs"] = (uint32_t)g_valveDriveCloseMs;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // POST openMs=... closeMs=...
  server.on("/api/valve/drive_ms", HTTP_POST, []() {
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
    if (!server.hasArg("openMs") && !server.hasArg("closeMs")) {
      sendText(400, "missing openMs/closeMs");
      return;
    }

    // Guardrails: 50ms..10000ms
    auto clampMs = [](uint32_t v) -> uint32_t {
      if (v < 50) return 50;
      if (v > 10000) return 10000;
      return v;
    };

    if (server.hasArg("openMs")) {
      const uint32_t v = (uint32_t)server.arg("openMs").toInt();
      g_valveDriveOpenMs = clampMs(v);
      g_cfg.valveDriveOpenMs = g_valveDriveOpenMs;
    }
    if (server.hasArg("closeMs")) {
      const uint32_t v = (uint32_t)server.arg("closeMs").toInt();
      g_valveDriveCloseMs = clampMs(v);
      g_cfg.valveDriveCloseMs = g_valveDriveCloseMs;
    }

    const bool ok = saveConfig();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  // Relay polarity control (for boards without H/L jumper)
  // GET returns activeLow bool. POST activeLow=0/1 updates and persists.
  server.on("/api/relay/polarity", HTTP_GET, []() {
    JsonDocument doc;
    doc["activeLow"] = g_relayActiveLow;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/relay/polarity", HTTP_POST, []() {
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
    if (!server.hasArg("activeLow")) {
      sendText(400, "missing activeLow");
      return;
    }
    const int v = server.arg("activeLow").toInt();
    g_relayActiveLow = (v != 0);
    relayValveStop();
    const bool ok = saveConfig();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  // Raw relay/GPIO test (bypasses valve logic) to debug "IN1 LED stuck on".
  // POST /api/relay/test?pin=7&level=0|1
  // Only allows the relay pins for safety.
  server.on("/api/relay/test", HTTP_POST, []() {
    if (!server.hasArg("pin") || !server.hasArg("level")) {
      sendText(400, "missing pin/level");
      return;
    }
    const int pin = server.arg("pin").toInt();
    if (pin != RELAY_OPEN_PIN && pin != RELAY_CLOSE_PIN) {
      sendText(400, "pin not allowed");
      return;
    }
    const int level = server.arg("level").toInt() ? HIGH : LOW;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
    JsonDocument doc;
    doc["ok"] = true;
    doc["pin"] = pin;
    doc["level"] = (level == HIGH) ? 1 : 0;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Diagnostics: RS485 removed (valve is relay-controlled)
  // RS485 diagnostics removed (valve now driven via relays)
  // Start a batch: productId + gallons. Uses sensor1 pulses.
  server.on("/api/batch/start", HTTP_POST, []() {
    if (!server.hasArg("productId") || !server.hasArg("gallons")) {
      sendText(400, "missing productId/gallons");
      return;
    }
    if (g_batchState != BatchState::Idle) {
      sendText(409, "batch already running");
      return;
    }

    const int pid = server.arg("productId").toInt();
    if (pid < 0 || (size_t)pid >= g_cfg.productCount) {
      sendText(400, "invalid productId");
      return;
    }
    const float gallons = server.arg("gallons").toFloat();
    if (!(gallons > 0.0f)) {
      sendText(400, "invalid gallons");
      return;
    }

    const Product& p = g_cfg.products[(size_t)pid];
    const float ppg = productPulsesPerGallon(p);
    if (!(ppg > 0.0f)) {
      sendText(400, "missing pulsesPerGallon (set product or calibration)");
      return;
    }

    const uint32_t target = (uint32_t)(gallons * ppg);
    g_batchProductId = pid;
    g_batchTargetPulses = target;
    g_batchStartPulses = readValue(g_pulses1);
    g_batchStopAtMs = 0;
    g_batchClosedAtMs = 0;

    valveWrite(true);
    g_batchState = BatchState::Running;

    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/batch/stop", HTTP_POST, []() {
    batchStopInternal();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // Calibration: set global pulses/gal
  server.on("/api/calibration", HTTP_POST, []() {
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
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
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
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
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
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
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
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

  // Config debug helpers
  server.on("/api/config/get", HTTP_GET, []() {
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
    if (!LittleFS.exists(CONFIG_PATH)) {
      sendText(404, "config not found");
      return;
    }
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
      sendText(500, "config open failed");
      return;
    }
    server.streamFile(f, "application/json");
    f.close();
  });

  server.on("/api/config/erase", HTTP_POST, []() {
    if (!ensureFS()) {
      sendText(500, "filesystem not mounted");
      return;
    }
    if (LittleFS.exists(CONFIG_PATH)) {
      if (!LittleFS.remove(CONFIG_PATH)) {
        sendText(500, "config remove failed");
        return;
      }
    }
    g_cfg = Config();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // Useful for debugging from browser
  server.on("/last", HTTP_GET, []() {
    String body = String("{\"last\":\"") + (g_lastKey ? String((char)g_lastKey) : String("")) + "\"}";
    server.send(200, "application/json", body);
  });

  server.on("/api/version", HTTP_GET, []() {
    sendNoCacheHeaders();
    String body = String("{\"uiVersion\":\"") + UI_VERSION + "\"}";
    server.send(200, "application/json", body);
  });

  // Static file serving from LittleFS (data/ uploaded to the device)
  // Some WebServer builds don't support cache control on serveStatic, so we stream
  // these assets manually with no-cache headers.
  auto streamAsset = [&](const char* uri, const char* path, const char* contentType, bool noCache) {
    server.on(uri, HTTP_GET, [=]() {
      if (!ensureFS()) {
        sendText(500, "filesystem not mounted");
        return;
      }
      if (!LittleFS.exists(path)) {
        sendText(404, "not found");
        return;
      }
      File f = LittleFS.open(path, "r");
      if (noCache) sendNoCacheHeaders();
      else sendShortCacheHeaders();
      server.streamFile(f, contentType);
      f.close();
    });
  };

  streamAsset("/index.html", "/index.html", "text/html", true);
  streamAsset("/setup.html", "/setup.html", "text/html", true);
  streamAsset("/run.html", "/run.html", "text/html", true);
  streamAsset("/diagnostics.html", "/diagnostics.html", "text/html", true);
  streamAsset("/app.js", "/app.js", "application/javascript", false);
  streamAsset("/styles.css", "/styles.css", "text/css", false);

  // Fallback: try to serve the requested path from filesystem
  server.onNotFound([]() {
    String path = server.uri();
    // tolerate cache-busting querystrings like /app.js?v=...
    const int q = path.indexOf('?');
    if (q >= 0) path = path.substring(0, q);

    if (LittleFS.exists(path)) {
      String contentType = "text/plain";
      if (path.endsWith(".html")) contentType = "text/html";
      else if (path.endsWith(".css")) contentType = "text/css";
      else if (path.endsWith(".js")) contentType = "application/javascript";
      else if (path.endsWith(".json")) contentType = "application/json";

      File f = LittleFS.open(path, "r");
      if (path.endsWith(".html")) sendNoCacheHeaders();
      else sendShortCacheHeaders();
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

  // Debug heartbeat: helps confirm WiFi/AP state even if the UI can't connect.
  serialPeriodicWiFiStatus();

  // Relay valve auto-stop
  if (g_valveDriveUntilMs != 0 && (int32_t)(nowMs() - g_valveDriveUntilMs) >= 0) {
    relayValveStop();
  }

  // Batch state machine
  const BatchState st = g_batchState;
  if (st == BatchState::Running) {
    const uint32_t cur = readValue(g_pulses1);
    const uint32_t delta = cur - g_batchStartPulses;
    if (delta >= g_batchTargetPulses) {
      valveWrite(false);
      g_batchStopAtMs = nowMs();

      // close-time compensation (remain in closing delay for valveCloseTimeMs)
      const int pid = g_batchProductId;
      uint32_t closeMs = 0;
      if (pid >= 0 && (size_t)pid < g_cfg.productCount) {
        closeMs = g_cfg.products[(size_t)pid].valveCloseTimeMs;
      }
      if (closeMs > 0) {
        g_batchClosedAtMs = g_batchStopAtMs + closeMs;
        g_batchState = BatchState::ClosingDelay;
      } else {
        g_batchState = BatchState::Done;
      }
    }
  } else if (st == BatchState::ClosingDelay) {
    if ((int32_t)(nowMs() - g_batchClosedAtMs) >= 0) {
      g_batchState = BatchState::Done;
    }
  } else if (st == BatchState::Done) {
    // Auto-return to idle after a short moment so UI can show DONE
    static uint32_t doneSince = 0;
    if (doneSince == 0) doneSince = nowMs();
    if ((int32_t)(nowMs() - doneSince) > 2000) {
      doneSince = 0;
      batchStopInternal();
    }
  }
}
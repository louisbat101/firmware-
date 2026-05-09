#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// --- RS485 / Modbus RTU (proportional valve) ---
#include <HardwareSerial.h>

// This project runs as a Wi-Fi hotspot (AP mode). Connect your phone/laptop to this SSID.
const char* apSsid = "ESP32C3-Keypad";
const char* apPassword = "12345678"; // 8+ chars

WebServer server(80);

static volatile char g_lastKey = 0;

// Flow inputs (hall sensors)
static const int FLOW_PIN_1 = 2;
static const int FLOW_PIN_2 = 3;

// Valve control (set to a real pin when you wire it)
// This build uses a Modbus-RTU proportional valve over RS485.
// Wiring (ESP32-C3): TX=GPIO4, RX=GPIO5, CN(DE/RE)=GPIO6
static const int RS485_TX_PIN = 4;
static const int RS485_RX_PIN = 5;
static const int RS485_DE_PIN = 6; // CN pin controls TX enable (DE/RE)

static const uint32_t RS485_BAUD = 9600; // datasheet: 9600 or 57600
static const uint8_t MODBUS_ADDR = 0x01; // default device address

// Valve positions are in degrees * 100: 0..9000 (0.00°..90.00°)
static const int16_t VALVE_POS_OPEN = 0;     // 0° open
static const int16_t VALVE_POS_CLOSED = 9000; // 90° closed

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
  // Map the legacy open/close semantic to the proportional valve's position set.
  // Note: This is synchronous and may take a few ms; acceptable for our state machine.
  const int16_t target = open ? VALVE_POS_OPEN : VALVE_POS_CLOSED;
  (void)valveSetPositionDeg100(target);
}

// ---------------------------
// Modbus RTU helpers
// ---------------------------

static HardwareSerial RS485(1);

static void rs485SetTx(bool enable) {
  // Many RS485 modules use DE+RE tied together.
  digitalWrite(RS485_DE_PIN, enable ? HIGH : LOW);
  // Small settle time helps some transceivers
  delayMicroseconds(50);
}

static uint16_t modbusCrc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

static void modbusDrainRx() {
  while (RS485.available() > 0) (void)RS485.read();
}

static bool modbusSendAndRead(const uint8_t* req, size_t reqLen, uint8_t* resp, size_t respCap, size_t* outRespLen,
                              uint32_t timeoutMs = 200) {
  if (outRespLen) *outRespLen = 0;

  modbusDrainRx();

  rs485SetTx(true);
  RS485.write(req, reqLen);
  RS485.flush();
  rs485SetTx(false);

  const uint32_t start = millis();
  size_t n = 0;
  while ((millis() - start) < timeoutMs) {
    while (RS485.available() > 0) {
      const int c = RS485.read();
      if (c < 0) break;
      if (n < respCap) resp[n++] = (uint8_t)c;
    }

    // Heuristic: if we have at least address+func+crc and no new bytes for a bit, stop.
    if (n >= 5) {
      // quick idle wait to accumulate remaining bytes
      delay(2);
      if (RS485.available() == 0) break;
    }
    delay(1);
  }

  if (outRespLen) *outRespLen = n;
  if (n < 5) return false;

  const uint16_t gotCrc = (uint16_t)resp[n - 2] | ((uint16_t)resp[n - 1] << 8);
  const uint16_t calcCrc = modbusCrc16(resp, n - 2);
  if (gotCrc != calcCrc) return false;

  return true;
}

static bool modbusReadHoldingRegisters(uint8_t addr, uint16_t startReg, uint16_t regCount, uint16_t* outRegs) {
  if (regCount == 0 || regCount > 125) return false;

  uint8_t req[8];
  req[0] = addr;
  req[1] = 0x03;
  req[2] = (uint8_t)(startReg >> 8);
  req[3] = (uint8_t)(startReg & 0xFF);
  req[4] = (uint8_t)(regCount >> 8);
  req[5] = (uint8_t)(regCount & 0xFF);
  const uint16_t crc = modbusCrc16(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)(crc >> 8);

  uint8_t resp[256];
  size_t n = 0;
  if (!modbusSendAndRead(req, sizeof(req), resp, sizeof(resp), &n)) return false;

  if (resp[0] != addr) return false;
  if (resp[1] != 0x03) return false;
  const uint8_t byteCount = resp[2];
  if (byteCount != regCount * 2) return false;
  if (n < (size_t)(3 + byteCount + 2)) return false;

  for (uint16_t i = 0; i < regCount; i++) {
    const uint8_t hi = resp[3 + i * 2];
    const uint8_t lo = resp[3 + i * 2 + 1];
    outRegs[i] = (uint16_t)hi << 8 | lo;
  }
  return true;
}

static bool modbusWriteMultipleRegisters(uint8_t addr, uint16_t startReg, uint16_t regCount, const uint16_t* regs) {
  if (regCount == 0 || regCount > 123) return false;
  const uint8_t byteCount = (uint8_t)(regCount * 2);
  if (byteCount + 9 > 255) return false;

  uint8_t req[255];
  size_t idx = 0;
  req[idx++] = addr;
  req[idx++] = 0x10;
  req[idx++] = (uint8_t)(startReg >> 8);
  req[idx++] = (uint8_t)(startReg & 0xFF);
  req[idx++] = (uint8_t)(regCount >> 8);
  req[idx++] = (uint8_t)(regCount & 0xFF);
  req[idx++] = byteCount;
  for (uint16_t i = 0; i < regCount; i++) {
    req[idx++] = (uint8_t)(regs[i] >> 8);
    req[idx++] = (uint8_t)(regs[i] & 0xFF);
  }
  const uint16_t crc = modbusCrc16(req, idx);
  req[idx++] = (uint8_t)(crc & 0xFF);
  req[idx++] = (uint8_t)(crc >> 8);

  uint8_t resp[16];
  size_t n = 0;
  if (!modbusSendAndRead(req, idx, resp, sizeof(resp), &n)) return false;

  // Expected response: addr 0x10 startHi startLo countHi countLo crcLo crcHi
  if (n < 8) return false;
  if (resp[0] != addr) return false;
  if (resp[1] != 0x10) return false;
  const uint16_t rStart = (uint16_t)resp[2] << 8 | resp[3];
  const uint16_t rCount = (uint16_t)resp[4] << 8 | resp[5];
  if (rStart != startReg) return false;
  if (rCount != regCount) return false;
  return true;
}

// Register map from your doc
static const uint16_t REG_DEVICE_ADDR = 0x0000;
static const uint16_t REG_POSITION_SET = 0x0001;
static const uint16_t REG_ERROR_STATE = 0x0002;
static const uint16_t REG_POSITION_FEEDBACK = 0x0003;

static bool valveSetPositionDeg100(int16_t deg100) {
  // Clamp to device documented range (we only use 0..9000 normally)
  if (deg100 < -10500) deg100 = -10500;
  if (deg100 > 19500) deg100 = 19500;

  const uint16_t v = (uint16_t)deg100; // keep two's complement bit pattern
  return modbusWriteMultipleRegisters(MODBUS_ADDR, REG_POSITION_SET, 1, &v);
}

static bool valveReadPositionDeg100(int16_t* outDeg100) {
  uint16_t r = 0;
  const bool ok = modbusReadHoldingRegisters(MODBUS_ADDR, REG_POSITION_FEEDBACK, 1, &r);
  if (!ok) return false;
  if (outDeg100) *outDeg100 = (int16_t)r;
  return true;
}

static bool valveReadError(uint16_t* outErr) {
  uint16_t r = 0;
  const bool ok = modbusReadHoldingRegisters(MODBUS_ADDR, REG_ERROR_STATE, 1, &r);
  if (!ok) return false;
  if (outErr) *outErr = r;
  return true;
}

static bool valveClearError() {
  const uint16_t zero = 0;
  return modbusWriteMultipleRegisters(MODBUS_ADDR, REG_ERROR_STATE, 1, &zero);
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

  // RS485 / Modbus setup
  pinMode(RS485_DE_PIN, OUTPUT);
  rs485SetTx(false); // receive mode
  // UART1 on ESP32-C3 can be mapped to arbitrary GPIOs.
  RS485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed. Did you upload the data/ folder?");
  }

  loadConfig();

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

    const BatchState st = g_batchState;
    const uint32_t cur = readValue(g_pulses1);
    doc["batch"] ["state"] = (uint32_t)st;
    doc["batch"] ["productId"] = (int)g_batchProductId;
    doc["batch"] ["targetPulses"] = (uint32_t)g_batchTargetPulses;
    doc["batch"] ["startPulses"] = (uint32_t)g_batchStartPulses;
    doc["batch"] ["currentPulses"] = (uint32_t)cur;
    doc["batch"] ["stopAtMs"] = (uint32_t)g_batchStopAtMs;
    doc["batch"] ["closedAtMs"] = (uint32_t)g_batchClosedAtMs;

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
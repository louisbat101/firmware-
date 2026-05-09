#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

// This project runs as a Wi-Fi hotspot (AP mode). Connect your phone/laptop to this SSID.
const char* apSsid = "ESP32C3-Keypad";
const char* apPassword = "12345678"; // 8+ chars

WebServer server(80);

static volatile char g_lastKey = 0;

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

  // Receive keypad presses: /keypress?value=1
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
    // No content
    server.send(204, "text/plain", "");
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
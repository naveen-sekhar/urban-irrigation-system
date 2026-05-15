/*
  Urban Irrigation System - ESP32
  Sensors : DHT22 (GPIO4), Soil Moisture Analog (GPIO34)
  Relay    : GPIO26
  Web Server:  http://<device-ip>/         → live dashboard
               http://<device-ip>/stats    → performance & system stats
  MQTT:        publishes to broker every 5 s
               topics: irrigation/sensor/temperature
                       irrigation/sensor/humidity
                       irrigation/sensor/soil_moisture
                       irrigation/sensor/soil_raw
                       irrigation/control/pump
                       irrigation/stats/latency
                       irrigation/stats/pdr
                       irrigation/stats/throughput

  Libraries needed (Arduino Library Manager):
    - DHT sensor library  (Adafruit)
    - Adafruit Unified Sensor
    - PubSubClient         (Nick O'Leary)  ← for MQTT
    - WiFi  (built-in with ESP32 board package)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <PubSubClient.h>    // MQTT

// ── WiFi credentials ──────────────────────────────────────────────────────────
const char* SSID     = "1234567890";
const char* PASSWORD = "Naveen@147";

// ── MQTT broker ───────────────────────────────────────────────────────────────
// Option A: public test broker (no setup needed, visible to anyone)
const char* MQTT_HOST   = "broker.hivemq.com";
// Option B: local Mosquitto on your PC → use your PC's LAN IP, e.g. "192.168.1.10"
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT = "esp32-irrigation-01";   // must be unique per device

// ── Pin definitions ───────────────────────────────────────────────────────────
#define DHT_PIN   4
#define SOIL_PIN  34
#define RELAY_PIN 26

#define RELAY_ON_LEVEL  HIGH
#define RELAY_OFF_LEVEL LOW

// ── DHT22 ─────────────────────────────────────────────────────────────────────
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ── Servers ───────────────────────────────────────────────────────────────────
WebServer    server(80);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ── Thresholds ────────────────────────────────────────────────────────────────
const float TEMP_HIGH_THRESHOLD        = 35.0;
const int   SOIL_DRY_PERCENT_THRESHOLD = 5;
const int   SOIL_WET_PERCENT_THRESHOLD = 15;

// ── Calibration (adjust after measuring your sensor in dry air and wet soil) ──
const int SOIL_DRY_ADC = 100;
const int SOIL_WET_ADC = 3000;

// ── Performance metrics ───────────────────────────────────────────────────────
unsigned long requestCount     = 0;
unsigned long totalLatencyMs   = 0;
unsigned long deliveredPackets = 0;
unsigned long totalPackets     = 0;
unsigned long lastThroughputMs = 0;
unsigned long bytesServedLast  = 0;
unsigned long bytesServedTotal = 0;
float         throughputKBps   = 0.0;

// MQTT metrics
unsigned long mqttPublished  = 0;
unsigned long mqttFailed     = 0;
unsigned long mqttReconnects = 0;

// ── Sensor state ──────────────────────────────────────────────────────────────
float temperature  = 0.0;
float humidity     = 0.0;
int   soilRaw      = 0;
int   soilPercent  = 0;
bool  pumpOn       = false;

// ── Timing ────────────────────────────────────────────────────────────────────
unsigned long lastSensorRead = 0;
unsigned long lastMqttPub    = 0;

// ── Irrigation counters ───────────────────────────────────────────────────────
unsigned long irrigationCycles = 0;
unsigned long pumpOnMs         = 0;
unsigned long pumpStartTime    = 0;

// ── Boot time ─────────────────────────────────────────────────────────────────
unsigned long bootMs = 0;

// =============================================================================
// Helpers
// =============================================================================

int readSoilRaw() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(SOIL_PIN); delay(5); }
  return (int)(sum / 10);
}

int soilToPercent(int raw) {
  if (SOIL_DRY_ADC == SOIL_WET_ADC) return 0;
  return constrain(map(raw, SOIL_DRY_ADC, SOIL_WET_ADC, 0, 100), 0, 100);
}

// =============================================================================
// Pump control
// =============================================================================

void setPump(bool on) {
  if (on && !pumpOn) { pumpStartTime = millis(); irrigationCycles++; }
  if (!on && pumpOn) { pumpOnMs += millis() - pumpStartTime; }
  pumpOn = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// =============================================================================
// Sensor update + pump logic
// =============================================================================

void updateSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;

  soilRaw     = readSoilRaw();
  soilPercent = soilToPercent(soilRaw);

  bool soilDry   = soilPercent <= SOIL_DRY_PERCENT_THRESHOLD;
  bool hotAndDry = (temperature > TEMP_HIGH_THRESHOLD) && (humidity < 40.0);

  if (soilDry || hotAndDry) {
    if (!pumpOn) setPump(true);
  } else if (pumpOn && soilPercent >= SOIL_WET_PERCENT_THRESHOLD) {
    setPump(false);
  }
}

// =============================================================================
// MQTT
// =============================================================================

bool mqttConnect() {
  if (mqtt.connected()) return true;
  Serial.print("[MQTT] Connecting...");
  bool ok = mqtt.connect(MQTT_CLIENT);
  if (ok) {
    Serial.println(" connected");
    mqttReconnects++;
  } else {
    Serial.print(" failed, rc="); Serial.println(mqtt.state());
  }
  return ok;
}

// Publish a single topic; returns true on success
bool mqttPublish(const char* topic, String payload, bool retain = true) {
  bool ok = mqtt.publish(topic, payload.c_str(), retain);
  if (ok) mqttPublished++; else mqttFailed++;
  return ok;
}

void publishMqtt() {
  if (!mqttConnect()) return;

  unsigned long avgLatency = (requestCount > 0) ? (totalLatencyMs / requestCount) : 0;
  float pdr = (totalPackets > 0) ? (100.0 * deliveredPackets / totalPackets) : 100.0;

  mqttPublish("irrigation/sensor/temperature",  String(temperature, 1));
  mqttPublish("irrigation/sensor/humidity",     String(humidity, 1));
  mqttPublish("irrigation/sensor/soil_moisture",String(soilPercent));
  mqttPublish("irrigation/sensor/soil_raw",     String(soilRaw));
  mqttPublish("irrigation/control/pump",        pumpOn ? "ON" : "OFF");
  mqttPublish("irrigation/stats/latency_ms",    String(avgLatency));
  mqttPublish("irrigation/stats/pdr_pct",       String(pdr, 1));
  mqttPublish("irrigation/stats/throughput_kbps",String(throughputKBps, 2));

  Serial.printf("[MQTT] Published. Total OK=%lu  Failed=%lu\n", mqttPublished, mqttFailed);
}

// =============================================================================
// Shared CSS snippet (used by both pages)
// =============================================================================

const char* COMMON_CSS = R"css(
  :root{
    --green:#1D9E75;--green-lt:#E1F5EE;--green-dk:#085041;
    --blue:#378ADD;--blue-lt:#E6F1FB;
    --amber:#BA7517;--amber-lt:#FAEEDA;
    --red:#E24B4A;--red-lt:#FCEBEB;
    --gray:#888780;--gray-lt:#F1EFE8;
    --bg:#f8f8f7;--card:#ffffff;
    --border:rgba(0,0,0,0.10);
    --text:#2c2c2a;--muted:#5f5e5a;
    --radius:12px;
  }
  *{box-sizing:border-box;margin:0;padding:0;}
  body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);padding:20px;}
  h1{font-size:22px;font-weight:500;margin-bottom:4px;}
  .subtitle{font-size:13px;color:var(--muted);margin-bottom:24px;}
  nav a{font-size:13px;margin-right:18px;color:var(--blue);text-decoration:none;}
  nav a:hover{text-decoration:underline;}
  nav{margin-bottom:20px;}
  .section-title{font-size:13px;font-weight:500;color:var(--muted);
    text-transform:uppercase;letter-spacing:.06em;margin:24px 0 12px;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:12px;}
  .card{background:var(--card);border:0.5px solid var(--border);
    border-radius:var(--radius);padding:16px 18px;}
  .card .label{font-size:12px;color:var(--muted);margin-bottom:6px;}
  .card .value{font-size:26px;font-weight:500;}
  .card .unit{font-size:13px;color:var(--muted);margin-left:2px;}
  .card .bar-bg{background:var(--gray-lt);border-radius:6px;height:6px;margin-top:10px;overflow:hidden;}
  .card .bar-fill{height:100%;border-radius:6px;transition:width .5s;}
  .pump-banner{border-radius:var(--radius);padding:16px 20px;
    display:flex;align-items:center;gap:14px;border:0.5px solid var(--border);}
  .pump-dot{width:14px;height:14px;border-radius:50%;flex-shrink:0;}
  .pump-label{font-size:16px;font-weight:500;}
  .pump-sub{font-size:12px;color:var(--muted);margin-top:2px;}
  .metric-card{background:var(--gray-lt);border-radius:var(--radius);padding:14px 16px;}
  .metric-card .m-label{font-size:11px;color:var(--muted);margin-bottom:4px;}
  .metric-card .m-value{font-size:20px;font-weight:500;}
  .metric-card .m-unit{font-size:12px;color:var(--muted);}
  .metric-card .m-desc{font-size:11px;color:var(--muted);margin-top:4px;}
  .stat-row{display:flex;justify-content:space-between;align-items:center;
    padding:9px 12px;border-bottom:0.5px solid var(--border);font-size:13px;}
  .stat-row:last-child{border-bottom:none;}
  .stat-label{color:var(--muted);}
  .stat-value{font-weight:500;font-family:monospace;}
  .stat-table{background:var(--card);border:0.5px solid var(--border);border-radius:var(--radius);overflow:hidden;}
  .badge{display:inline-block;padding:2px 8px;border-radius:20px;font-size:11px;font-weight:500;}
  .badge-green{background:var(--green-lt);color:var(--green-dk);}
  .badge-amber{background:var(--amber-lt);color:#633806;}
  .badge-red{background:var(--red-lt);color:#791F1F;}
  .refresh-note{font-size:11px;color:var(--muted);margin-top:20px;text-align:center;}
)css";

// =============================================================================
// Dashboard page  GET /
// =============================================================================

String buildDashboard() {
  unsigned long avgLatency = (requestCount > 0) ? (totalLatencyMs / requestCount) : 0;
  float pdr = (totalPackets > 0) ? (100.0 * deliveredPackets / totalPackets) : 100.0;
  float pumpOnSec = (pumpOnMs + (pumpOn ? (millis() - pumpStartTime) : 0)) / 1000.0;

  String html = "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='3'>"
    "<title>Irrigation System</title>"
    "<style>" + String(COMMON_CSS) + "</style></head><body>";

  html += "<h1>&#127807; Urban Irrigation System</h1>";
  html += "<p class='subtitle'>ESP32 IoT Controller &nbsp;&middot;&nbsp; Auto-refreshes every 3 s</p>";
  html += "<nav><a href='/'>Dashboard</a><a href='/stats'>System Stats</a></nav>";

  // Pump banner
  String pumpColor   = pumpOn ? "#1D9E75" : "#888780";
  String pumpBgColor = pumpOn ? "#E1F5EE" : "#F1EFE8";
  String pumpText    = pumpOn ? "Pump is ON &mdash; Irrigating" : "Pump is OFF &mdash; Soil OK";
  String pumpSub     = "Cycles: " + String(irrigationCycles)
                     + " &nbsp;|&nbsp; Total on-time: " + String((int)pumpOnSec) + " s";
  html += "<div class='pump-banner' style='background:" + pumpBgColor + ";border-color:" + pumpColor + "'>";
  html += "<div class='pump-dot' style='background:" + pumpColor + "'></div>";
  html += "<div><div class='pump-label' style='color:" + String(pumpOn ? "#085041" : "#5f5e5a") + "'>"
        + pumpText + "</div><div class='pump-sub'>" + pumpSub + "</div></div></div>";

  // Sensor cards
  html += "<p class='section-title'>Sensor readings</p><div class='grid'>";

  String tColor = (temperature > TEMP_HIGH_THRESHOLD) ? "#D85A30" : "#378ADD";
  int    tPct   = constrain((int)map((int)temperature, 0, 50, 0, 100), 0, 100);
  html += "<div class='card'><div class='label'>Temperature</div>"
          "<div class='value' style='color:" + tColor + "'>" + String(temperature, 1)
        + "<span class='unit'>&deg;C</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String(tPct) + "%;background:" + tColor + "'></div></div></div>";

  String hColor = (humidity < 40.0) ? "#D85A30" : "#378ADD";
  html += "<div class='card'><div class='label'>Humidity</div>"
          "<div class='value' style='color:" + hColor + "'>" + String(humidity, 1)
        + "<span class='unit'>%</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String((int)humidity) + "%;background:" + hColor + "'></div></div></div>";

  String sColor = (soilPercent < 30) ? "#D85A30" : "#1D9E75";
  html += "<div class='card'><div class='label'>Soil Moisture</div>"
          "<div class='value' style='color:" + sColor + "'>" + String(soilPercent)
        + "<span class='unit'>%</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String(soilPercent) + "%;background:" + sColor + "'></div></div></div>";

  html += "<div class='card'><div class='label'>Soil ADC (raw)</div>"
          "<div class='value'>" + String(soilRaw) + "<span class='unit'>/4095</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String(soilRaw * 100 / 4095) + "%;background:var(--amber)'></div></div></div>";
  html += "</div>";

  // Performance metric cards
  html += "<p class='section-title'>Performance metrics</p><div class='grid'>";
  html += "<div class='metric-card'><div class='m-label'>Avg. Response Latency</div>"
          "<div class='m-value'>" + String(avgLatency) + "<span class='m-unit'> ms</span></div>"
          "<div class='m-desc'>HTTP page build time</div></div>";
  html += "<div class='metric-card'><div class='m-label'>Throughput</div>"
          "<div class='m-value'>" + String(throughputKBps, 1) + "<span class='m-unit'> KB/s</span></div>"
          "<div class='m-desc'>Data served / 5 s window</div></div>";
  html += "<div class='metric-card'><div class='m-label'>Packet Delivery Rate</div>"
          "<div class='m-value'>" + String(pdr, 1) + "<span class='m-unit'> %</span></div>"
          "<div class='m-desc'>HTTP requests answered</div></div>";
  html += "<div class='metric-card'><div class='m-label'>Total Requests</div>"
          "<div class='m-value'>" + String(requestCount) + "</div>"
          "<div class='m-desc'>Since last boot</div></div>";
  html += "</div>";

  html += "<p class='refresh-note'>Auto-refreshes every 3 s &nbsp;&middot;&nbsp; "
          "<a href='/stats' style='color:var(--blue)'>View system stats &rarr;</a></p>";
  html += "</body></html>";
  return html;
}

// =============================================================================
// Stats page  GET /stats
// =============================================================================

String buildStats() {
  unsigned long uptimeSec  = (millis() - bootMs) / 1000;
  unsigned long uptimeMin  = uptimeSec / 60;
  unsigned long uptimeHour = uptimeMin / 60;
  unsigned long avgLatency = (requestCount > 0) ? (totalLatencyMs / requestCount) : 0;
  float pdr      = (totalPackets > 0) ? (100.0 * deliveredPackets / totalPackets) : 100.0;
  float mqttPdr  = ((mqttPublished + mqttFailed) > 0)
                 ? (100.0 * mqttPublished / (mqttPublished + mqttFailed)) : 100.0;
  float pumpOnSec = (pumpOnMs + (pumpOn ? (millis() - pumpStartTime) : 0)) / 1000.0;

  String html = "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>System Stats</title>"
    "<style>" + String(COMMON_CSS) + "</style></head><body>";

  html += "<h1>&#128202; System Stats</h1>";
  html += "<p class='subtitle'>ESP32 Irrigation Controller &nbsp;&middot;&nbsp; Refreshes every 5 s</p>";
  html += "<nav><a href='/'>Dashboard</a><a href='/stats'>System Stats</a></nav>";

  // ── Network / HTTP metrics ──────────────────────────────────────────────────
  html += "<p class='section-title'>HTTP performance</p>";
  html += "<div class='stat-table'>";

  auto statRow = [](String label, String value, String badge = "") -> String {
    return "<div class='stat-row'><span class='stat-label'>" + label + "</span>"
           "<span>" + (badge.length() ? "<span class='badge " + badge + "'>" + value + "</span>" : "<span class='stat-value'>" + value + "</span>") + "</span></div>";
  };

  html += statRow("Avg. response latency",    String(avgLatency) + " ms");
  html += statRow("Throughput (5 s window)",  String(throughputKBps, 2) + " KB/s");
  html += statRow("Total bytes served",       String(bytesServedTotal / 1024.0, 1) + " KB");
  html += statRow("Total HTTP requests",      String(totalPackets));
  html += statRow("Delivered (200 OK)",       String(deliveredPackets));
  html += statRow("Packet delivery rate",     String(pdr, 1) + " %",
                  pdr >= 99.0 ? "badge-green" : (pdr >= 90.0 ? "badge-amber" : "badge-red"));
  html += "</div>";

  // ── MQTT metrics ─────────────────────────────────────────────────────────────
  html += "<p class='section-title'>MQTT performance</p>";
  html += "<div class='stat-table'>";
  html += statRow("Broker",                    String(MQTT_HOST) + ":" + String(MQTT_PORT));
  html += statRow("Client ID",                 String(MQTT_CLIENT));
  html += statRow("Broker connected",          mqtt.connected() ? "Yes" : "No",
                  mqtt.connected() ? "badge-green" : "badge-red");
  html += statRow("Messages published (OK)",   String(mqttPublished));
  html += statRow("Messages failed",           String(mqttFailed));
  html += statRow("MQTT delivery rate",        String(mqttPdr, 1) + " %",
                  mqttPdr >= 99.0 ? "badge-green" : (mqttPdr >= 90.0 ? "badge-amber" : "badge-red"));
  html += statRow("Reconnect attempts",        String(mqttReconnects));
  html += "</div>";

  // ── System / heap ─────────────────────────────────────────────────────────────
  html += "<p class='section-title'>System</p>";
  html += "<div class='stat-table'>";
  html += statRow("Uptime",                    String(uptimeHour) + "h " + String(uptimeMin % 60) + "m " + String(uptimeSec % 60) + "s");
  html += statRow("Free heap",                 String(ESP.getFreeHeap()) + " bytes");
  html += statRow("Total heap",                String(ESP.getHeapSize()) + " bytes");
  html += statRow("Min free heap (ever)",      String(ESP.getMinFreeHeap()) + " bytes");
  html += statRow("Max alloc heap",            String(ESP.getMaxAllocHeap()) + " bytes");
  html += statRow("Chip model",                String(ESP.getChipModel()));
  html += statRow("CPU freq.",                 String(ESP.getCpuFreqMHz()) + " MHz");
  html += statRow("Flash size",                String(ESP.getFlashChipSize() / 1024) + " KB");
  html += statRow("WiFi RSSI",                 String(WiFi.RSSI()) + " dBm");
  html += statRow("IP address",                WiFi.localIP().toString());
  html += statRow("MAC address",               WiFi.macAddress());
  html += "</div>";

  // ── Irrigation ────────────────────────────────────────────────────────────────
  html += "<p class='section-title'>Irrigation</p>";
  html += "<div class='stat-table'>";
  html += statRow("Pump state",           pumpOn ? "ON" : "OFF",
                  pumpOn ? "badge-green" : "");
  html += statRow("Irrigation cycles",    String(irrigationCycles));
  html += statRow("Total pump on-time",   String((int)pumpOnSec) + " s");
  html += statRow("Last soil raw ADC",    String(soilRaw));
  html += statRow("Last soil moisture",   String(soilPercent) + " %");
  html += statRow("Last temperature",     String(temperature, 1) + " &deg;C");
  html += statRow("Last humidity",        String(humidity, 1) + " %");
  html += "</div>";

  html += "<p class='refresh-note'>Auto-refreshes every 5 s &nbsp;&middot;&nbsp; "
          "<a href='/' style='color:var(--blue)'>&larr; Back to dashboard</a></p>";
  html += "</body></html>";
  return html;
}

// =============================================================================
// HTTP route handlers
// =============================================================================

void handleRoot() {
  totalPackets++;
  unsigned long t0 = millis();
  String page = buildDashboard();
  totalLatencyMs += millis() - t0;
  requestCount++;
  deliveredPackets++;
  bytesServedTotal += page.length();
  server.send(200, "text/html", page);
}

void handleStats() {
  totalPackets++;
  unsigned long t0 = millis();
  String page = buildStats();
  totalLatencyMs += millis() - t0;
  requestCount++;
  deliveredPackets++;
  bytesServedTotal += page.length();
  server.send(200, "text/html", page);
}

void handleNotFound() {
  totalPackets++;
  server.send(404, "text/plain", "Not found");
}

// =============================================================================
// Setup
// =============================================================================

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  dht.begin();

  // WiFi
  Serial.print("Connecting to WiFi: "); Serial.println(SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.println("  /        → live dashboard");
  Serial.println("  /stats   → system stats");

  // MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqttConnect();

  // HTTP routes
  server.on("/",       handleRoot);
  server.on("/stats",  handleStats);
  server.onNotFound(   handleNotFound);
  server.begin();

  bootMs         = millis();
  lastSensorRead = millis();
  lastThroughputMs = millis();
}

// =============================================================================
// Loop
// =============================================================================

void loop() {
  server.handleClient();
  mqtt.loop();   // keeps MQTT connection alive

  // Read sensors every 2 s
  if (millis() - lastSensorRead >= 2000) {
    lastSensorRead = millis();
    updateSensors();
    Serial.printf("[Sensors] T=%.1f°C  H=%.1f%%  Soil=%d%%(%d)  Pump=%s\n",
                  temperature, humidity, soilPercent, soilRaw, pumpOn ? "ON" : "OFF");
  }

  // Publish MQTT every 5 s
  if (millis() - lastMqttPub >= 5000) {
    lastMqttPub = millis();
    publishMqtt();
  }

  // Throughput window every 5 s
  if (millis() - lastThroughputMs >= 5000) {
    unsigned long dt = millis() - lastThroughputMs;
    throughputKBps = ((bytesServedTotal - bytesServedLast) / 1024.0) / (dt / 1000.0);
    bytesServedLast  = bytesServedTotal;
    lastThroughputMs = millis();
  }
}

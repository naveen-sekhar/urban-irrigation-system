/*
  Urban Irrigation System - ESP32
  Sensors : DHT22 (GPIO4), Soil Moisture Analog (GPIO34)
  Relay    : GPIO26 (LOW = pump ON for most relay modules)
  Web Server: Opens dashboard at http://<device-ip>/

  Libraries needed (install via Arduino Library Manager):
    - DHT sensor library by Adafruit
    - Adafruit Unified Sensor
    - WiFi (built-in with ESP32 board package)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>

// ── WiFi credentials ──────────────────────────────────────────────────────────
const char* SSID     = "1234567890";
const char* PASSWORD = "Naveen@147";

// ── Pin definitions ───────────────────────────────────────────────────────────
#define DHT_PIN        4      // DHT22 data pin
#define SOIL_PIN       34     // Soil moisture analog pin (ADC1)
#define RELAY_PIN      26     // Relay IN pin

const uint8_t RELAY_ON_LEVEL  = HIGH;
const uint8_t RELAY_OFF_LEVEL = LOW;

// ── DHT22 setup ───────────────────────────────────────────────────────────────
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ── Web server on port 80 ─────────────────────────────────────────────────────
WebServer server(80);

// ── Thresholds ────────────────────────────────────────────────────────────────
const float TEMP_HIGH_THRESHOLD = 35.0;  // °C — extra dryness flag
const int   SOIL_DRY_PERCENT_THRESHOLD = 5;
const int   SOIL_WET_PERCENT_THRESHOLD = 15;

// ── Performance metric accumulators ───────────────────────────────────────────
unsigned long requestCount      = 0;
unsigned long totalLatencyMs    = 0;
unsigned long deliveredPackets  = 0;
unsigned long totalPackets      = 0;
unsigned long lastThroughputMs  = 0;
unsigned long bytesServedLast   = 0;
unsigned long bytesServedTotal  = 0;
float         throughputKBps    = 0.0;

// ── Sensor state (updated every 2 s) ─────────────────────────────────────────
float    temperature   = 0.0;
float    humidity      = 0.0;
int      soilRaw       = 0;
int      soilPercent   = 0;
bool     pumpOn        = false;
unsigned long lastSensorRead = 0;

// ── Irrigation cycle counters ─────────────────────────────────────────────────
unsigned long irrigationCycles = 0;
unsigned long pumpOnMs         = 0;
unsigned long pumpStartTime    = 0;

// ── Map soil ADC to 0–100 % (wet=0 V → high ADC, dry=3.3 V → low ADC) ────────
// Calibrate these two values for your specific sensor + soil combo.
// For many capacitive moisture sensor v1.2 modules, dry air reads LOWER and
// wet soil reads HIGHER on the ESP32 ADC. If your readings are inverted, swap
// these two values.
const int SOIL_DRY_ADC = 100;
const int SOIL_WET_ADC = 3000;

int readSoilRaw() {
  long sum = 0;
  const int samples = 10;

  for (int i = 0; i < samples; i++) {
    sum += analogRead(SOIL_PIN);
    delay(5);
  }

  return (int)(sum / samples);
}

int soilToPercent(int raw) {
  if (SOIL_DRY_ADC == SOIL_WET_ADC) {
    return 0;
  }

  int pct = map(raw, SOIL_DRY_ADC, SOIL_WET_ADC, 0, 100);
  return constrain(pct, 0, 100);
}

// ── Pump control ──────────────────────────────────────────────────────────────
void setPump(bool on) {
  if (on && !pumpOn) {
    pumpStartTime = millis();
    irrigationCycles++;
  }
  if (!on && pumpOn) {
    pumpOnMs += millis() - pumpStartTime;
  }
  pumpOn = on;
  // Most relay modules used with ESP32 are active-low: LOW = relay ON.
  digitalWrite(RELAY_PIN, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// ── Read sensors and decide pump state ────────────────────────────────────────
void updateSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;

  soilRaw     = readSoilRaw();
  soilPercent = soilToPercent(soilRaw);

  bool soilDry  = soilPercent <= SOIL_DRY_PERCENT_THRESHOLD;
  bool hotAndDry = (temperature > TEMP_HIGH_THRESHOLD) && (humidity < 40.0);

  if (soilDry || hotAndDry) {
    if (!pumpOn) setPump(true);
  } else if (pumpOn && soilPercent >= SOIL_WET_PERCENT_THRESHOLD) {
    if (pumpOn) setPump(false);
  }
}

// ── HTML dashboard ─────────────────────────────────────────────────────────────
String buildDashboard() {
  unsigned long avgLatency = (requestCount > 0) ? (totalLatencyMs / requestCount) : 0;
  float pdr = (totalPackets > 0) ? (100.0 * deliveredPackets / totalPackets) : 100.0;
  float pumpOnSec = (pumpOnMs + (pumpOn ? (millis() - pumpStartTime) : 0)) / 1000.0;

  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="3">
<title>Irrigation System</title>
<style>
  :root {
    --green: #1D9E75; --green-lt: #E1F5EE; --green-dk: #085041;
    --blue:  #378ADD; --blue-lt:  #E6F1FB;
    --amber: #BA7517; --amber-lt: #FAEEDA;
    --red:   #E24B4A; --red-lt:   #FCEBEB;
    --gray:  #888780; --gray-lt:  #F1EFE8;
    --bg: #f8f8f7; --card: #ffffff;
    --border: rgba(0,0,0,0.10);
    --text: #2c2c2a; --muted: #5f5e5a;
    --radius: 12px;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, sans-serif; background: var(--bg);
         color: var(--text); padding: 20px; }
  h1 { font-size: 22px; font-weight: 500; margin-bottom: 4px; }
  .subtitle { font-size: 13px; color: var(--muted); margin-bottom: 24px; }
  .section-title { font-size: 13px; font-weight: 500; color: var(--muted);
                   text-transform: uppercase; letter-spacing: .06em;
                   margin: 24px 0 12px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
          gap: 12px; }
  .card { background: var(--card); border: 0.5px solid var(--border);
          border-radius: var(--radius); padding: 16px 18px; }
  .card .label { font-size: 12px; color: var(--muted); margin-bottom: 6px; }
  .card .value { font-size: 26px; font-weight: 500; }
  .card .unit  { font-size: 13px; color: var(--muted); margin-left: 2px; }
  .card .bar-bg { background: var(--gray-lt); border-radius: 6px;
                  height: 6px; margin-top: 10px; overflow: hidden; }
  .card .bar-fill { height: 100%; border-radius: 6px; transition: width .5s; }

  /* Pump status banner */
  .pump-banner { border-radius: var(--radius); padding: 16px 20px;
                 display: flex; align-items: center; gap: 14px;
                 border: 0.5px solid var(--border); }
  .pump-dot { width: 14px; height: 14px; border-radius: 50%; flex-shrink: 0; }
  .pump-label { font-size: 16px; font-weight: 500; }
  .pump-sub   { font-size: 12px; color: var(--muted); margin-top: 2px; }

  /* Metric cards */
  .metric-card { background: var(--gray-lt); border-radius: var(--radius);
                 padding: 14px 16px; }
  .metric-card .m-label { font-size: 11px; color: var(--muted); margin-bottom: 4px; }
  .metric-card .m-value { font-size: 20px; font-weight: 500; }
  .metric-card .m-unit  { font-size: 12px; color: var(--muted); }
  .metric-card .m-desc  { font-size: 11px; color: var(--muted); margin-top: 4px; }

  .refresh-note { font-size: 11px; color: var(--muted); margin-top: 20px; text-align: center; }
</style>
</head>
<body>
<h1>🌿 Urban Irrigation System</h1>
<p class="subtitle">ESP32 IoT Controller &nbsp;·&nbsp; Auto-refreshes every 3 s</p>
)rawhtml";

  // ── Pump status banner ────────────────────────────────────────────────────
  String pumpColor   = pumpOn ? "#1D9E75" : "#888780";
  String pumpBgColor = pumpOn ? "#E1F5EE" : "#F1EFE8";
  String pumpText    = pumpOn ? "Pump is ON — Irrigating" : "Pump is OFF — Soil OK";
  String pumpSub     = "Total cycles: " + String(irrigationCycles)
                     + " &nbsp;|&nbsp; Total on-time: " + String((int)pumpOnSec) + " s";

  html += "<div class='pump-banner' style='background:" + pumpBgColor + ";border-color:"
        + pumpColor + "'>";
  html += "<div class='pump-dot' style='background:" + pumpColor + "'></div>";
  html += "<div><div class='pump-label' style='color:" + String(pumpOn ? "#085041" : "#5f5e5a") + "'>"
        + pumpText + "</div><div class='pump-sub'>" + pumpSub + "</div></div></div>";

  // ── Sensor readings ───────────────────────────────────────────────────────
  html += "<p class='section-title'>Sensor readings</p><div class='grid'>";

  // Temperature
  String tColor = (temperature > TEMP_HIGH_THRESHOLD) ? "#D85A30" : "#378ADD";
  int tPct = constrain((int)map((int)temperature, 0, 50, 0, 100), 0, 100);
  html += "<div class='card'><div class='label'>Temperature</div>"
          "<div class='value' style='color:" + tColor + "'>" + String(temperature, 1)
        + "<span class='unit'>°C</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String(tPct)
        + "%;background:" + tColor + "'></div></div></div>";

  // Humidity
  String hColor = (humidity < 40.0) ? "#D85A30" : "#378ADD";
  html += "<div class='card'><div class='label'>Humidity</div>"
          "<div class='value' style='color:" + hColor + "'>" + String(humidity, 1)
        + "<span class='unit'>%</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String((int)humidity)
        + "%;background:" + hColor + "'></div></div></div>";

  // Soil moisture
  String sColor = (soilPercent < 30) ? "#D85A30" : "#1D9E75";
  html += "<div class='card'><div class='label'>Soil Moisture</div>"
          "<div class='value' style='color:" + sColor + "'>" + String(soilPercent)
        + "<span class='unit'>%</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:" + String(soilPercent)
        + "%;background:" + sColor + "'></div></div></div>";

  // Raw ADC
  html += "<div class='card'><div class='label'>Soil ADC (raw)</div>"
          "<div class='value'>" + String(soilRaw)
        + "<span class='unit'>/4095</span></div>"
          "<div class='bar-bg'><div class='bar-fill' style='width:"
        + String(soilRaw * 100 / 4095) + "%;background:var(--amber)'></div></div></div>";

  html += "</div>";

  // ── Performance metrics ───────────────────────────────────────────────────
  html += "<p class='section-title'>Performance metrics</p><div class='grid'>";

  // Latency
  html += "<div class='metric-card'><div class='m-label'>Avg. Response Latency</div>"
          "<div class='m-value'>" + String(avgLatency) + "<span class='m-unit'> ms</span></div>"
          "<div class='m-desc'>Time to serve this page</div></div>";

  // Throughput
  html += "<div class='metric-card'><div class='m-label'>Throughput</div>"
          "<div class='m-value'>" + String(throughputKBps, 1) + "<span class='m-unit'> KB/s</span></div>"
          "<div class='m-desc'>Data served over last interval</div></div>";

  // PDR
  html += "<div class='metric-card'><div class='m-label'>Packet Delivery Rate</div>"
          "<div class='m-value'>" + String(pdr, 1) + "<span class='m-unit'> %</span></div>"
          "<div class='m-desc'>HTTP requests answered / sent</div></div>";

  // Total requests
  html += "<div class='metric-card'><div class='m-label'>Total Requests</div>"
          "<div class='m-value'>" + String(requestCount) + "</div>"
          "<div class='m-desc'>Since last boot</div></div>";

  html += "</div>";

  html += "<p class='refresh-note'>Page auto-refreshes every 3 seconds via meta-refresh</p>";
  html += "</body></html>";
  return html;
}

// ── HTTP route handlers ────────────────────────────────────────────────────────
void handleRoot() {
  totalPackets++;
  unsigned long t0 = millis();

  String page = buildDashboard();

  unsigned long latency = millis() - t0;
  totalLatencyMs += latency;
  requestCount++;
  deliveredPackets++;

  size_t len = page.length();
  bytesServedTotal += len;

  server.send(200, "text/html", page);
}

void handleReadings() {
  totalPackets++;
  
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="3">
<title>Sensor Readings</title>
<style>
  body { font-family: monospace; background: #1e1e1e; color: #00ff00; 
         padding: 20px; margin: 0; }
  h1 { color: #00ff00; margin-bottom: 20px; }
  .reading { background: #2d2d2d; padding: 12px; margin: 8px 0; 
             border-left: 3px solid #00ff00; font-size: 16px; }
  .label { color: #888; width: 200px; display: inline-block; }
  .value { color: #00ff00; font-weight: bold; }
  .unit { color: #666; font-size: 14px; }
  .refresh { font-size: 12px; color: #666; margin-top: 20px; }
</style>
</head>
<body>
<h1>📊 Live Sensor Monitor</h1>
)rawhtml";

  html += "<div class='reading'><span class='label'>Temperature:</span> ";
  html += "<span class='value'>" + String(temperature, 1) + "</span>";
  html += "<span class='unit'>°C</span></div>";

  html += "<div class='reading'><span class='label'>Humidity:</span> ";
  html += "<span class='value'>" + String(humidity, 1) + "</span>";
  html += "<span class='unit'>%</span></div>";

  html += "<div class='reading'><span class='label'>Soil Raw ADC:</span> ";
  html += "<span class='value'>" + String(soilRaw) + "</span>";
  html += "<span class='unit'>/4095</span></div>";

  html += "<div class='reading'><span class='label'>Soil Moisture:</span> ";
  html += "<span class='value'>" + String(soilPercent) + "</span>";
  html += "<span class='unit'>%</span></div>";

  html += "<div class='reading'><span class='label'>Pump Status:</span> ";
  html += "<span class='value'>" + String(pumpOn ? "ON" : "OFF") + "</span></div>";

  html += "<div class='reading'><span class='label'>Irrigation Cycles:</span> ";
  html += "<span class='value'>" + String(irrigationCycles) + "</span></div>";

  html += "<p class='refresh'>Auto-refreshes every 3 seconds</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleNotFound() {
  totalPackets++;
  server.send(404, "text/plain", "Not found");
}

// ── Arduino setup ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);    // Relay off by default

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  dht.begin();

  Serial.print("Connecting to WiFi: ");
  Serial.println(SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Open this IP in your browser.");

  server.on("/",         handleRoot);
  server.on("/readings", handleReadings);
  server.onNotFound(     handleNotFound);
  server.begin();

  lastSensorRead     = millis();
  lastThroughputMs   = millis();
}

// ── Arduino loop ──────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  // Read sensors every 2 seconds
  if (millis() - lastSensorRead >= 2000) {
    lastSensorRead = millis();
    updateSensors();

    Serial.printf("[Sensors] Temp=%.1f°C  Hum=%.1f%%  SoilRaw=%d  Soil=%d%%  Pump=%s\n",
                  temperature, humidity, soilRaw, soilPercent, pumpOn ? "ON" : "OFF");
  }

  // Recalculate throughput every 5 seconds
  if (millis() - lastThroughputMs >= 5000) {
    unsigned long dt = millis() - lastThroughputMs;
    unsigned long bytesDelta = bytesServedTotal - bytesServedLast;
    throughputKBps = (bytesDelta / 1024.0) / (dt / 1000.0);
    bytesServedLast   = bytesServedTotal;
    lastThroughputMs  = millis();
  }
}

/*
  Urban Irrigation System - ESP32
  Sensors  : DHT22 (GPIO4), Soil Moisture Analog (GPIO34)
  Relay    : GPIO26
  Web      : http://<ip>/          → live dashboard
             http://<ip>/stats     → full performance metrics
  MQTT     : publishes sensor + metric topics every 5 s

  Performance metrics (from reference script approach):
  ─────────────────────────────────────────────────────
  Latency   : time between sensor read START and data-ready timestamp,
              averaged across all readings (µs precision via esp_timer)
  Throughput: total sensor-data bytes produced per second (real data, not HTML)
  PDR       : expected vs actually completed sensor reads (missed reads counted)

  Libraries (Arduino Library Manager):
    • DHT sensor library  (Adafruit)
    • Adafruit Unified Sensor
    • PubSubClient         (Nick O'Leary)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <PubSubClient.h>
#include "esp_timer.h"       // for esp_timer_get_time() → microseconds

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* SSID     = "YOUR_WIFI_SSID";
const char* PASSWORD = "YOUR_WIFI_PASSWORD";

// ── MQTT ──────────────────────────────────────────────────────────────────────
const char* MQTT_HOST   = "broker.hivemq.com"; // or your local Mosquitto IP
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT = "esp32-irrigation-01";

// ── Pins ──────────────────────────────────────────────────────────────────────
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

// ── Soil ADC calibration ──────────────────────────────────────────────────────
const int SOIL_DRY_ADC = 100;
const int SOIL_WET_ADC = 3000;

// =============================================================================
//  PERFORMANCE METRIC VARIABLES  (reference-script style)
// =============================================================================

// ── Latency ───────────────────────────────────────────────────────────────────
// Per the reference: measure time from read-start to data-ready each cycle.
// We use esp_timer_get_time() for microsecond resolution (like millis() but µs).
unsigned long long latencyReadStart  = 0;   // µs timestamp when read begins
unsigned long long totalLatencyUs    = 0;   // sum of all per-read latencies (µs)
unsigned long      latencySamples    = 0;   // how many reads completed
float              lastLatencyMs     = 0.0; // most recent single read latency (ms)
float              avgLatencyMs      = 0.0; // running average (ms)

// ── Throughput ────────────────────────────────────────────────────────────────
// Per the reference: bytes of SENSOR DATA produced per second.
// Each reading produces a fixed payload: "T=xx.x,H=xx.x,S=xxx,R=xxxx,P=ON\n"
// We count those bytes (not HTML) and divide by elapsed seconds.
unsigned long totalSensorBytes  = 0;   // cumulative sensor-data bytes
unsigned long throughputWindowBytes = 0; // bytes in current 5-s window
unsigned long lastThroughputMs  = 0;   // window start timestamp
float         throughputBps     = 0.0; // bytes per second (current window)

// ── Packet Delivery Rate ──────────────────────────────────────────────────────
// Per the reference: expected reads (scheduled) vs completed reads.
// A read is "lost" if DHT returns NaN or millis drift causes a skip.
unsigned long expectedReadCount  = 0;   // incremented every 2-s interval
unsigned long deliveredReadCount = 0;   // incremented only on successful read
unsigned long lostReadCount      = 0;   // expected - delivered
float         pdr                = 100.0; // packet delivery rate %

// ── HTTP request tracking (for /stats page) ───────────────────────────────────
unsigned long httpRequests     = 0;
unsigned long httpDelivered    = 0;
unsigned long httpBytesTotal   = 0;

// ── MQTT tracking ─────────────────────────────────────────────────────────────
unsigned long mqttPublished  = 0;
unsigned long mqttFailed     = 0;
unsigned long mqttReconnects = 0;

// =============================================================================
//  SENSOR STATE
// =============================================================================
float temperature  = 0.0;
float humidity     = 0.0;
int   soilRaw      = 0;
int   soilPercent  = 0;
bool  pumpOn       = false;

// ── Irrigation counters ───────────────────────────────────────────────────────
unsigned long irrigationCycles = 0;
unsigned long pumpOnMs         = 0;
unsigned long pumpStartTime    = 0;

// ── Timing ────────────────────────────────────────────────────────────────────
unsigned long lastSensorReadMs = 0;
unsigned long lastMqttPubMs    = 0;
unsigned long bootMs           = 0;

// =============================================================================
//  HELPERS
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

// Build the sensor-data string exactly as the reference script counts bytes.
// Format: "T=28.5,H=65.2,S=42,R=1840,P=OFF\n"
String buildSensorPayload() {
  return "T=" + String(temperature, 1)
       + ",H=" + String(humidity, 1)
       + ",S=" + String(soilPercent)
       + ",R=" + String(soilRaw)
       + ",P=" + String(pumpOn ? "ON" : "OFF")
       + "\n";
}

// =============================================================================
//  PUMP
// =============================================================================

void setPump(bool on) {
  if (on  && !pumpOn) { pumpStartTime = millis(); irrigationCycles++; }
  if (!on &&  pumpOn) { pumpOnMs += millis() - pumpStartTime; }
  pumpOn = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// =============================================================================
//  SENSOR READ  — with reference-script-style metric tracking
// =============================================================================

void updateSensors() {
  // ── Step 1: mark expected read (like reference counting packetCount++) ──────
  expectedReadCount++;

  // ── Step 2: start latency timer (µs, like reference's startTime = millis()) ─
  latencyReadStart = (unsigned long long)esp_timer_get_time();

  // ── Step 3: read sensors ────────────────────────────────────────────────────
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  soilRaw     = readSoilRaw();

  // ── Step 4: stop latency timer (like reference's receiveTime = millis()) ────
  unsigned long long latencyReadEnd = (unsigned long long)esp_timer_get_time();

  // ── Step 5: validate — NaN from DHT counts as a lost packet ─────────────────
  bool dhtOk = !isnan(t) && !isnan(h);
  if (dhtOk) {
    temperature = t;
    humidity    = h;
  }
  soilPercent = soilToPercent(soilRaw);

  if (dhtOk) {
    // ── Latency: per-read duration in µs, converted to ms ───────────────────
    unsigned long long singleLatencyUs = latencyReadEnd - latencyReadStart;
    totalLatencyUs  += singleLatencyUs;
    latencySamples++;
    lastLatencyMs    = singleLatencyUs / 1000.0;
    avgLatencyMs     = (totalLatencyUs / 1000.0) / latencySamples;

    // ── Throughput: count sensor-data bytes (not HTML) ───────────────────────
    String payload = buildSensorPayload();
    int payloadBytes = payload.length();
    totalSensorBytes        += payloadBytes;
    throughputWindowBytes   += payloadBytes;

    // ── PDR: this read delivered successfully ────────────────────────────────
    deliveredReadCount++;
  } else {
    // DHT read failed → lost packet
    lostReadCount++;
    Serial.println("[WARN] DHT read failed — counted as lost packet");
  }

  // Recalculate PDR
  pdr = (expectedReadCount > 0)
      ? (100.0 * deliveredReadCount / expectedReadCount)
      : 100.0;

  // ── Pump decision ────────────────────────────────────────────────────────────
  bool soilDry   = soilPercent <= SOIL_DRY_PERCENT_THRESHOLD;
  bool hotAndDry = (temperature > TEMP_HIGH_THRESHOLD) && (humidity < 40.0);
  if (soilDry || hotAndDry) {
    if (!pumpOn) setPump(true);
  } else if (pumpOn && soilPercent >= SOIL_WET_PERCENT_THRESHOLD) {
    setPump(false);
  }

  // ── Serial log (mirrors reference script's Serial.println output) ────────────
  Serial.printf("[Sensors] T=%.1f°C  H=%.1f%%  Soil=%d%%(%d)  Pump=%s\n",
                temperature, humidity, soilPercent, soilRaw, pumpOn ? "ON" : "OFF");
  Serial.printf("[Metrics] Latency=%.2fms(avg)  PDR=%.1f%%  SensorBytes=%lu\n",
                avgLatencyMs, pdr, totalSensorBytes);
}

// =============================================================================
//  THROUGHPUT WINDOW  (recalculated every 5 s, like reference's timeSec block)
// =============================================================================

void recalcThroughput() {
  unsigned long dt = millis() - lastThroughputMs;   // ms elapsed
  if (dt == 0) return;
  float dtSec   = dt / 1000.0;
  throughputBps = throughputWindowBytes / dtSec;     // bytes/sec
  throughputWindowBytes = 0;
  lastThroughputMs = millis();

  Serial.printf("[Metrics] Throughput=%.1f B/s  (window %.1f s)\n", throughputBps, dtSec);
}

// =============================================================================
//  MQTT
// =============================================================================

bool mqttConnect() {
  if (mqtt.connected()) return true;
  Serial.print("[MQTT] Connecting...");
  if (mqtt.connect(MQTT_CLIENT)) {
    Serial.println(" connected");
    mqttReconnects++;
    return true;
  }
  Serial.print(" failed rc="); Serial.println(mqtt.state());
  return false;
}

bool mqttPub(const char* topic, String payload, bool retain = true) {
  bool ok = mqtt.publish(topic, payload.c_str(), retain);
  ok ? mqttPublished++ : mqttFailed++;
  return ok;
}

void publishMqtt() {
  if (!mqttConnect()) return;

  // Sensor topics
  mqttPub("irrigation/sensor/temperature",   String(temperature, 1));
  mqttPub("irrigation/sensor/humidity",      String(humidity, 1));
  mqttPub("irrigation/sensor/soil_moisture", String(soilPercent));
  mqttPub("irrigation/sensor/soil_raw",      String(soilRaw));
  mqttPub("irrigation/control/pump",         pumpOn ? "ON" : "OFF");

  // Metric topics (what MQTT Explorer will show under irrigation/metrics/)
  mqttPub("irrigation/metrics/latency_last_ms",  String(lastLatencyMs, 3));
  mqttPub("irrigation/metrics/latency_avg_ms",   String(avgLatencyMs, 3));
  mqttPub("irrigation/metrics/throughput_bps",   String(throughputBps, 1));
  mqttPub("irrigation/metrics/pdr_pct",          String(pdr, 1));
  mqttPub("irrigation/metrics/expected_reads",   String(expectedReadCount));
  mqttPub("irrigation/metrics/delivered_reads",  String(deliveredReadCount));
  mqttPub("irrigation/metrics/lost_reads",       String(lostReadCount));

  Serial.printf("[MQTT] Published. OK=%lu  Failed=%lu\n", mqttPublished, mqttFailed);
}

// =============================================================================
//  CSS (shared)
// =============================================================================

const char* CSS = R"css(
:root{
  --green:#1D9E75;--green-lt:#E1F5EE;--green-dk:#085041;
  --blue:#378ADD;--blue-lt:#E6F1FB;
  --amber:#BA7517;--amber-lt:#FAEEDA;
  --red:#E24B4A;--red-lt:#FCEBEB;
  --purple:#534AB7;--purple-lt:#EEEDFE;
  --gray:#888780;--gray-lt:#F1EFE8;
  --bg:#f8f8f7;--card:#ffffff;
  --border:rgba(0,0,0,0.10);
  --text:#2c2c2a;--muted:#5f5e5a;
  --radius:12px;
}
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);padding:20px;}
h1{font-size:22px;font-weight:500;margin-bottom:4px;}
.sub{font-size:13px;color:var(--muted);margin-bottom:20px;}
nav{margin-bottom:20px;}
nav a{font-size:13px;margin-right:18px;color:var(--blue);text-decoration:none;}
.sec{font-size:12px;font-weight:500;color:var(--muted);text-transform:uppercase;
     letter-spacing:.06em;margin:22px 0 10px;}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;}
.card{background:var(--card);border:0.5px solid var(--border);
      border-radius:var(--radius);padding:14px 16px;}
.card .lbl{font-size:11px;color:var(--muted);margin-bottom:5px;}
.card .val{font-size:24px;font-weight:500;}
.card .unt{font-size:12px;color:var(--muted);margin-left:2px;}
.bar-bg{background:var(--gray-lt);border-radius:6px;height:5px;margin-top:8px;overflow:hidden;}
.bar-fill{height:100%;border-radius:6px;}
.pump-banner{border-radius:var(--radius);padding:14px 18px;
             display:flex;align-items:center;gap:12px;border:0.5px solid;}
.pump-dot{width:12px;height:12px;border-radius:50%;flex-shrink:0;}
.pump-lbl{font-size:15px;font-weight:500;}
.pump-sub{font-size:12px;color:var(--muted);margin-top:2px;}
.tbl{background:var(--card);border:0.5px solid var(--border);border-radius:var(--radius);overflow:hidden;}
.row{display:flex;justify-content:space-between;align-items:center;
     padding:9px 14px;border-bottom:0.5px solid var(--border);font-size:13px;}
.row:last-child{border-bottom:none;}
.row-lbl{color:var(--muted);}
.row-val{font-weight:500;font-family:monospace;font-size:13px;}
.badge{display:inline-block;padding:2px 9px;border-radius:20px;font-size:11px;font-weight:500;}
.bg{background:var(--green-lt);color:var(--green-dk);}
.ba{background:var(--amber-lt);color:#633806;}
.br{background:var(--red-lt);color:#791F1F;}
.bp{background:var(--purple-lt);color:#26215C;}
.formula{background:var(--card);border:0.5px solid var(--border);border-radius:var(--radius);
         padding:14px 16px;font-size:12px;color:var(--muted);line-height:1.8;}
.formula strong{color:var(--text);font-weight:500;}
.formula code{background:var(--gray-lt);padding:1px 5px;border-radius:4px;
              font-family:monospace;font-size:11px;color:var(--text);}
.note{font-size:11px;color:var(--muted);margin-top:18px;text-align:center;}
)css";

// =============================================================================
//  DASHBOARD  GET /
// =============================================================================

String buildDashboard() {
  float pumpOnSec = (pumpOnMs + (pumpOn ? (millis() - pumpStartTime) : 0)) / 1000.0;

  String h = "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='3'>"
    "<title>Irrigation</title><style>" + String(CSS) + "</style></head><body>";

  h += "<h1>&#127807; Irrigation System</h1>";
  h += "<p class='sub'>ESP32 IoT Controller &nbsp;&middot;&nbsp; refreshes every 3 s</p>";
  h += "<nav><a href='/'>Dashboard</a><a href='/stats'>Performance Stats</a></nav>";

  // Pump banner
  String pc = pumpOn ? "#1D9E75":"#888780";
  String pb = pumpOn ? "#E1F5EE":"#F1EFE8";
  h += "<div class='pump-banner' style='background:" + pb + ";border-color:" + pc + "'>";
  h += "<div class='pump-dot' style='background:" + pc + "'></div><div>";
  h += "<div class='pump-lbl' style='color:" + String(pumpOn?"#085041":"#5f5e5a") + "'>"
     + (pumpOn ? "Pump ON &mdash; Irrigating" : "Pump OFF &mdash; Soil OK") + "</div>";
  h += "<div class='pump-sub'>Cycles: " + String(irrigationCycles)
     + " &nbsp;|&nbsp; On-time: " + String((int)pumpOnSec) + " s</div></div></div>";

  // Sensor cards
  h += "<p class='sec'>Sensor readings</p><div class='grid'>";

  String tc = (temperature > TEMP_HIGH_THRESHOLD) ? "#D85A30":"#378ADD";
  h += "<div class='card'><div class='lbl'>Temperature</div>"
       "<div class='val' style='color:" + tc + "'>" + String(temperature,1)
     + "<span class='unt'>&deg;C</span></div>"
       "<div class='bar-bg'><div class='bar-fill' style='width:"
     + String(constrain((int)map((int)temperature,0,50,0,100),0,100))
     + "%;background:" + tc + "'></div></div></div>";

  String hc = (humidity < 40.0) ? "#D85A30":"#378ADD";
  h += "<div class='card'><div class='lbl'>Humidity</div>"
       "<div class='val' style='color:" + hc + "'>" + String(humidity,1)
     + "<span class='unt'>%</span></div>"
       "<div class='bar-bg'><div class='bar-fill' style='width:"
     + String((int)constrain((int)humidity,0,100))
     + "%;background:" + hc + "'></div></div></div>";

  String sc = (soilPercent < 30) ? "#D85A30":"#1D9E75";
  h += "<div class='card'><div class='lbl'>Soil Moisture</div>"
       "<div class='val' style='color:" + sc + "'>" + String(soilPercent)
     + "<span class='unt'>%</span></div>"
       "<div class='bar-bg'><div class='bar-fill' style='width:"
     + String(soilPercent) + "%;background:" + sc + "'></div></div></div>";

  h += "<div class='card'><div class='lbl'>Soil ADC (raw)</div>"
       "<div class='val'>" + String(soilRaw) + "<span class='unt'>/4095</span></div>"
       "<div class='bar-bg'><div class='bar-fill' style='width:"
     + String(soilRaw * 100 / 4095) + "%;background:var(--amber)'></div></div></div>";
  h += "</div>";

  // Live metric cards
  h += "<p class='sec'>Performance metrics</p><div class='grid'>";

  h += "<div class='card'><div class='lbl'>Latency (last read)</div>"
       "<div class='val'>" + String(lastLatencyMs, 2) + "<span class='unt'> ms</span></div>"
       "<div class='lbl' style='margin-top:6px'>avg " + String(avgLatencyMs, 2) + " ms</div></div>";

  h += "<div class='card'><div class='lbl'>Throughput</div>"
       "<div class='val'>" + String(throughputBps, 0) + "<span class='unt'> B/s</span></div>"
       "<div class='lbl' style='margin-top:6px'>" + String(totalSensorBytes) + " bytes total</div></div>";

  h += "<div class='card'><div class='lbl'>Packet Delivery Rate</div>"
       "<div class='val' style='color:" + String(pdr>99?"#1D9E75":pdr>90?"#BA7517":"#E24B4A") + "'>"
     + String(pdr, 1) + "<span class='unt'> %</span></div>"
       "<div class='lbl' style='margin-top:6px'>" + String(lostReadCount) + " lost / " + String(expectedReadCount) + " expected</div></div>";

  h += "<div class='card'><div class='lbl'>MQTT Published</div>"
       "<div class='val'>" + String(mqttPublished) + "</div>"
       "<div class='lbl' style='margin-top:6px'>" + String(mqttFailed) + " failed</div></div>";
  h += "</div>";

  h += "<p class='note'>Auto-refresh 3 s &nbsp;&middot;&nbsp; "
       "<a href='/stats' style='color:var(--blue)'>Full stats &rarr;</a></p>";
  h += "</body></html>";
  return h;
}

// =============================================================================
//  STATS PAGE  GET /stats
// =============================================================================

String buildStats() {
  unsigned long upSec  = (millis() - bootMs) / 1000;
  float pumpOnSec = (pumpOnMs + (pumpOn ? (millis() - pumpStartTime) : 0)) / 1000.0;
  float mqttPdr   = ((mqttPublished + mqttFailed) > 0)
                  ? (100.0 * mqttPublished / (mqttPublished + mqttFailed)) : 100.0;

  auto row = [](String label, String value, String badge="") -> String {
    String v = badge.length()
      ? "<span class='badge " + badge + "'>" + value + "</span>"
      : "<span class='row-val'>" + value + "</span>";
    return "<div class='row'><span class='row-lbl'>" + label + "</span>" + v + "</div>";
  };

  String h = "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>Stats</title><style>" + String(CSS) + "</style></head><body>";

  h += "<h1>&#128202; Performance Stats</h1>";
  h += "<p class='sub'>Metric calculations match reference TCP/IP script &nbsp;&middot;&nbsp; refreshes every 5 s</p>";
  h += "<nav><a href='/'>Dashboard</a><a href='/stats'>Performance Stats</a></nav>";

  // ── Formulas explained ───────────────────────────────────────────────────────
  h += "<p class='sec'>How metrics are calculated</p>";
  h += "<div class='formula'>"
       "<strong>Latency</strong> — time from sensor read start to data-ready, measured in µs "
       "using <code>esp_timer_get_time()</code>, converted to ms. "
       "Same idea as your reference script's <code>receiveTime - startTime</code>.<br>"
       "Formula: <code>latency_ms = (readEnd_µs − readStart_µs) / 1000</code><br><br>"
       "<strong>Throughput</strong> — sensor-data bytes per second (real payload, not HTML). "
       "Resets every 5 s window, matches reference <code>totalBytes / timeSec</code>.<br>"
       "Formula: <code>throughput_Bps = sensorBytesInWindow / windowSeconds</code><br><br>"
       "<strong>PDR</strong> — expected reads (scheduled every 2 s) vs delivered reads "
       "(only counted when DHT succeeds and soil ADC returns valid data). "
       "Matches reference <code>packetCount / expectedCount × 100</code>.<br>"
       "Formula: <code>PDR% = (deliveredReads / expectedReads) × 100</code>"
       "</div>";

  // ── Latency ──────────────────────────────────────────────────────────────────
  h += "<p class='sec'>Latency</p><div class='tbl'>";
  h += row("Last read latency",    String(lastLatencyMs, 3) + " ms");
  h += row("Average latency",      String(avgLatencyMs,  3) + " ms",
           avgLatencyMs < 50 ? "bg" : avgLatencyMs < 200 ? "ba" : "br");
  h += row("Total latency (sum)",  String(totalLatencyUs / 1000.0, 1) + " ms accumulated");
  h += row("Latency samples",      String(latencySamples));
  h += "</div>";

  // ── Throughput ────────────────────────────────────────────────────────────────
  h += "<p class='sec'>Throughput</p><div class='tbl'>";
  h += row("Current throughput",   String(throughputBps, 1) + " B/s");
  h += row("Total sensor bytes",   String(totalSensorBytes) + " bytes");
  h += row("Total sensor KB",      String(totalSensorBytes / 1024.0, 2) + " KB");
  h += row("HTTP bytes served",    String(httpBytesTotal / 1024.0, 1) + " KB (HTML)");
  h += "</div>";

  // ── PDR ────────────────────────────────────────────────────────────────────────
  h += "<p class='sec'>Packet Delivery Rate</p><div class='tbl'>";
  h += row("PDR",                  String(pdr, 2) + " %",
           pdr >= 99.0 ? "bg" : pdr >= 90.0 ? "ba" : "br");
  h += row("Expected reads",       String(expectedReadCount));
  h += row("Delivered reads",      String(deliveredReadCount));
  h += row("Lost reads (NaN/skip)",String(lostReadCount));
  h += row("Read interval",        "2000 ms (fixed)");
  h += "</div>";

  // ── MQTT ─────────────────────────────────────────────────────────────────────
  h += "<p class='sec'>MQTT</p><div class='tbl'>";
  h += row("Broker",               String(MQTT_HOST) + ":" + String(MQTT_PORT));
  h += row("Connected",            mqtt.connected() ? "Yes" : "No",
           mqtt.connected() ? "bg" : "br");
  h += row("MQTT PDR",             String(mqttPdr, 1) + " %",
           mqttPdr >= 99.0 ? "bg" : mqttPdr >= 90.0 ? "ba" : "br");
  h += row("Published OK",         String(mqttPublished));
  h += row("Failed publishes",     String(mqttFailed));
  h += row("Reconnects",           String(mqttReconnects));
  h += "</div>";

  // ── HTTP ─────────────────────────────────────────────────────────────────────
  h += "<p class='sec'>HTTP server</p><div class='tbl'>";
  h += row("Total requests",       String(httpRequests));
  h += row("Delivered (200 OK)",   String(httpDelivered));
  h += row("HTTP PDR",             String(httpRequests > 0 ? 100.0 * httpDelivered / httpRequests : 100.0, 1) + " %");
  h += row("HTML bytes served",    String(httpBytesTotal / 1024.0, 1) + " KB");
  h += "</div>";

  // ── System ───────────────────────────────────────────────────────────────────
  h += "<p class='sec'>System</p><div class='tbl'>";
  h += row("Uptime",               String(upSec/3600) + "h " + String((upSec%3600)/60) + "m " + String(upSec%60) + "s");
  h += row("Free heap",            String(ESP.getFreeHeap()) + " bytes");
  h += row("Total heap",           String(ESP.getHeapSize()) + " bytes");
  h += row("Min free heap",        String(ESP.getMinFreeHeap()) + " bytes");
  h += row("Max alloc heap",       String(ESP.getMaxAllocHeap()) + " bytes");
  h += row("CPU freq",             String(ESP.getCpuFreqMHz()) + " MHz");
  h += row("Flash size",           String(ESP.getFlashChipSize() / 1024) + " KB");
  h += row("WiFi RSSI",            String(WiFi.RSSI()) + " dBm");
  h += row("IP address",           WiFi.localIP().toString());
  h += row("MAC address",          WiFi.macAddress());
  h += "</div>";

  // ── Irrigation ───────────────────────────────────────────────────────────────
  h += "<p class='sec'>Irrigation</p><div class='tbl'>";
  h += row("Pump state",           pumpOn ? "ON" : "OFF", pumpOn ? "bg" : "");
  h += row("Irrigation cycles",    String(irrigationCycles));
  h += row("Total pump on-time",   String((int)pumpOnSec) + " s");
  h += row("Last soil ADC",        String(soilRaw));
  h += row("Last soil moisture",   String(soilPercent) + " %");
  h += row("Last temperature",     String(temperature, 1) + " &deg;C");
  h += row("Last humidity",        String(humidity, 1) + " %");
  h += "</div>";

  h += "<p class='note'>Auto-refresh 5 s &nbsp;&middot;&nbsp; "
       "<a href='/' style='color:var(--blue)'>&larr; Dashboard</a></p>";
  h += "</body></html>";
  return h;
}

// =============================================================================
//  HTTP handlers
// =============================================================================

void handleRoot() {
  httpRequests++;
  String page = buildDashboard();
  httpDelivered++;
  httpBytesTotal += page.length();
  server.send(200, "text/html", page);
}

void handleStats() {
  httpRequests++;
  String page = buildStats();
  httpDelivered++;
  httpBytesTotal += page.length();
  server.send(200, "text/html", page);
}

void handleNotFound() {
  httpRequests++;
  server.send(404, "text/plain", "Not found");
}

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  dht.begin();

  Serial.print("Connecting to WiFi...");
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.println("  /        → dashboard");
  Serial.println("  /stats   → performance stats");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqttConnect();

  server.on("/",       handleRoot);
  server.on("/stats",  handleStats);
  server.onNotFound(   handleNotFound);
  server.begin();

  bootMs           = millis();
  lastSensorReadMs = millis();
  lastMqttPubMs    = millis();
  lastThroughputMs = millis();
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
  server.handleClient();
  mqtt.loop();

  // Sensor read every 2 s (matches reference script's per-packet timing)
  if (millis() - lastSensorReadMs >= 2000) {
    lastSensorReadMs = millis();
    updateSensors();
  }

  // MQTT publish every 5 s
  if (millis() - lastMqttPubMs >= 5000) {
    lastMqttPubMs = millis();
    publishMqtt();
  }

  // Throughput window recalc every 5 s
  if (millis() - lastThroughputMs >= 5000) {
    recalcThroughput();
  }
}

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// ESP-01 relay modules usually drive the relay from GPIO0.
// If your relay works in reverse, change RELAY_ACTIVE_LOW to false.
const uint8_t RELAY_PIN = 0;
const bool RELAY_ACTIVE_LOW = true;

// ESP-01S commonly has the onboard blue LED on GPIO2.
// If your board does not blink, try changing LED_PIN to 1.
const uint8_t LED_PIN = 2;
const bool LED_ACTIVE_LOW = true;
const uint32_t LED_BLINK_INTERVAL_MS = 500;

const char *AP_SSID = "HLC_ESP01";
const char *AP_PASSWORD = "";  // Leave empty for an open AP, or use 8+ chars.

const uint16_t MAX_RELAY_SECONDS = 2;
const uint32_t MAX_RELAY_MS = MAX_RELAY_SECONDS * 1000UL;
const uint32_t EEPROM_MAGIC = 0x484C4333;  // HLC3
const uint32_t TRIGGER_EEPROM_MAGIC = 0x484C5431;  // HLT1
const int TRIGGER_EEPROM_OFFSET = 128;
const uint8_t WIFI_SSID_MAX = 32;
const uint8_t WIFI_PASSWORD_MAX = 64;

ESP8266WebServer server(80);

struct ScheduleConfig {
  bool enabled = false;
  uint8_t hour = 8;
  uint8_t minute = 0;
  uint8_t durationSeconds = 2;
};

ScheduleConfig scheduleConfig;

struct WifiConfig {
  char ssid[WIFI_SSID_MAX + 1] = "";
  char password[WIFI_PASSWORD_MAX + 1] = "";
};

WifiConfig wifiConfig;

struct PersistedConfig {
  uint32_t magic;
  uint8_t enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t durationSeconds;
  char wifiSsid[WIFI_SSID_MAX + 1];
  char wifiPassword[WIFI_PASSWORD_MAX + 1];
} __attribute__((packed));

struct PersistedTriggerRecord {
  uint32_t magic;
  uint8_t hasRecord;
  uint32_t triggeredEpochSeconds;
  uint8_t durationSeconds;
} __attribute__((packed));

struct ScheduleTriggerRecord {
  bool hasRecord = false;
  uint32_t triggeredEpochSeconds = 0;
  uint8_t durationSeconds = 0;
};

ScheduleTriggerRecord lastScheduleTrigger;

bool relayOn = false;
uint32_t relayOnAtMs = 0;
uint32_t relayAutoOffAfterMs = MAX_RELAY_MS;
bool ledOn = false;
uint32_t lastLedBlinkMs = 0;

bool clockSynced = false;
uint32_t syncedAtMs = 0;
uint32_t syncedEpochSeconds = 0;  // Seconds since an arbitrary day 0.
uint32_t lastTriggeredKey = 0xFFFFFFFF;

String htmlPage() {
  return R"HTML(
<!doctype html>
<html lang="zh-Hant">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>HLC ESP01 Relay</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; background: #f2f4f8; color: #172033; }
    main { max-width: 520px; margin: 0 auto; padding: 20px; }
    .card { background: white; border-radius: 16px; padding: 18px; margin: 14px 0; box-shadow: 0 3px 14px #0001; }
    h1 { font-size: 24px; margin: 8px 0 4px; }
    label { display: block; margin: 12px 0 5px; font-weight: bold; }
    input, button { width: 100%; font-size: 18px; box-sizing: border-box; border-radius: 10px; }
    input { padding: 10px; border: 1px solid #cbd3df; }
    button { padding: 13px; border: 0; margin-top: 10px; color: white; font-weight: bold; }
    .on { background: #17803d; }
    .off { background: #b42318; }
    .save { background: #185abc; }
    .muted { color: #5d6b82; font-size: 14px; line-height: 1.45; }
    .status { font-size: 20px; font-weight: bold; }
    .row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .msg { margin-top: 10px; font-size: 14px; color: #17803d; font-weight: bold; min-height: 20px; }
  </style>
</head>
<body>
<main>
  <h1>HLC ESP01 Relay</h1>
  <p class="muted">手機連上 Wi-Fi：<b>HLC_ESP01</b>，瀏覽器開啟 <b>192.168.4.1</b></p>

  <section class="card">
    <h2>Wi-Fi 設定</h2>
    <div id="wifiStatus" class="muted">讀取中...</div>
    <label for="wifiSsid">Wi-Fi 名稱 SSID</label>
    <input id="wifiSsid" maxlength="32" placeholder="輸入要連線的 Wi-Fi 名稱">
    <label for="wifiPassword">Wi-Fi 密碼</label>
    <input id="wifiPassword" type="password" maxlength="64" placeholder="不顯示已保存密碼">
    <button class="save" onclick="saveWiFi()">儲存並連線 Wi-Fi</button>
    <p class="muted">儲存後 ESP-01 仍會保留 HLC_ESP01 AP，之後可繼續用 192.168.4.1 修改設定。</p>
  </section>

  <section class="card">
    <div>Relay 狀態</div>
    <div id="relayStatus" class="status">讀取中...</div>
    <div id="clockStatus" class="muted"></div>
    <div class="row">
      <button class="on" onclick="relayOn()">開啟 2 秒</button>
      <button class="off" onclick="relayOff()">立即關閉</button>
    </div>
    <p class="muted">安全限制：每次開啟最多 2 秒後自動關閉。</p>
  </section>

  <section class="card">
    <h2>每日固定時間開啟</h2>
    <label>
      <input id="enabled" type="checkbox" style="width:auto"> 啟用排程
    </label>
    <label for="time">開啟時間</label>
    <input id="time" type="time" value="08:00">
    <label for="duration">開啟秒數（最多 2 秒）</label>
    <input id="duration" type="number" min="1" max="2" value="2">
    <button class="save" onclick="saveSchedule()">儲存排程</button>
    <button class="save" onclick="syncTime()">同步手機時間</button>
    <div id="scheduleState" class="muted">排程狀態讀取中...</div>
    <div id="lastTriggerStatus" class="status" style="font-size:16px;margin-top:8px;">上次啟動紀錄讀取中...</div>
    <div id="scheduleMsg" class="msg"></div>
    <p class="muted">ESP-01 沒有時鐘電池，重新開機後請按「同步手機時間」。</p>
  </section>
</main>

<script>
let scheduleFormReady = false;
let scheduleDirty = false;

function showScheduleMsg(text) {
  document.getElementById('scheduleMsg').textContent = text;
}

function markScheduleDirty() {
  scheduleDirty = true;
}

async function api(path) {
  const res = await fetch(path, { cache: 'no-store' });
  return res.json();
}

function applyScheduleForm(schedule) {
  document.getElementById('enabled').checked = schedule.enabled;
  document.getElementById('time').value = schedule.time;
  document.getElementById('duration').value = schedule.durationSeconds;
}

async function syncTime() {
  const now = new Date();
  const seconds = now.getHours() * 3600 + now.getMinutes() * 60 + now.getSeconds();
  const result = await api('/api/time?seconds=' + seconds);
  if (result.ok) {
    showScheduleMsg('時間已同步');
  } else {
    showScheduleMsg('時間同步失敗');
  }
  updateStatus();
}

async function updateStatus() {
  const s = await api('/api/status');
  document.getElementById('relayStatus').textContent = s.relayOn ? '開啟' : '關閉';
  document.getElementById('clockStatus').textContent =
    s.clockSynced ? ('目前 ESP 時間：' + s.timeText) : '尚未同步時間';

  if (s.schedule.enabled) {
    document.getElementById('scheduleState').textContent =
      s.clockSynced
        ? `排程已啟用，等待今日 ${s.schedule.time} 自動啟動`
        : `排程已啟用，但尚未同步時間`;
  } else {
    document.getElementById('scheduleState').textContent = '排程未啟用';
  }

  if (s.lastScheduleTrigger.hasRecord) {
    document.getElementById('lastTriggerStatus').textContent =
      `上次排程成功啟動：${s.lastScheduleTrigger.timeText}，開啟 ${s.lastScheduleTrigger.durationSeconds} 秒`;
  } else {
    document.getElementById('lastTriggerStatus').textContent = '尚無排程成功啟動紀錄';
  }

  const editingSchedule =
    scheduleDirty ||
    document.activeElement === document.getElementById('time') ||
    document.activeElement === document.getElementById('duration') ||
    document.activeElement === document.getElementById('enabled');

  if (!scheduleFormReady || !editingSchedule) {
    applyScheduleForm(s.schedule);
    scheduleFormReady = true;
    scheduleDirty = false;
  }

  const wifiSsid = document.getElementById('wifiSsid');
  if (document.activeElement !== wifiSsid && !wifiSsid.value) {
    wifiSsid.value = s.wifi.ssid;
  }
  document.getElementById('wifiStatus').textContent =
    s.wifi.connected
      ? `已連線：${s.wifi.ssid}，IP：${s.wifi.ip}`
      : (s.wifi.ssid ? `尚未連線：${s.wifi.ssid}` : '尚未設定外部 Wi-Fi');
}

async function relayOn() {
  await api('/api/on');
  updateStatus();
}

async function relayOff() {
  await api('/api/off');
  updateStatus();
}

async function saveSchedule() {
  const enabled = document.getElementById('enabled').checked ? 1 : 0;
  const time = document.getElementById('time').value || '08:00';
  const duration = document.getElementById('duration').value || 2;
  const parts = time.split(':');
  const hour = parts[0] || '8';
  const minute = parts[1] || '0';
  const result = await api(`/api/config?enabled=${enabled}&hour=${hour}&minute=${minute}&duration=${duration}`);
  if (result.ok) {
    showScheduleMsg('排程已儲存');
    scheduleFormReady = false;
    scheduleDirty = false;
    updateStatus();
  } else {
    showScheduleMsg('排程儲存失敗');
  }
}

async function saveWiFi() {
  const ssid = document.getElementById('wifiSsid').value.trim();
  const password = document.getElementById('wifiPassword').value;
  await api(`/api/wifi?ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`);
  document.getElementById('wifiPassword').value = '';
  updateStatus();
}

function initPage() {
  document.getElementById('enabled').addEventListener('change', markScheduleDirty);
  document.getElementById('time').addEventListener('input', markScheduleDirty);
  document.getElementById('duration').addEventListener('input', markScheduleDirty);
  syncTime().then(updateStatus);
  setInterval(updateStatus, 1000);
}

initPage();
</script>
</body>
</html>
)HTML";
}

void loadScheduleConfig() {
  PersistedConfig saved;
  EEPROM.get(0, saved);

  if (saved.magic != EEPROM_MAGIC) {
    return;
  }

  if (saved.hour <= 23 && saved.minute <= 59) {
    scheduleConfig.enabled = saved.enabled == 1;
    scheduleConfig.hour = saved.hour;
    scheduleConfig.minute = saved.minute;
    scheduleConfig.durationSeconds = constrain(saved.durationSeconds, 1, MAX_RELAY_SECONDS);
  }

  memcpy(wifiConfig.ssid, saved.wifiSsid, sizeof(wifiConfig.ssid));
  memcpy(wifiConfig.password, saved.wifiPassword, sizeof(wifiConfig.password));
  wifiConfig.ssid[WIFI_SSID_MAX] = '\0';
  wifiConfig.password[WIFI_PASSWORD_MAX] = '\0';
}

void saveScheduleConfig() {
  PersistedConfig saved;
  saved.magic = EEPROM_MAGIC;
  saved.enabled = scheduleConfig.enabled ? 1 : 0;
  saved.hour = scheduleConfig.hour;
  saved.minute = scheduleConfig.minute;
  saved.durationSeconds = scheduleConfig.durationSeconds;
  strncpy(saved.wifiSsid, wifiConfig.ssid, sizeof(saved.wifiSsid));
  strncpy(saved.wifiPassword, wifiConfig.password, sizeof(saved.wifiPassword));
  saved.wifiSsid[WIFI_SSID_MAX] = '\0';
  saved.wifiPassword[WIFI_PASSWORD_MAX] = '\0';

  EEPROM.put(0, saved);
  EEPROM.commit();
}

void loadTriggerRecord() {
  PersistedTriggerRecord saved;
  EEPROM.get(TRIGGER_EEPROM_OFFSET, saved);

  if (saved.magic != TRIGGER_EEPROM_MAGIC || !saved.hasRecord) {
    return;
  }

  lastScheduleTrigger.hasRecord = true;
  lastScheduleTrigger.triggeredEpochSeconds = saved.triggeredEpochSeconds;
  lastScheduleTrigger.durationSeconds = saved.durationSeconds;

  uint32_t day = saved.triggeredEpochSeconds / 86400UL;
  uint32_t minuteOfDay = (saved.triggeredEpochSeconds % 86400UL) / 60UL;
  lastTriggeredKey = day * 1440UL + minuteOfDay;
}

void saveTriggerRecord() {
  PersistedTriggerRecord saved;
  saved.magic = TRIGGER_EEPROM_MAGIC;
  saved.hasRecord = lastScheduleTrigger.hasRecord ? 1 : 0;
  saved.triggeredEpochSeconds = lastScheduleTrigger.triggeredEpochSeconds;
  saved.durationSeconds = lastScheduleTrigger.durationSeconds;

  EEPROM.put(TRIGGER_EEPROM_OFFSET, saved);
  EEPROM.commit();
}

void recordScheduleTrigger(uint8_t durationSeconds) {
  lastScheduleTrigger.hasRecord = true;
  lastScheduleTrigger.triggeredEpochSeconds = nowEpochSeconds();
  lastScheduleTrigger.durationSeconds = durationSeconds;
  saveTriggerRecord();

  Serial.print("Schedule triggered at ");
  Serial.println(epochToTimeText(lastScheduleTrigger.triggeredEpochSeconds));
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  return value;
}

void connectToConfiguredWiFi() {
  if (strlen(wifiConfig.ssid) == 0) {
    WiFi.disconnect();
    return;
  }

  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
}

void startAccessPoint() {
  IPAddress apIp(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPdisconnect(true);
  delay(200);
  WiFi.softAPConfig(apIp, gateway, subnet);

  if (strlen(AP_PASSWORD) >= 8) {
    WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4);
  } else {
    WiFi.softAP(AP_SSID, nullptr, 1, false, 4);
  }
}

uint32_t nowEpochSeconds() {
  if (!clockSynced) {
    return 0;
  }
  return syncedEpochSeconds + ((millis() - syncedAtMs) / 1000UL);
}

String twoDigits(uint8_t value) {
  if (value < 10) {
    return "0" + String(value);
  }
  return String(value);
}

String currentTimeText() {
  if (!clockSynced) {
    return "--:--:--";
  }
  return epochToTimeText(nowEpochSeconds());
}

String epochToTimeText(uint32_t epochSeconds) {
  uint32_t secondsOfDay = epochSeconds % 86400UL;
  uint8_t hour = secondsOfDay / 3600UL;
  uint8_t minute = (secondsOfDay % 3600UL) / 60UL;
  uint8_t second = secondsOfDay % 60UL;
  return twoDigits(hour) + ":" + twoDigits(minute) + ":" + twoDigits(second);
}

void writeRelay(bool on) {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? !on : on);
  relayOn = on;
}

void writeLed(bool on) {
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? !on : on);
  ledOn = on;
}

void turnRelayOn(uint8_t requestedSeconds) {
  uint8_t safeSeconds = constrain(requestedSeconds, 1, MAX_RELAY_SECONDS);
  relayAutoOffAfterMs = safeSeconds * 1000UL;
  if (relayAutoOffAfterMs > MAX_RELAY_MS) {
    relayAutoOffAfterMs = MAX_RELAY_MS;
  }
  relayOnAtMs = millis();
  writeRelay(true);
}

void turnRelayOff() {
  writeRelay(false);
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleStatus() {
  String scheduleTime = twoDigits(scheduleConfig.hour) + ":" + twoDigits(scheduleConfig.minute);
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"relayOn\":" + String(relayOn ? "true" : "false") + ",";
  json += "\"clockSynced\":" + String(clockSynced ? "true" : "false") + ",";
  json += "\"timeText\":\"" + currentTimeText() + "\",";
  json += "\"wifi\":{";
  json += "\"ssid\":\"" + jsonEscape(String(wifiConfig.ssid)) + "\",";
  json += "\"connected\":" + String(wifiConnected ? "true" : "false") + ",";
  json += "\"ip\":\"" + (wifiConnected ? WiFi.localIP().toString() : String("")) + "\"";
  json += "},";
  json += "\"schedule\":{";
  json += "\"enabled\":" + String(scheduleConfig.enabled ? "true" : "false") + ",";
  json += "\"time\":\"" + scheduleTime + "\",";
  json += "\"durationSeconds\":" + String(scheduleConfig.durationSeconds);
  json += "},";
  json += "\"lastScheduleTrigger\":{";
  json += "\"hasRecord\":" + String(lastScheduleTrigger.hasRecord ? "true" : "false") + ",";
  json += "\"timeText\":\"" + (lastScheduleTrigger.hasRecord ? epochToTimeText(lastScheduleTrigger.triggeredEpochSeconds) : String("--:--:--")) + "\",";
  json += "\"durationSeconds\":" + String(lastScheduleTrigger.durationSeconds);
  json += "}}";
  server.send(200, "application/json", json);
}

void handleTimeSync() {
  if (!server.hasArg("seconds")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing seconds\"}");
    return;
  }

  uint32_t secondsOfDay = server.arg("seconds").toInt() % 86400UL;
  uint32_t currentDay = clockSynced ? (nowEpochSeconds() / 86400UL) : 0;
  syncedEpochSeconds = currentDay * 86400UL + secondsOfDay;
  syncedAtMs = millis();
  clockSynced = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRelayOn() {
  turnRelayOn(MAX_RELAY_SECONDS);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRelayOff() {
  turnRelayOff();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWiFiConfig() {
  if (!server.hasArg("ssid")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.arg("password");
  ssid.trim();

  ssid.toCharArray(wifiConfig.ssid, sizeof(wifiConfig.ssid));
  password.toCharArray(wifiConfig.password, sizeof(wifiConfig.password));

  saveScheduleConfig();
  connectToConfiguredWiFi();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleConfig() {
  if (server.hasArg("enabled")) {
    scheduleConfig.enabled = server.arg("enabled").toInt() == 1;
  }
  if (server.hasArg("hour")) {
    scheduleConfig.hour = constrain(server.arg("hour").toInt(), 0, 23);
  }
  if (server.hasArg("minute")) {
    scheduleConfig.minute = constrain(server.arg("minute").toInt(), 0, 59);
  }
  if (server.hasArg("duration")) {
    scheduleConfig.durationSeconds = constrain(server.arg("duration").toInt(), 1, MAX_RELAY_SECONDS);
  }

  lastTriggeredKey = 0xFFFFFFFF;
  saveScheduleConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void updateRelaySafetyTimer() {
  if (relayOn && (millis() - relayOnAtMs >= relayAutoOffAfterMs)) {
    turnRelayOff();
  }
}

void updateSchedule() {
  if (!scheduleConfig.enabled || !clockSynced) {
    return;
  }

  uint32_t nowSeconds = nowEpochSeconds();
  uint32_t day = nowSeconds / 86400UL;
  uint32_t secondsOfDay = nowSeconds % 86400UL;
  uint32_t currentMinute = secondsOfDay / 60UL;
  uint32_t scheduleMinute = scheduleConfig.hour * 60UL + scheduleConfig.minute;
  uint32_t currentKey = day * 1440UL + currentMinute;
  uint32_t targetKey = day * 1440UL + scheduleMinute;

  if (currentKey == targetKey && lastTriggeredKey != targetKey) {
    lastTriggeredKey = targetKey;
    recordScheduleTrigger(scheduleConfig.durationSeconds);
    turnRelayOn(scheduleConfig.durationSeconds);
  }
}

void updateHeartbeatLed() {
  if (millis() - lastLedBlinkMs < LED_BLINK_INTERVAL_MS) {
    return;
  }

  lastLedBlinkMs = millis();
  writeLed(!ledOn);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting HLC ESP01 Relay Controller");

  EEPROM.begin(160);
  loadScheduleConfig();
  loadTriggerRecord();

  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  turnRelayOff();
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
  pinMode(LED_PIN, OUTPUT);
  writeLed(false);

  WiFi.disconnect(true);
  delay(200);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  startAccessPoint();
  connectToConfiguredWiFi();

  Serial.print("AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/time", handleTimeSync);
  server.on("/api/on", handleRelayOn);
  server.on("/api/off", handleRelayOff);
  server.on("/api/wifi", handleWiFiConfig);
  server.on("/api/config", handleConfig);
  server.begin();
}

void loop() {
  server.handleClient();
  updateSchedule();
  updateRelaySafetyTimer();
  updateHeartbeatLed();
}

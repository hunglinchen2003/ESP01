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

const uint16_t MIN_RELAY_SECONDS = 1;
const uint16_t MAX_RELAY_SECONDS = 5;
const uint16_t DEFAULT_RELAY_SECONDS = 3;
const uint32_t MAX_RELAY_MS = MAX_RELAY_SECONDS * 1000UL;
const uint32_t EEPROM_MAGIC = 0x484C4334;  // HLC4
const uint32_t EEPROM_MAGIC_V3 = 0x484C4333;  // HLC3
const uint32_t TRIGGER_EEPROM_MAGIC = 0x484C5432;  // HLT2
const uint32_t TRIGGER_EEPROM_MAGIC_V1 = 0x484C5431;  // HLT1
const int TRIGGER_EEPROM_OFFSET = 128;
const uint8_t WIFI_SSID_MAX = 32;
const uint8_t WIFI_PASSWORD_MAX = 64;

ESP8266WebServer server(80);

struct ScheduleSlot {
  uint8_t hour = 8;
  uint8_t minute = 0;
  uint8_t durationSeconds = DEFAULT_RELAY_SECONDS;
};

struct ScheduleConfig {
  bool enabled = false;
  ScheduleSlot slot1;
  ScheduleSlot slot2 = {18, 0, DEFAULT_RELAY_SECONDS};
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
  uint8_t hour1;
  uint8_t minute1;
  uint8_t duration1;
  uint8_t hour2;
  uint8_t minute2;
  uint8_t duration2;
  char wifiSsid[WIFI_SSID_MAX + 1];
  char wifiPassword[WIFI_PASSWORD_MAX + 1];
} __attribute__((packed));

struct PersistedConfigV3 {
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
  uint8_t slotIndex;
} __attribute__((packed));

struct PersistedTriggerRecordV1 {
  uint32_t magic;
  uint8_t hasRecord;
  uint32_t triggeredEpochSeconds;
  uint8_t durationSeconds;
} __attribute__((packed));

struct ScheduleTriggerRecord {
  bool hasRecord = false;
  uint32_t triggeredEpochSeconds = 0;
  uint8_t durationSeconds = 0;
  uint8_t slotIndex = 0;
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
uint32_t lastTriggeredKeys[2] = {0xFFFFFFFF, 0xFFFFFFFF};

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
    h3 { font-size: 18px; margin: 16px 0 0; }
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
    <label for="manualDuration">手動開啟秒數（1-5 秒）</label>
    <input id="manualDuration" type="number" min="1" max="5" value="3">
    <div class="row">
      <button class="on" onclick="relayOn()">開啟 Relay</button>
      <button class="off" onclick="relayOff()">立即關閉</button>
    </div>
    <p class="muted">每次開啟會在設定秒數後自動關閉，最多 5 秒。</p>
  </section>

  <section class="card">
    <h2>每日固定時間開啟（兩個時段）</h2>
    <label>
      <input id="enabled" type="checkbox" style="width:auto"> 啟用排程
    </label>

    <h3>時段 1</h3>
    <label for="time1">開啟時間</label>
    <input id="time1" type="time" value="08:00">
    <label for="duration1">開啟秒數（1-5 秒）</label>
    <input id="duration1" type="number" min="1" max="5" value="3">

    <h3>時段 2</h3>
    <label for="time2">開啟時間</label>
    <input id="time2" type="time" value="18:00">
    <label for="duration2">開啟秒數（1-5 秒）</label>
    <input id="duration2" type="number" min="1" max="5" value="3">

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
  document.getElementById('time1').value = schedule.slot1.time;
  document.getElementById('duration1').value = schedule.slot1.durationSeconds;
  document.getElementById('time2').value = schedule.slot2.time;
  document.getElementById('duration2').value = schedule.slot2.durationSeconds;
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
        ? `排程已啟用，等待今日 ${s.schedule.slot1.time}、${s.schedule.slot2.time} 自動啟動`
        : `排程已啟用，但尚未同步時間`;
  } else {
    document.getElementById('scheduleState').textContent = '排程未啟用';
  }

  if (s.lastScheduleTrigger.hasRecord) {
    const slotLabel = s.lastScheduleTrigger.slotIndex ? `時段 ${s.lastScheduleTrigger.slotIndex}` : '排程';
    document.getElementById('lastTriggerStatus').textContent =
      `上次${slotLabel}成功啟動：${s.lastScheduleTrigger.timeText}，開啟 ${s.lastScheduleTrigger.durationSeconds} 秒`;
  } else {
    document.getElementById('lastTriggerStatus').textContent = '尚無排程成功啟動紀錄';
  }

  const editingSchedule =
    scheduleDirty ||
    document.activeElement === document.getElementById('time1') ||
    document.activeElement === document.getElementById('time2') ||
    document.activeElement === document.getElementById('duration1') ||
    document.activeElement === document.getElementById('duration2') ||
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
  const duration = document.getElementById('manualDuration').value || 3;
  await api('/api/on?duration=' + duration);
  updateStatus();
}

async function relayOff() {
  await api('/api/off');
  updateStatus();
}

async function saveSchedule() {
  const enabled = document.getElementById('enabled').checked ? 1 : 0;
  const time1 = document.getElementById('time1').value || '08:00';
  const time2 = document.getElementById('time2').value || '18:00';
  const duration1 = document.getElementById('duration1').value || 3;
  const duration2 = document.getElementById('duration2').value || 3;
  const parts1 = time1.split(':');
  const parts2 = time2.split(':');
  const hour1 = parts1[0] || '8';
  const minute1 = parts1[1] || '0';
  const hour2 = parts2[0] || '18';
  const minute2 = parts2[1] || '0';
  const result = await api(`/api/config?enabled=${enabled}&hour1=${hour1}&minute1=${minute1}&duration1=${duration1}&hour2=${hour2}&minute2=${minute2}&duration2=${duration2}`);
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
  document.getElementById('time1').addEventListener('input', markScheduleDirty);
  document.getElementById('time2').addEventListener('input', markScheduleDirty);
  document.getElementById('duration1').addEventListener('input', markScheduleDirty);
  document.getElementById('duration2').addEventListener('input', markScheduleDirty);
  syncTime().then(updateStatus);
  setInterval(updateStatus, 1000);
}

initPage();
</script>
</body>
</html>
)HTML";
}

void loadWifiFromSaved(const char *ssid, const char *password) {
  memcpy(wifiConfig.ssid, ssid, WIFI_SSID_MAX + 1);
  memcpy(wifiConfig.password, password, WIFI_PASSWORD_MAX + 1);
  wifiConfig.ssid[WIFI_SSID_MAX] = '\0';
  wifiConfig.password[WIFI_PASSWORD_MAX] = '\0';
}

void loadScheduleConfig() {
  PersistedConfig saved;
  EEPROM.get(0, saved);

  if (saved.magic == EEPROM_MAGIC) {
    if (saved.hour1 <= 23 && saved.minute1 <= 59 && saved.hour2 <= 23 && saved.minute2 <= 59) {
      scheduleConfig.enabled = saved.enabled == 1;
      scheduleConfig.slot1.hour = saved.hour1;
      scheduleConfig.slot1.minute = saved.minute1;
      scheduleConfig.slot1.durationSeconds = constrain(saved.duration1, MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
      scheduleConfig.slot2.hour = saved.hour2;
      scheduleConfig.slot2.minute = saved.minute2;
      scheduleConfig.slot2.durationSeconds = constrain(saved.duration2, MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
    }
    loadWifiFromSaved(saved.wifiSsid, saved.wifiPassword);
    return;
  }

  PersistedConfigV3 savedV3;
  EEPROM.get(0, savedV3);
  if (savedV3.magic == EEPROM_MAGIC_V3 && savedV3.hour <= 23 && savedV3.minute <= 59) {
    scheduleConfig.enabled = savedV3.enabled == 1;
    scheduleConfig.slot1.hour = savedV3.hour;
    scheduleConfig.slot1.minute = savedV3.minute;
    scheduleConfig.slot1.durationSeconds = constrain(savedV3.durationSeconds, MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
    loadWifiFromSaved(savedV3.wifiSsid, savedV3.wifiPassword);
  }
}

void saveScheduleConfig() {
  PersistedConfig saved;
  saved.magic = EEPROM_MAGIC;
  saved.enabled = scheduleConfig.enabled ? 1 : 0;
  saved.hour1 = scheduleConfig.slot1.hour;
  saved.minute1 = scheduleConfig.slot1.minute;
  saved.duration1 = scheduleConfig.slot1.durationSeconds;
  saved.hour2 = scheduleConfig.slot2.hour;
  saved.minute2 = scheduleConfig.slot2.minute;
  saved.duration2 = scheduleConfig.slot2.durationSeconds;
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

  if (saved.magic == TRIGGER_EEPROM_MAGIC && saved.hasRecord) {
    lastScheduleTrigger.hasRecord = true;
    lastScheduleTrigger.triggeredEpochSeconds = saved.triggeredEpochSeconds;
    lastScheduleTrigger.durationSeconds = saved.durationSeconds;
    lastScheduleTrigger.slotIndex = saved.slotIndex;

    uint32_t day = saved.triggeredEpochSeconds / 86400UL;
    uint32_t minuteOfDay = (saved.triggeredEpochSeconds % 86400UL) / 60UL;
    uint8_t slotArrayIndex = saved.slotIndex >= 1 && saved.slotIndex <= 2 ? saved.slotIndex - 1 : 0;
    lastTriggeredKeys[slotArrayIndex] = day * 1440UL + minuteOfDay;
    return;
  }

  PersistedTriggerRecordV1 savedV1;
  EEPROM.get(TRIGGER_EEPROM_OFFSET, savedV1);
  if (savedV1.magic == TRIGGER_EEPROM_MAGIC_V1 && savedV1.hasRecord) {
    lastScheduleTrigger.hasRecord = true;
    lastScheduleTrigger.triggeredEpochSeconds = savedV1.triggeredEpochSeconds;
    lastScheduleTrigger.durationSeconds = savedV1.durationSeconds;
    lastScheduleTrigger.slotIndex = 1;

    uint32_t day = savedV1.triggeredEpochSeconds / 86400UL;
    uint32_t minuteOfDay = (savedV1.triggeredEpochSeconds % 86400UL) / 60UL;
    lastTriggeredKeys[0] = day * 1440UL + minuteOfDay;
  }
}

void saveTriggerRecord() {
  PersistedTriggerRecord saved;
  saved.magic = TRIGGER_EEPROM_MAGIC;
  saved.hasRecord = lastScheduleTrigger.hasRecord ? 1 : 0;
  saved.triggeredEpochSeconds = lastScheduleTrigger.triggeredEpochSeconds;
  saved.durationSeconds = lastScheduleTrigger.durationSeconds;
  saved.slotIndex = lastScheduleTrigger.slotIndex;

  EEPROM.put(TRIGGER_EEPROM_OFFSET, saved);
  EEPROM.commit();
}

void recordScheduleTrigger(uint8_t durationSeconds, uint8_t slotIndex) {
  lastScheduleTrigger.hasRecord = true;
  lastScheduleTrigger.triggeredEpochSeconds = nowEpochSeconds();
  lastScheduleTrigger.durationSeconds = durationSeconds;
  lastScheduleTrigger.slotIndex = slotIndex;
  saveTriggerRecord();

  Serial.print("Schedule slot ");
  Serial.print(slotIndex);
  Serial.print(" triggered at ");
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
  uint8_t safeSeconds = constrain(requestedSeconds, MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
  relayAutoOffAfterMs = safeSeconds * 1000UL;
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
  String scheduleTime1 = twoDigits(scheduleConfig.slot1.hour) + ":" + twoDigits(scheduleConfig.slot1.minute);
  String scheduleTime2 = twoDigits(scheduleConfig.slot2.hour) + ":" + twoDigits(scheduleConfig.slot2.minute);
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
  json += "\"slot1\":{";
  json += "\"time\":\"" + scheduleTime1 + "\",";
  json += "\"durationSeconds\":" + String(scheduleConfig.slot1.durationSeconds);
  json += "},";
  json += "\"slot2\":{";
  json += "\"time\":\"" + scheduleTime2 + "\",";
  json += "\"durationSeconds\":" + String(scheduleConfig.slot2.durationSeconds);
  json += "}},";
  json += "\"lastScheduleTrigger\":{";
  json += "\"hasRecord\":" + String(lastScheduleTrigger.hasRecord ? "true" : "false") + ",";
  json += "\"timeText\":\"" + (lastScheduleTrigger.hasRecord ? epochToTimeText(lastScheduleTrigger.triggeredEpochSeconds) : String("--:--:--")) + "\",";
  json += "\"durationSeconds\":" + String(lastScheduleTrigger.durationSeconds) + ",";
  json += "\"slotIndex\":" + String(lastScheduleTrigger.slotIndex);
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
  uint8_t seconds = DEFAULT_RELAY_SECONDS;
  if (server.hasArg("duration")) {
    seconds = constrain(server.arg("duration").toInt(), MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
  }
  turnRelayOn(seconds);
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
  if (server.hasArg("hour1")) {
    scheduleConfig.slot1.hour = constrain(server.arg("hour1").toInt(), 0, 23);
  }
  if (server.hasArg("minute1")) {
    scheduleConfig.slot1.minute = constrain(server.arg("minute1").toInt(), 0, 59);
  }
  if (server.hasArg("duration1")) {
    scheduleConfig.slot1.durationSeconds = constrain(server.arg("duration1").toInt(), MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
  }
  if (server.hasArg("hour2")) {
    scheduleConfig.slot2.hour = constrain(server.arg("hour2").toInt(), 0, 23);
  }
  if (server.hasArg("minute2")) {
    scheduleConfig.slot2.minute = constrain(server.arg("minute2").toInt(), 0, 59);
  }
  if (server.hasArg("duration2")) {
    scheduleConfig.slot2.durationSeconds = constrain(server.arg("duration2").toInt(), MIN_RELAY_SECONDS, MAX_RELAY_SECONDS);
  }

  lastTriggeredKeys[0] = 0xFFFFFFFF;
  lastTriggeredKeys[1] = 0xFFFFFFFF;
  saveScheduleConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void updateRelaySafetyTimer() {
  if (relayOn && (millis() - relayOnAtMs >= relayAutoOffAfterMs)) {
    turnRelayOff();
  }
}

void checkScheduleSlot(uint8_t slotArrayIndex, uint8_t slotNumber, const ScheduleSlot &slot) {
  uint32_t nowSeconds = nowEpochSeconds();
  uint32_t day = nowSeconds / 86400UL;
  uint32_t secondsOfDay = nowSeconds % 86400UL;
  uint32_t currentMinute = secondsOfDay / 60UL;
  uint32_t scheduleMinute = slot.hour * 60UL + slot.minute;
  uint32_t currentKey = day * 1440UL + currentMinute;
  uint32_t targetKey = day * 1440UL + scheduleMinute;

  if (currentKey == targetKey && lastTriggeredKeys[slotArrayIndex] != targetKey) {
    lastTriggeredKeys[slotArrayIndex] = targetKey;
    recordScheduleTrigger(slot.durationSeconds, slotNumber);
    turnRelayOn(slot.durationSeconds);
  }
}

void updateSchedule() {
  if (!scheduleConfig.enabled || !clockSynced) {
    return;
  }

  checkScheduleSlot(0, 1, scheduleConfig.slot1);
  checkScheduleSlot(1, 2, scheduleConfig.slot2);
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

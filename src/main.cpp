// ESP32 4-channel IR hub
//  - 4 independent IR outputs, 1 IR receiver for learning
//  - Web UI, REST (JSON + plain GET), line-based TCP, MQTT
//  - Learned commands persist in LittleFS (/commands.json)

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

#include "config.h"
#include "webui.h"
#include "presets_xfinity.h"

#define CMD_FILE "/commands.json"
#define CFG_FILE "/config.json"

// ---------------------------------------------------------------- state ----
struct Config {
  String ssid, pass;
  bool mqttEnabled = false;
  String mqttHost, mqttUser, mqttPass;
  String mqttPrefix = "irhub";
  uint16_t mqttPort = 1883;
} cfg;

IRsend *tx[NUM_CHANNELS];
IRrecv irrecv(RX_PIN, CAPTURE_BUF, CAPTURE_TIMEOUT_MS, true);
decode_results results;

WebServer http(80);
WiFiServer tcp(TCP_PORT);
WiFiClient tcpClients[MAX_TCP_CLIENTS];
String tcpBuf[MAX_TCP_CLIENTS];

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

JsonDocument cmds;        // name -> {proto,bits,hex,hz} or {raw:[...],hz} or {seq:[...]}
JsonDocument presetCmds;  // same shape, built in from presets_xfinity.h; cmds[name] overrides a preset of the same name

bool apMode = false;
uint32_t rebootAt = 0;
uint32_t lastMqttTry = 0;

bool learnActive = false;
String learnName;
String learnStatus = "idle";
uint32_t learnDeadline = 0;

// -------------------------------------------------------------- helpers ----
bool loadJson(const char *path, JsonDocument &doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  return !e;
}

bool saveJson(const char *path, JsonDocument &doc) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

String cleanName(String s) {
  s.trim();
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') o += c;
    else if (c == ' ') o += '_';
  }
  if (o.length() > 40) o = o.substring(0, 40);
  return o;
}

String errJson(const String &m) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = m;
  String s;
  serializeJson(d, s);
  return s;
}

String summarize(JsonVariantConst c) {
  if (c["raw"].is<JsonArray>()) {
    return "raw (" + String((unsigned)c["raw"].size()) + " pulses, " + String((int)(c["hz"] | 38)) + " kHz)";
  }
  return String(c["proto"] | "?") + " " + String((int)(c["bits"] | 0)) + "b 0x" + String(c["hex"] | "");
}

void loadConfig() {
  JsonDocument d;
  if (!loadJson(CFG_FILE, d)) return;
  cfg.ssid = d["ssid"] | "";
  cfg.pass = d["pass"] | "";
  cfg.mqttEnabled = d["mqttEnabled"] | false;
  cfg.mqttHost = d["mqttHost"] | "";
  cfg.mqttPort = d["mqttPort"] | 1883;
  cfg.mqttUser = d["mqttUser"] | "";
  cfg.mqttPass = d["mqttPass"] | "";
  cfg.mqttPrefix = d["mqttPrefix"] | "irhub";
}

void saveConfig() {
  JsonDocument d;
  d["ssid"] = cfg.ssid;
  d["pass"] = cfg.pass;
  d["mqttEnabled"] = cfg.mqttEnabled;
  d["mqttHost"] = cfg.mqttHost;
  d["mqttPort"] = cfg.mqttPort;
  d["mqttUser"] = cfg.mqttUser;
  d["mqttPass"] = cfg.mqttPass;
  d["mqttPrefix"] = cfg.mqttPrefix;
  saveJson(CFG_FILE, d);
}

void loadPresets() {
  if (deserializeJson(presetCmds, XFINITY_PRESETS_JSON)) {
    Serial.println("built-in preset JSON failed to parse");
    presetCmds.to<JsonObject>();
  }
}

// A learned/stored command wins over a built-in preset of the same name.
JsonVariantConst lookupCmd(const String &name) {
  JsonVariantConst c = cmds[name];
  if (!c.isNull()) return c;
  return presetCmds[name];
}

void mqttPub(const String &sub, const String &payload, bool retain = false) {
  if (!mqtt.connected()) return;
  mqtt.publish((cfg.mqttPrefix + "/" + sub).c_str(), payload.c_str(), retain);
}

void tcpBroadcast(const String &s) {
  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    if (tcpClients[i] && tcpClients[i].connected()) tcpClients[i].print(s + "\r\n");
  }
}

// ----------------------------------------------------------------- send ----
// Returns "" on success, otherwise an error message.
// A single code is either {raw:[...], hz:38} or {proto:"NEC", bits:32, hex:"20DF10EF"}.
String sendOneCode(IRsend *s, JsonVariantConst c, int repeat) {
  uint16_t khz = c["hz"] | 38;
  if (c["raw"].is<JsonArray>()) {
    JsonArrayConst a = c["raw"].as<JsonArrayConst>();
    size_t n = a.size();
    if (n == 0) return "empty raw data";
    uint16_t *buf = new uint16_t[n];
    size_t i = 0;
    for (JsonVariantConst v : a) buf[i++] = v.as<uint16_t>();
    for (int r = 0; r <= repeat; r++) {
      s->sendRaw(buf, n, khz);
      if (r < repeat) delay(RAW_REPEAT_GAP_MS);
    }
    delete[] buf;
    return "";
  }
  decode_type_t type = strToDecodeType(c["proto"] | "");
  if (type == UNKNOWN) return "unknown protocol";
  uint64_t val = strtoull(c["hex"] | "0", nullptr, 16);
  uint16_t bits = c["bits"] | 0;
  if (bits == 0) bits = IRsend::defaultBits(type);
  if (!s->send(type, val, bits, repeat)) return "protocol not supported for sending";
  return "";
}

// A command is a single code (above), or {seq:[...]}: a fixed sequence of codes and
// {delayMs:N} pauses, sent once through in order (repeat is ignored for seq commands --
// some remote buttons, e.g. the Xfinity ALL_POWER/GUIDE/MENU presets, emit more than one
// IR code with a gap in between rather than a single repeatable code).
String doSend(int ch, JsonVariantConst c, int repeat) {
  if (ch < 1 || ch > NUM_CHANNELS) return "bad channel (1-" + String(NUM_CHANNELS) + ")";
  if (learnActive) return "busy: learn mode active";
  if (repeat < 0) repeat = 0;
  if (repeat > 20) repeat = 20;

  IRsend *s = tx[ch - 1];
  String err = "";

  digitalWrite(STATUS_LED, HIGH);
  if (c["seq"].is<JsonArray>()) {
    for (JsonVariantConst step : c["seq"].as<JsonArrayConst>()) {
      if (step["delayMs"].is<int>()) {
        delay(step["delayMs"].as<int>());
      } else {
        err = sendOneCode(s, step, 0);
        if (!err.isEmpty()) break;
      }
    }
  } else {
    err = sendOneCode(s, c, repeat);
  }
  digitalWrite(STATUS_LED, LOW);
  return err;
}

void reportSend(int ch, const String &what, const String &err) {
  JsonDocument d;
  d["event"] = "sent";
  d["ch"] = ch;
  d["cmd"] = what;
  d["ok"] = err.isEmpty();
  if (!err.isEmpty()) d["error"] = err;
  String s;
  serializeJson(d, s);
  mqttPub("event", s);
}

String sendByName(int ch, const String &name, int repeat) {
  JsonVariantConst c = lookupCmd(name);
  if (c.isNull()) return "unknown command: " + name;
  String err = doSend(ch, c, repeat);
  reportSend(ch, name, err);
  return err;
}

// Accepts {"ch":1,"cmd":"NAME","repeat":0} or an ad-hoc {"ch":1,"proto":..,"bits":..,"hex":..} / {"ch":1,"raw":[..],"hz":38}
String handleSendDoc(JsonVariantConst d) {
  int ch = d["ch"] | 0;
  int rep = d["repeat"] | 0;
  if (d["cmd"].is<const char *>()) return sendByName(ch, String(d["cmd"].as<const char *>()), rep);
  String err = doSend(ch, d, rep);
  reportSend(ch, "(adhoc)", err);
  return err;
}

// ---------------------------------------------------------------- learn ----
void startLearn(const String &name) {
  learnName = name;
  learnActive = true;
  learnDeadline = millis() + LEARN_TIMEOUT_MS;
  learnStatus = "waiting";
  irrecv.enableIRIn();
  mqttPub("learn/status", "waiting");
}

void stopLearn(const char *status) {
  learnActive = false;
  irrecv.disableIRIn();
  learnStatus = status;
  digitalWrite(STATUS_LED, LOW);
  mqttPub("learn/status", status);
}

void learnLoop() {
  if (!learnActive) return;
  digitalWrite(STATUS_LED, (millis() / 250) & 1);  // blink while waiting

  if (irrecv.decode(&results)) {
    irrecv.resume();
    if (results.repeat) return;  // ignore NEC-style repeat frames, wait for the real one

    uint16_t rawLen = getCorrectedRawLength(&results);
    if (rawLen < 4) return;  // noise

    JsonDocument e;
    bool usable = results.decode_type != UNKNOWN && !hasACState(results.decode_type) && results.bits <= 64;
    if (usable) {
      e["proto"] = typeToString(results.decode_type);
      e["bits"] = results.bits;
      e["hex"] = uint64ToString(results.value, 16);
      e["hz"] = 38;
    } else {
      uint16_t *raw = resultToRawArray(&results);  // microseconds, caller frees
      JsonArray a = e["raw"].to<JsonArray>();
      for (uint16_t i = 0; i < rawLen; i++) a.add(raw[i]);
      delete[] raw;
      e["hz"] = 38;
    }

    String name = learnName;
    cmds[name].set(e.as<JsonVariantConst>());
    saveJson(CMD_FILE, cmds);

    String sum = summarize(e);
    stopLearn("learned");
    tcpBroadcast("LEARNED " + name + " " + sum);
    JsonDocument ev;
    ev["name"] = name;
    ev["summary"] = sum;
    String s;
    serializeJson(ev, s);
    mqttPub("learned", s);
  } else if ((int32_t)(millis() - learnDeadline) > 0) {
    stopLearn("timeout");
    tcpBroadcast("LEARN_TIMEOUT");
  }
}

// ----------------------------------------------------------------- TCP -----
// Commands (terminate with CR and/or LF):
//   SEND <ch> <name> [repeat]
//   SENDHEX <ch> <proto> <bits> <hex> [repeat]
//   LIST | LEARN <name> | CANCEL | DEL <name> | STATUS
String tcpExec(const String &line) {
  String tok[6];
  int n = 0;
  String l = line;
  l.trim();
  while (l.length() && n < 6) {
    int sp = l.indexOf(' ');
    if (sp < 0) { tok[n++] = l; break; }
    tok[n++] = l.substring(0, sp);
    l = l.substring(sp + 1);
    l.trim();
  }
  if (n == 0) return "";
  tok[0].toUpperCase();

  if (tok[0] == "SEND" && n >= 3) {
    String e = sendByName(tok[1].toInt(), tok[2], n >= 4 ? tok[3].toInt() : 0);
    return e.isEmpty() ? "OK" : "ERR " + e;
  }
  if (tok[0] == "SENDHEX" && n >= 5) {
    JsonDocument d;
    d["proto"] = tok[2];
    d["bits"] = tok[3].toInt();
    d["hex"] = tok[4];
    int ch = tok[1].toInt();
    String e = doSend(ch, d.as<JsonVariantConst>(), n >= 6 ? tok[5].toInt() : 0);
    reportSend(ch, "(adhoc)", e);
    return e.isEmpty() ? "OK" : "ERR " + e;
  }
  if (tok[0] == "LIST") {
    String out;
    for (JsonPairConst kv : presetCmds.as<JsonObjectConst>()) {
      String k = String(kv.key().c_str());
      if (!cmds[k].isNull()) continue;  // overridden below, listed once
      out += "CMD " + k + " (builtin)\r\n";
    }
    for (JsonPairConst kv : cmds.as<JsonObjectConst>()) out += "CMD " + String(kv.key().c_str()) + "\r\n";
    return out + "END";
  }
  if (tok[0] == "LEARN" && n >= 2) {
    String nm = cleanName(tok[1]);
    if (nm.isEmpty()) return "ERR bad name";
    if (learnActive) return "ERR learn already active";
    startLearn(nm);
    return "OK";
  }
  if (tok[0] == "CANCEL") {
    if (learnActive) stopLearn("cancelled");
    return "OK";
  }
  if (tok[0] == "DEL" && n >= 2) {
    cmds.remove(tok[1]);
    saveJson(CMD_FILE, cmds);
    return "OK";
  }
  if (tok[0] == "STATUS") {
    return "STATUS ip=" + WiFi.localIP().toString() + " channels=" + String(NUM_CHANNELS) +
           " learn=" + learnStatus + " commands=" + String((unsigned)cmds.size()) +
           " presets=" + String((unsigned)presetCmds.size());
  }
  return "ERR unknown command";
}

void tcpLoop() {
  WiFiClient nc = tcp.available();
  if (nc) {
    bool placed = false;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
      if (!tcpClients[i] || !tcpClients[i].connected()) {
        tcpClients[i] = nc;
        tcpBuf[i] = "";
        placed = true;
        break;
      }
    }
    if (!placed) nc.stop();
  }
  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    if (!tcpClients[i]) continue;
    if (!tcpClients[i].connected()) { tcpClients[i].stop(); continue; }
    while (tcpClients[i].available()) {
      char c = tcpClients[i].read();
      if (c == '\n' || c == '\r') {
        if (tcpBuf[i].length()) {
          String r = tcpExec(tcpBuf[i]);
          tcpBuf[i] = "";
          if (r.length()) tcpClients[i].print(r + "\r\n");
        }
      } else if (tcpBuf[i].length() < 200) {
        tcpBuf[i] += c;
      }
    }
  }
}

// ---------------------------------------------------------------- MQTT -----
// Subscribes:  <prefix>/ch/<1-4>/send   payload: command name, or JSON {"cmd":"X","repeat":1} / ad-hoc proto or raw
//              <prefix>/learn           payload: name to store the next button press under
//              <prefix>/learn/cancel
// Publishes:   <prefix>/status (retained online/offline), <prefix>/event, <prefix>/learned, <prefix>/learn/status
void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String t(topic);
  String p;
  p.reserve(len);
  for (unsigned i = 0; i < len; i++) p += (char)payload[i];
  p.trim();

  String base = cfg.mqttPrefix + "/";
  if (!t.startsWith(base)) return;
  String sub = t.substring(base.length());

  if (sub.startsWith("ch/") && sub.endsWith("/send")) {
    int ch = sub.substring(3, sub.indexOf('/', 3)).toInt();
    if (p.startsWith("{")) {
      JsonDocument d;
      if (deserializeJson(d, p)) return;
      d["ch"] = ch;
      handleSendDoc(d.as<JsonVariantConst>());
    } else {
      sendByName(ch, p, 0);
    }
  } else if (sub == "learn") {
    String nm = cleanName(p);
    if (nm.isEmpty()) nm = "learned_" + String(millis());
    if (!learnActive) startLearn(nm);
  } else if (sub == "learn/cancel") {
    if (learnActive) stopLearn("cancelled");
  }
}

void mqttLoop() {
  if (!cfg.mqttEnabled || cfg.mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - lastMqttTry < 5000) return;
  lastMqttTry = millis();

  String cid = String(HOSTNAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String will = cfg.mqttPrefix + "/status";
  bool ok = cfg.mqttUser.length()
                ? mqtt.connect(cid.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str(), will.c_str(), 0, true, "offline")
                : mqtt.connect(cid.c_str(), nullptr, nullptr, will.c_str(), 0, true, "offline");
  if (ok) {
    mqtt.publish(will.c_str(), "online", true);
    mqtt.subscribe((cfg.mqttPrefix + "/ch/+/send").c_str());
    mqtt.subscribe((cfg.mqttPrefix + "/learn").c_str());
    mqtt.subscribe((cfg.mqttPrefix + "/learn/cancel").c_str());
  }
}

// ---------------------------------------------------------------- HTTP -----
void jsonOut(int code, const String &body) { http.send(code, "application/json", body); }
void okOut() { jsonOut(200, "{\"ok\":true}"); }

void hRoot() { http.send_P(200, "text/html", INDEX_HTML); }

void hStatus() {
  JsonDocument d;
  d["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["ap"] = apMode;
  d["rssi"] = apMode ? 0 : WiFi.RSSI();
  d["channels"] = NUM_CHANNELS;
  d["mqtt"] = cfg.mqttEnabled ? (mqtt.connected() ? "connected" : "disconnected") : "off";
  d["learn"] = learnStatus;
  d["tcpPort"] = TCP_PORT;
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  String s;
  serializeJson(d, s);
  jsonOut(200, s);
}

// {name: {summary, builtin, overridden}}. builtin=true means it ships in firmware with no
// learned override yet (can't be deleted, only shadowed by learning the same name).
// overridden=true means a learned command is standing in for a built-in preset of the same name.
void hCommands() {
  JsonDocument out;
  JsonObject o = out.to<JsonObject>();
  for (JsonPairConst kv : presetCmds.as<JsonObjectConst>()) {
    JsonObject e = o[String(kv.key().c_str())].to<JsonObject>();
    e["summary"] = summarize(kv.value());
    e["builtin"] = true;
  }
  for (JsonPairConst kv : cmds.as<JsonObjectConst>()) {
    String key = String(kv.key().c_str());
    bool wasBuiltin = o[key]["builtin"] | false;
    JsonObject e = o[key].to<JsonObject>();
    e["summary"] = summarize(kv.value());
    e["builtin"] = false;
    e["overridden"] = wasBuiltin;
  }
  String s;
  serializeJson(out, s);
  jsonOut(200, s);
}

void hSendPost() {
  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { jsonOut(400, errJson("bad json")); return; }
  String e = handleSendDoc(d.as<JsonVariantConst>());
  if (e.isEmpty()) okOut(); else jsonOut(400, errJson(e));
}

// Plain GET for simple control systems: /send?ch=1&cmd=POWER_TV&repeat=0
void hSendGet() {
  String e = sendByName(http.arg("ch").toInt(), http.arg("cmd"), http.arg("repeat").toInt());
  if (e.isEmpty()) okOut(); else jsonOut(400, errJson(e));
}

void hLearnPost() {
  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { jsonOut(400, errJson("bad json")); return; }
  String nm = cleanName(String(d["name"] | ""));
  if (nm.isEmpty()) { jsonOut(400, errJson("name required")); return; }
  if (learnActive) { jsonOut(409, errJson("learn already active")); return; }
  startLearn(nm);
  okOut();
}

void hLearnGet() {
  JsonDocument d;
  d["active"] = learnActive;
  d["state"] = learnStatus;
  d["name"] = learnName;
  String s;
  serializeJson(d, s);
  jsonOut(200, s);
}

void hLearnCancel() {
  if (learnActive) stopLearn("cancelled");
  okOut();
}

void hDelete() {
  String name = http.arg("name");
  if (name.isEmpty()) { jsonOut(400, errJson("name required")); return; }
  cmds.remove(name);
  saveJson(CMD_FILE, cmds);
  okOut();
}

void hExport() {
  String s;
  serializeJson(cmds, s);
  http.sendHeader("Content-Disposition", "attachment; filename=irhub-commands.json");
  jsonOut(200, s);
}

void hImport() {
  JsonDocument d;
  if (deserializeJson(d, http.arg("plain")) || !d.is<JsonObject>()) { jsonOut(400, errJson("expected a JSON object")); return; }
  int n = 0;
  for (JsonPair kv : d.as<JsonObject>()) {
    String nm = cleanName(String(kv.key().c_str()));
    if (nm.isEmpty()) continue;
    cmds[nm].set(kv.value());
    n++;
  }
  saveJson(CMD_FILE, cmds);
  JsonDocument r;
  r["ok"] = true;
  r["imported"] = n;
  String s;
  serializeJson(r, s);
  jsonOut(200, s);
}

void hConfigGet() {
  JsonDocument d;
  d["ssid"] = cfg.ssid;
  d["mqttEnabled"] = cfg.mqttEnabled;
  d["mqttHost"] = cfg.mqttHost;
  d["mqttPort"] = cfg.mqttPort;
  d["mqttUser"] = cfg.mqttUser;
  d["mqttPrefix"] = cfg.mqttPrefix;
  String s;
  serializeJson(d, s);
  jsonOut(200, s);
}

void hConfigPost() {
  JsonDocument d;
  if (deserializeJson(d, http.arg("plain"))) { jsonOut(400, errJson("bad json")); return; }
  if (d["ssid"].is<const char *>()) cfg.ssid = d["ssid"].as<const char *>();
  String pw = d["pass"] | "";
  if (pw.length()) cfg.pass = pw;  // blank = keep existing
  if (d["mqttEnabled"].is<bool>()) cfg.mqttEnabled = d["mqttEnabled"];
  if (d["mqttHost"].is<const char *>()) cfg.mqttHost = d["mqttHost"].as<const char *>();
  if (d["mqttPort"].is<int>()) cfg.mqttPort = d["mqttPort"];
  if (d["mqttUser"].is<const char *>()) cfg.mqttUser = d["mqttUser"].as<const char *>();
  String mpw = d["mqttPass"] | "";
  if (mpw.length()) cfg.mqttPass = mpw;
  String pre = d["mqttPrefix"] | "";
  pre.trim();
  if (pre.length()) cfg.mqttPrefix = pre;
  saveConfig();
  rebootAt = millis() + 1000;  // settings apply after reboot
  okOut();
}

void hReboot() {
  rebootAt = millis() + 500;
  okOut();
}

void setupHttp() {
  http.on("/", HTTP_GET, hRoot);
  http.on("/api/status", HTTP_GET, hStatus);
  http.on("/api/commands", HTTP_GET, hCommands);
  http.on("/api/send", HTTP_POST, hSendPost);
  http.on("/send", HTTP_GET, hSendGet);
  http.on("/api/learn", HTTP_POST, hLearnPost);
  http.on("/api/learn", HTTP_GET, hLearnGet);
  http.on("/api/learn/cancel", HTTP_POST, hLearnCancel);
  http.on("/api/delete", HTTP_POST, hDelete);
  http.on("/api/export", HTTP_GET, hExport);
  http.on("/api/import", HTTP_POST, hImport);
  http.on("/api/config", HTTP_GET, hConfigGet);
  http.on("/api/config", HTTP_POST, hConfigPost);
  http.on("/api/reboot", HTTP_POST, hReboot);
  http.onNotFound([]() { jsonOut(404, errJson("not found")); });
  http.begin();
}

// ---------------------------------------------------------------- setup ----
bool connectWifi() {
  if (cfg.ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_CONNECT_TIMEOUT_MS) {
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
    delay(250);
  }
  digitalWrite(STATUS_LED, LOW);
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadConfig();
  if (!loadJson(CMD_FILE, cmds) || !cmds.is<JsonObject>()) cmds.to<JsonObject>();
  loadPresets();

  for (int i = 0; i < NUM_CHANNELS; i++) {
    tx[i] = new IRsend(TX_PINS[i]);
    tx[i]->begin();
  }
  // The receiver stays off until learn mode starts, so it never hears our own transmissions.

  if (connectWifi()) {
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    Serial.printf("Setup AP '%s' at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("irhub", "tcp", TCP_PORT);
  }

  mqtt.setBufferSize(2048);
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);  // cfg strings live for the whole run
  mqtt.setCallback(mqttCallback);

  setupHttp();
  tcp.begin();
  tcp.setNoDelay(true);
}

void loop() {
  http.handleClient();
  tcpLoop();
  learnLoop();
  mqttLoop();
  if (rebootAt && (int32_t)(millis() - rebootAt) > 0) ESP.restart();
  delay(1);
}

// === Blynk ===
#define BLYNK_TEMPLATE_ID "TMPL6h4imXQEF"
#define BLYNK_TEMPLATE_NAME "IoT Fibbis"
#define BLYNK_AUTH_TOKEN "F9C6di-E2mx2iI1Nm1u4dhLBUURuvOez"

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <LiquidCrystal_I2C.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>

// === WiFi ===
const char* ssid = "HotSpot Pesantren";
const char* password = ""; // open network

// === Telegram ===
#define BOTtoken "8436138118:AAEvw7_OXWhLJGT9Uez6wPiGml8Aj6D6BdM"
#define CHAT_ID  "895163106"

// === Web Server ===
WebServer server(80);

// === LCD I2C ===
LiquidCrystal_I2C lcd(0x27, 16, 2);

// === Relay Pins ===
// lampu1..4 = 4,5,18,19 ; MAIN = 25 ; ALT = 23
const int relayPins[6] = {4, 5, 18, 19, 25, 23};
String relayNames[6] = {"Lampu 1", "Lampu 2", "Lampu 3", "Lampu 4", "Sumber Utama", "Sumber Alternatif"};
bool relayState[6] = {false, false, false, false, false, false};

// === Sensor ACS712 ===
const int currentSensorPin = 34;
float voltage = 220.0;
float sensitivity = 0.066; // 66 mV/A (ACS712 30A)
float offsetVoltage = 1.65;
float currentLimit = 20000.0; // default 20000 mA (20 A)

// === Telegram Client ===
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// === Vars ===
float currentValue = 0.0;     // in mA
float powerValue = 0.0;       // in W
float resistanceValue = 0.0;  // in Ohm
bool manualMode = false;
bool showIPMode = false;
unsigned long showIPStart = 0;

// hysteresis
int overCount = 0;
int normalCount = 0;
const int OVER_THRESHOLD = 3;
const int NORMAL_THRESHOLD = 6;

// forward
void calibrateCurrentSensor();
float readCurrent();
String makePage();
void handleRoot();
void handleToggle();
void handleSetLimit();
void handleMode();
void handleStatus();
void setSourceMain(bool on);   // ensure exclusivity
void setSourceAlt(bool on);

// === Calibration ===
void calibrateCurrentSensor() {
  int samples = 1000;
  float voltageSum = 0.0;
  Serial.println("Kalibrasi ACS712...");
  for (int i = 0; i < samples; i++) {
    int sensorValue = analogRead(currentSensorPin);
    float volt = sensorValue * (3.3 / 4095.0);
    voltageSum += volt;
    delayMicroseconds(300);
  }
  offsetVoltage = voltageSum / samples;
  Serial.print("Offset Voltage = ");
  Serial.println(offsetVoltage, 6);
  bot.sendMessage(CHAT_ID, "🔧 Kalibrasi selesai!\nOffset = " + String(offsetVoltage, 6) + " V", "");
  lcd.clear();
  lcd.print("Kalibrasi OK");
  delay(1000);
  Blynk.virtualWrite(V5, offsetVoltage);
}

// === Read current (returns mA) ===
float readCurrent() {
  const int samples = 200;
  float voltageSum = 0.0;
  for (int i = 0; i < samples; i++) {
    int sensorValue = analogRead(currentSensorPin);
    float volt = sensorValue * (3.3 / 4095.0);
    voltageSum += volt;
    delayMicroseconds(300);
  }
  float avgVoltage = voltageSum / samples;
  float currentA = (avgVoltage - offsetVoltage) / sensitivity; // Ampere
  return abs(currentA * 1000.0); // to mA
}

// === Ensure MAIN/ALT exclusivity helpers ===
void setSourceMain(bool on) {
  if (on) {
    // turn MAIN on, ALT off
    digitalWrite(relayPins[4], HIGH);
    digitalWrite(relayPins[5], LOW);
    relayState[4] = true;
    relayState[5] = false;
    bot.sendMessage(CHAT_ID, "✅ " + relayNames[4] + " ON, " + relayNames[5] + " OFF", "");
  } else {
    // just turn MAIN off (do not force ALT on)
    digitalWrite(relayPins[4], LOW);
    relayState[4] = false;
    bot.sendMessage(CHAT_ID, "❌ " + relayNames[4] + " OFF", "");
  }
}

void setSourceAlt(bool on) {
  if (on) {
    // turn ALT on, MAIN off
    digitalWrite(relayPins[5], HIGH);
    digitalWrite(relayPins[4], LOW);
    relayState[5] = true;
    relayState[4] = false;
    bot.sendMessage(CHAT_ID, "✅ " + relayNames[5] + " ON, " + relayNames[4] + " OFF", "");
  } else {
    digitalWrite(relayPins[5], LOW);
    relayState[5] = false;
    bot.sendMessage(CHAT_ID, "❌ " + relayNames[5] + " OFF", "");
  }
}

// === Web Page ===
String makePage() {
  String page = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>ESP32 Power Monitor</title>";
  page += "<style>body{font-family:Arial;margin:10px} button{padding:8px;margin:4px} .relay{display:inline-block;width:48%;border:1px solid #ccc;padding:6px;border-radius:8px;margin-bottom:6px;}</style>";
  page += "<script>async function api(p){let r=await fetch(p);return r.text();}"
          "async function toggle(i){await fetch('/toggle?relay='+i);await update();}"
          "async function setLimit(){let v=document.getElementById('limit').value;await fetch('/setlimit?amp='+v);await update();}"
          "async function setMode(m){await fetch('/mode?manual='+(m?1:0));await update();}"
          "async function update(){let r=await fetch('/status');let j=await r.json();"
          "document.getElementById('v').innerText=j.v;document.getElementById('i').innerText=j.i;document.getElementById('p').innerText=j.p;"
          "document.getElementById('r').innerText=j.r;document.getElementById('lim').innerText=j.limit;"
          "for(let k=0;k<6;k++){document.getElementById('s'+k).innerText=j.relays[k]?'ON':'OFF';}}"
          "setInterval(update,2000);window.onload=update;</script></head><body>";
  page += "<h2>ESP32 Power Monitor</h2>";
  page += "<div>V: <span id='v'>-</span>V &nbsp; I: <span id='i'>-</span>mA &nbsp; R: <span id='r'>-</span>Ω</div>";
  page += "<div>P: <span id='p'>-</span>W &nbsp; Limit: <span id='lim'>-</span>mA</div><hr>";
  page += "<div style='display:flex;flex-wrap:wrap'>";
  for (int i = 0; i < 6; i++) {
    page += "<div class='relay'><b>" + relayNames[i] + "</b><br>"
            "Status: <span id='s" + String(i) + "'>OFF</span><br>"
            "<button onclick='toggle(" + String(i) + ")'>Toggle</button></div>";
  }
  page += "</div><hr><label>Set Current Limit (mA): </label>"
          "<input id='limit' type='number' step='10' min='0' value='" + String(currentLimit,0) + "'>"
          "<button onclick='setLimit()'>Update</button><br><br>"
          "<button onclick='setMode(false)'>Set AUTO</button> <button onclick='setMode(true)'>Set MANUAL</button>";
  page += "</body></html>";
  return page;
}

// === Web Handlers ===
void handleRoot() { server.send(200, "text/html", makePage()); }

void handleToggle() {
  if (!server.hasArg("relay")) { server.send(400, "text/plain", "missing relay arg"); return; }
  int idx = server.arg("relay").toInt();
  if (idx < 0 || idx >= 6) { server.send(400, "text/plain", "invalid relay"); return; }

  manualMode = true;

  // If toggling MAIN or ALT, enforce exclusivity:
  if (idx == 4) {
    // toggle MAIN
    if (!relayState[4]) {
      setSourceMain(true);
    } else {
      setSourceMain(false);
    }
  } else if (idx == 5) {
    // toggle ALT
    if (!relayState[5]) {
      setSourceAlt(true);
    } else {
      setSourceAlt(false);
    }
  } else {
    // regular lamp relays
    relayState[idx] = !relayState[idx];
    digitalWrite(relayPins[idx], relayState[idx] ? HIGH : LOW);
    String msg = (relayState[idx] ? "✅ ON " : "❌ OFF ") + relayNames[idx];
    bot.sendMessage(CHAT_ID, msg, "");
  }

  server.send(200, "text/plain", "OK");
}

void handleSetLimit() {
  if (server.hasArg("amp")) {
    float v = server.arg("amp").toFloat();
    if (v >= 0.0) {
      currentLimit = v;
      bot.sendMessage(CHAT_ID, "🔧 Batas arus diubah via Web: " + String(currentLimit, 0) + " mA", "");
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleMode() {
  if (server.hasArg("manual")) manualMode = (server.arg("manual").toInt() != 0);
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  String json = "{";
  json += "\"v\":" + String(voltage, 0) + ",";
  json += "\"i\":" + String(currentValue, 0) + ",";
  json += "\"r\":" + String(resistanceValue, 1) + ",";
  json += "\"p\":" + String(powerValue, 1) + ",";
  json += "\"limit\":" + String(currentLimit, 0) + ",";
  json += "\"relays\":[";
  for (int i = 0; i < 6; i++) {
    json += (relayState[i] ? "1" : "0");
    if (i < 5) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// === Blynk Handlers ===
BLYNK_WRITE(V4) { if (param.asInt() == 1) calibrateCurrentSensor(); }

BLYNK_WRITE(V6) {
  if (param.asInt() == 1) {
    lcd.clear();
    lcd.print("IP Address:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    showIPMode = true;
    showIPStart = millis();
  }
}

// Blynk slider for limit (mA)
BLYNK_WRITE(V15) {
  float v = param.asFloat();
  if (v >= 0.0) {
    currentLimit = v;
    bot.sendMessage(CHAT_ID, "🔧 Batas arus diubah via Blynk: " + String(currentLimit, 0) + " mA", "");
  }
}

// Blynk manual controls: lamps V10..V13, MAIN V14, ALT V16
BLYNK_WRITE(V10) { manualMode = true; relayState[0] = param.asInt(); digitalWrite(relayPins[0], relayState[0] ? HIGH : LOW); bot.sendMessage(CHAT_ID, String(relayState[0] ? "✅ ON " : "❌ OFF ") + relayNames[0], ""); }
BLYNK_WRITE(V11) { manualMode = true; relayState[1] = param.asInt(); digitalWrite(relayPins[1], relayState[1] ? HIGH : LOW); bot.sendMessage(CHAT_ID, String(relayState[1] ? "✅ ON " : "❌ OFF ") + relayNames[1], ""); }
BLYNK_WRITE(V12) { manualMode = true; relayState[2] = param.asInt(); digitalWrite(relayPins[2], relayState[2] ? HIGH : LOW); bot.sendMessage(CHAT_ID, String(relayState[2] ? "✅ ON " : "❌ OFF ") + relayNames[2], ""); }
BLYNK_WRITE(V13) { manualMode = true; relayState[3] = param.asInt(); digitalWrite(relayPins[3], relayState[3] ? HIGH : LOW); bot.sendMessage(CHAT_ID, String(relayState[3] ? "✅ ON " : "❌ OFF ") + relayNames[3], ""); }

BLYNK_WRITE(V14) { // MAIN
  manualMode = true;
  if (param.asInt() == 1) setSourceMain(true);
  else setSourceMain(false);
}

BLYNK_WRITE(V16) { // ALT
  manualMode = true;
  if (param.asInt() == 1) setSourceAlt(true);
  else setSourceAlt(false);
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);      // SDA=21, SCL=22
  lcd.init();
  lcd.backlight();
  lcd.print("Init System...");
  delay(1000);
  lcd.clear();

  for (int i = 0; i < 6; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
    relayState[i] = false;
  }

  // Connect WiFi (open)
  WiFi.begin(ssid);
  lcd.print("WiFi Connect...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  lcd.clear();
  lcd.print("WiFi OK");
  Serial.println("\nIP: " + WiFi.localIP().toString());

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, password);

  client.setInsecure();
  bot.sendMessage(CHAT_ID, "✅ ESP32 Online!\nIP: " + WiFi.localIP().toString(), "");

  // Web routes
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/setlimit", handleSetLimit);
  server.on("/mode", handleMode);
  server.on("/status", handleStatus);
  server.begin();

  // initial calibration
  calibrateCurrentSensor();
}

// === Loop ===
void loop() {
  Blynk.run();
  server.handleClient();

  currentValue = readCurrent(); // mA
  powerValue = (voltage * (currentValue / 1000.0)); // W
  resistanceValue = (currentValue > 1.0) ? (voltage / (currentValue / 1000.0)) : 0.0;

  // send to Blynk
  Blynk.virtualWrite(V0, currentValue);
  Blynk.virtualWrite(V1, voltage);
  Blynk.virtualWrite(V2, powerValue);
  Blynk.virtualWrite(V3, resistanceValue);

  // LCD: show IP for 10s if requested, otherwise realtime R,I,P
  if (showIPMode && (millis() - showIPStart < 10000UL)) {
    // keep showing IP
  } else {
    showIPMode = false;
    lcd.setCursor(0,0);
    lcd.print("R:");
    lcd.print(resistanceValue, 1);
    lcd.print("Ω ");
    // clear remaining space if needed
    lcd.print("   ");

    lcd.setCursor(0,1);
    lcd.print("I:");
    lcd.print(currentValue, 0);
    lcd.print("mA ");
    lcd.print("P:");
    lcd.print(powerValue, 1);
    lcd.print("W ");
  }

  // Automatic switching with exclusivity and hysteresis (only when not manualMode)
  if (!manualMode) {
    if (currentValue > currentLimit) {
      overCount++;
      normalCount = 0;
    } else {
      normalCount++;
      overCount = 0;
    }

    if (overCount >= OVER_THRESHOLD) {
      if (!relayState[5]) { // if ALT not already on
        // switch to ALT (ALT on, MAIN off)
        setSourceAlt(true);
      }
      overCount = 0;
    }

    if (normalCount >= NORMAL_THRESHOLD) {
      if (!relayState[4]) { // if MAIN is not on
        setSourceMain(true); // switch back to MAIN
      }
      normalCount = 0;
    }
  }

  delay(800);
}void setup() {
  // put your setup code here, to run once:

}

void loop() {
  // put your main code here, to run repeatedly:

}

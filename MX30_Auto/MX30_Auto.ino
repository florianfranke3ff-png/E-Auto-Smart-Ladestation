/*
 * MX-30 Smart Charging 2.11.2 | Fahrzeug-Node, ESP32-C3 + MMA8452Q
 * SDA=GPIO6, SCL=GPIO7, INT1=GPIO3, Versorgung 3,3 V.
 * WLAN nur zum Melden, dazwischen Deep-Sleep. Keine Internetverbindung notwendig.
 * WLAN-Einrichtung ueber AP 192.168.4.1; IP via DHCP oder optional statisch.
 * Arduino-ESP32 3.x, keine zusaetzlichen Libraries.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <esp_sleep.h>
// Gleicher lokaler UDP-Schluessel in beiden Sketches. Keine Heim-WLAN-Daten im Quelltext.
// ACHTUNG: Das Setup-WLAN ist absichtlich OFFEN; nur in sicherer Umgebung verwenden.
#include "local_secret.h"
constexpr char SETUP_AP_SSID[] = "MX30-Node-Setup";
constexpr gpio_num_t FORCE_SETUP_PIN = GPIO_NUM_10;  // optional: beim Start mit GND verbinden

// Voreinstellung fuer die bisherige Haus-Basis. Per Setup-Webseite aenderbar.
IPAddress baseIp(192, 168, 178, 200);
constexpr int SDA_PIN = 6;
constexpr int SCL_PIN = 7;
constexpr gpio_num_t MOTION_PIN = GPIO_NUM_3;

Preferences wifiPrefs;
WebServer setupWeb(80);
String wlanSsid, wlanPassword;
bool fixedIp = false, portalActive = false, pendingRestart = false, mdnsReady = false;
uint32_t restartAt = 0;
IPAddress nodeIp(192,168,178,201), gatewayIp(192,168,178,1);
IPAddress subnetIp(255,255,255,0), dnsIp(192,168,178,1);
constexpr uint32_t PORTAL_WITH_SAVED_WIFI_MS = 180000; // nach Fehlverbindung: 3 Min. Zugang
RTC_DATA_ATTR bool apAlreadyOffered = false; // Deep-Sleep: nicht bei jedem Wakeup AP starten

String esc(const String &s) {
  String r;
  for (size_t i=0;i<s.length();++i) {
    switch(s[i]) { case '&':r+="&amp;";break;case '<':r+="&lt;";break;
    case '>':r+="&gt;";break;case '\"':r+="&quot;";break;
    case '\'':r+="&#39;";break;default:r+=s[i]; }
  }
  return r;
}
void loadNetwork() {
  wifiPrefs.begin("mx30-wifi", false);
  wlanSsid = wifiPrefs.getString("ssid", "");
  wlanPassword = wifiPrefs.getString("pass", "");
  fixedIp = wifiPrefs.getBool("fixed", false);
  IPAddress tmp;
  if (tmp.fromString(wifiPrefs.getString("nodeip", "192.168.178.201"))) nodeIp=tmp;
  if (tmp.fromString(wifiPrefs.getString("gw", "192.168.178.1"))) gatewayIp=tmp;
  if (tmp.fromString(wifiPrefs.getString("mask", "255.255.255.0"))) subnetIp=tmp;
  if (tmp.fromString(wifiPrefs.getString("dns", "192.168.178.1"))) dnsIp=tmp;
  if (tmp.fromString(wifiPrefs.getString("baseip", "192.168.178.200"))) baseIp=tmp;
}
void initSetupWeb() {
  setupWeb.on("/", HTTP_GET, []() {
    String html = R"HTML(<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>MX-30 Node WLAN</title><style>body{font:16px system-ui;max-width:600px;margin:24px auto;padding:0 18px;background:#111b26;color:#eef5ff}article{padding:20px;background:#243447;border-radius:16px}label{display:block;margin:15px 0 5px}input{box-sizing:border-box;width:100%;padding:12px;font-size:16px;border-radius:8px;border:1px solid #8395a5}input[type=checkbox]{width:auto}button{margin-top:20px;padding:13px 20px;background:#a9e2ff;border:0;border-radius:8px;font-weight:700}small{color:#bcc9d9}</style></head><body><h2>MX-30 Fahrzeug-Node</h2><article><p>WLAN einrichten oder aendern. Die Zugangsdaten bleiben im ESP32 gespeichert.</p><form action="/save" method="POST"><label>WLAN-SSID (2,4 GHz)</label><input name="ssid" maxlength="32" required value=")HTML";
    html += esc(wlanSsid);
    html += R"HTML("><label>WLAN-Passwort</label><input name="pass" type="password" maxlength="63" placeholder="Leer lassen = altes Passwort behalten"><small>Bei neuer SSID ohne Passwort: offenes WLAN.</small><label>IP der Basisstation (Fallback, mDNS wird zuerst versucht)</label><input name="baseip" value=")HTML";
    html += esc(baseIp.toString());
    html += R"HTML("><label><input type="checkbox" name="fixed" value="1" )HTML";
    if(fixedIp) html += "checked";
    html += R"HTML(> Feste IP fuer den Node statt DHCP verwenden</label><label>Node-IP</label><input name="nodeip" value=")HTML";
    html += esc(nodeIp.toString());
    html += R"HTML("><label>Gateway</label><input name="gw" value=")HTML";
    html += esc(gatewayIp.toString());
    html += R"HTML("><label>Subnetzmaske</label><input name="mask" value=")HTML";
    html += esc(subnetIp.toString());
    html += R"HTML("><label>DNS</label><input name="dns" value=")HTML";
    html += esc(dnsIp.toString());
    html += R"HTML("><button type="submit">Speichern und neu starten</button></form></article><p><small>Normalbetrieb: Node schlaeft regelmaessig. Zur erneuten Einrichtung GPIO10 vor einem Neustart mit GND verbinden oder bei nicht erreichbarem WLAN die Einrichtungsphase abwarten.</small></p></body></html>)HTML";
    setupWeb.send(200,"text/html; charset=utf-8",html);
  });
  setupWeb.on("/save", HTTP_POST, []() {
    String newSsid = setupWeb.arg("ssid"); newSsid.trim();
    String newPass = setupWeb.arg("pass");
    if (newSsid.isEmpty() || newSsid.length()>32 || newPass.length()>63) {
      setupWeb.send(400,"text/plain","SSID oder Passwort ungueltig");return;
    }
    IPAddress tmpBase,tmpIp,tmpGw,tmpMask,tmpDns;
    if (!tmpBase.fromString(setupWeb.arg("baseip")) ||
        !tmpIp.fromString(setupWeb.arg("nodeip")) ||
        !tmpGw.fromString(setupWeb.arg("gw")) ||
        !tmpMask.fromString(setupWeb.arg("mask")) ||
        !tmpDns.fromString(setupWeb.arg("dns"))) {
      setupWeb.send(400,"text/plain","IP-Adresse ungueltig");return;
    }
    if (newSsid != wlanSsid || newPass.length()>0) wlanPassword=newPass;
    wlanSsid=newSsid;
    wifiPrefs.putString("ssid",wlanSsid);
    wifiPrefs.putString("pass",wlanPassword);
    wifiPrefs.putBool("fixed",setupWeb.hasArg("fixed"));
    wifiPrefs.putString("baseip",tmpBase.toString());
    wifiPrefs.putString("nodeip",tmpIp.toString());
    wifiPrefs.putString("gw",tmpGw.toString());
    wifiPrefs.putString("mask",tmpMask.toString());
    wifiPrefs.putString("dns",tmpDns.toString());
    setupWeb.send(200,"text/html; charset=utf-8","<h2>Gespeichert.</h2><p>Der Node startet neu und testet die WLAN-Verbindung. Falls diese fehlschlaegt, erscheint der Setup-AP wieder beim Kaltstart.</p>");
    pendingRestart=true;restartAt=millis()+1600;
  });
  setupWeb.onNotFound([](){setupWeb.sendHeader("Location","/");setupWeb.send(302,"text/plain","");});
  setupWeb.begin();
}
void startSetupPortal(uint32_t timeoutMs) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if(!WiFi.softAP(SETUP_AP_SSID)) {
    Serial.println("FEHLER: Setup-AP konnte nicht starten"); return;
  }
  portalActive=true;
  Serial.printf("SETUP-WLAN (offen): %s\n", SETUP_AP_SSID);
  Serial.printf("SETUP-IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("SETUP-Webseite: http://192.168.4.1/");
  initSetupWeb();
  uint32_t began=millis();
  while(!pendingRestart && (timeoutMs==0 || millis()-began<timeoutMs)) {
    setupWeb.handleClient();delay(5);
  }
  if(pendingRestart) {while(millis()<restartAt) delay(10);ESP.restart();}
  setupWeb.stop(); WiFi.softAPdisconnect(true); portalActive=false;
}
constexpr uint16_t BASE_PORT = 42100;
constexpr uint16_t CAR_PORT = 42101;
constexpr uint32_t ACTIVE_INTERVAL_SEC = 60;    // Bewegung: 1 Minute
constexpr uint32_t STILL_INTERVAL_SEC = 600;    // Ruhe: 10 Minuten
constexpr uint8_t ACTIVE_MINUTES_AFTER_MOTION = 5;
constexpr uint32_t WIFI_TIMEOUT_MS = 7500;
constexpr uint32_t ACK_TIMEOUT_MS = 1600;
// Sensorempfindlichkeit 0..100 aus der Basisstation.
// Der MMA8452Q kann nur diskrete Schwellwerte (1 LSB = ca. 0,063 g) nutzen.
// Darum kombinieren wir die Schwelle mit der Mindestdauer. ODR = 50 Hz,
// Low-Power-Modus = 20 ms pro Entprell-Schritt.
// 0:  ~2,0 g / 120 ms, 90: ~0,25 g / 40 ms,
// 95: ~0,13 g / 40 ms, 98: ~0,06 g / 40 ms, 100: ~0,06 g / 20 ms.
constexpr uint8_t DEFAULT_MOTION_SENSITIVITY = 50;
uint8_t motionSensitivity = DEFAULT_MOTION_SENSITIVITY;
uint8_t transientThreshold(uint8_t sensitivity) {
  // 0..100 auf 32..1 Quantisierungsstufen abbilden.
  return (uint8_t)(1U + (uint32_t)(100U - sensitivity) * 31U / 100U);
}
uint8_t transientDebounce(uint8_t sensitivity) {
  if (sensitivity == 100) return 1;  // Schnellste Reaktion
  if (sensitivity >= 85) return 2;
  if (sensitivity >= 65) return 3;
  if (sensitivity >= 40) return 4;
  if (sensitivity >= 20) return 5;
  return 6;
}
void loadMotionSensitivity() {
  Preferences cfg;
  if (cfg.begin("mx30-motion", true)) {
    motionSensitivity = (uint8_t)min((unsigned int)cfg.getUChar("level", DEFAULT_MOTION_SENSITIVITY), 100U);
    cfg.end();
  }
}

RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint32_t rtcCounter = 0;
RTC_DATA_ATTR uint8_t activeMinutesLeft = 0;
constexpr uint32_t RTC_MAGIC = 0x4D583330;
uint8_t mmaAddress = 0;
bool sensorReady = false;

bool sensorWrite(uint8_t reg, uint8_t data) {
  Wire.beginTransmission(mmaAddress);
  Wire.write(reg);
  Wire.write(data);
  return Wire.endTransmission() == 0;
}
bool sensorRead(uint8_t reg, uint8_t &out) {
  Wire.beginTransmission(mmaAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)mmaAddress, 1) != 1) return false;
  out = Wire.read();
  return true;
}

bool initMotionSensor(bool &hadLatchedMotion) {
  hadLatchedMotion = false;
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  // WHO_AM_I = 0x2A (MMA8452Q), SA0 bestimmt 0x1C / 0x1D.
  uint8_t who = 0;
  for (uint8_t candidate : { (uint8_t)0x1C, (uint8_t)0x1D }) {
    mmaAddress = candidate;
    if (sensorRead(0x0D, who) && who == 0x2A) break;
    mmaAddress = 0;
  }
  if (!mmaAddress) {
    Serial.println("MMA8452Q nicht gefunden; nur Timer-Wakeup aktiv.");
    return false;
  }
  // Event des letzten Schlafes VOR dem Rekonfigurieren sichern und quittieren.
  uint8_t transientSrc = 0;
  if (sensorRead(0x1E, transientSrc)) hadLatchedMotion = (transientSrc & 0x40) != 0;
  // Standby, Messbereich +/-2g und HPF-Transient auf allen Achsen.
  bool ok = true;
  ok &= sensorWrite(0x2A, 0x00); // standby
  ok &= sensorWrite(0x0E, 0x00); // +/-2g
  ok &= sensorWrite(0x2B, 0x03); // low-power oversampling
  ok &= sensorWrite(0x1D, 0x1E); // Latch, XYZ aktiv, HPF nicht umgangen
  ok &= sensorWrite(0x1F, transientThreshold(motionSensitivity));
  ok &= sensorWrite(0x20, transientDebounce(motionSensitivity));
  ok &= sensorWrite(0x2C, 0x40); // Transient kann interne sleep mode verlassen, active-low INT
  ok &= sensorWrite(0x2D, 0x20); // Transient-Interrupt aktiv
  ok &= sensorWrite(0x2E, 0x20); // Transient -> INT1
  ok &= sensorWrite(0x2A, 0x21); // 50 Hz, active
  uint8_t discard = 0;
  sensorRead(0x1E, discard); // ausgeloesten Status nach Konfiguration loeschen
  Serial.printf("MMA8452Q Adresse: 0x%02X, Konfiguration: %s | Sensitivitaet: %u/100 | 50 Hz | THS: %u | CNT: %u\n",
      mmaAddress, ok ? "OK" : "Fehler", motionSensitivity,
      transientThreshold(motionSensitivity), transientDebounce(motionSensitivity));
  return ok;
}

// Nach dem Empfang eines neuen Basis-Werts direkt konfigurieren, bevor Deep-Sleep startet.
bool applyMotionSensitivity(uint8_t value) {
  if (value > 100) return false;
  if (value == motionSensitivity) return sensorReady;
  Preferences cfg;
  if (!cfg.begin("mx30-motion", false)) return false;
  const bool written = (cfg.putUChar("level",value) == 1);
  cfg.end();
  if (!written) return false;
  motionSensitivity = value;
  if (!sensorReady) return false;  // Wert gespeichert; Sensor wird beim naechsten Start neu erkannt.
  bool ok = sensorWrite(0x2A,0x00);
  ok &= sensorWrite(0x1F,transientThreshold(motionSensitivity));
  ok &= sensorWrite(0x20,transientDebounce(motionSensitivity));
  ok &= sensorWrite(0x2A,0x21);
  uint8_t cleared=0;
  sensorRead(0x1E,cleared);
  sensorReady = ok;
  Serial.printf("Sensorwert von Basis: %u/100 | THS=%u | CNT=%u | %s\n",
      motionSensitivity, transientThreshold(motionSensitivity),
      transientDebounce(motionSensitivity), ok ? "angewendet":"Fehler");
  return ok;
}

bool wifiConnect() {
  if(wlanSsid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("mx30-auto");
  if(fixedIp) {
    if(!WiFi.config(nodeIp,gatewayIp,subnetIp,dnsIp))
      Serial.println("FEHLER: Statische Node-IP konnte nicht konfiguriert werden");
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
  WiFi.setSleep(false);
  WiFi.begin(wlanSsid.c_str(),wlanPassword.c_str());
  uint32_t started=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-started<WIFI_TIMEOUT_MS) delay(100);
  if(WiFi.status()!=WL_CONNECTED) {
    Serial.printf("WLAN nicht erreichbar. Status: %d\n",(int)WiFi.status());
    return false;
  }
  Serial.printf("NODE-WLAN verbunden | SSID: %s | IP: %s | Gateway: %s | RSSI: %d dBm\n",
      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
      WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
  mdnsReady=MDNS.begin("mx30-auto");
  if(!mdnsReady) Serial.println("mDNS Start fehlgeschlagen; Basis-Fallback-IP wird benutzt");
  return true;
}
IPAddress findBase() {
  // Basisstation per mDNS, egal in welchem Heimnetz; andernfalls gespeicherte IP.
  IPAddress discovered = mdnsReady ? MDNS.queryHost("mx30-basis",1200) : IPAddress(0,0,0,0);
  if(discovered != IPAddress(0,0,0,0)) {
    Serial.printf("Basis per mDNS: %s\n",discovered.toString().c_str());
    return discovered;
  }
  Serial.printf("Basis mDNS nicht erreichbar; verwende Fallback %s\n",baseIp.toString().c_str());
  return baseIp;
}

bool reportToBase(bool moving, uint32_t plannedSleep, esp_sleep_wakeup_cause_t cause) {
  const IPAddress target=findBase();
  WiFiUDP udp;
  if (!udp.begin(CAR_PORT)) return false;
  const String msg = String("MX30|HB|") + MX30_AUTH_TOKEN + "|" +
      String(rtcCounter) + "|" + (moving ? "1" : "0") + "|" +
      String(plannedSleep) + "|" + String(WiFi.RSSI()) + "|" + String((int)cause) +
      "|" + String(motionSensitivity) + "|" + (sensorReady ? "1" : "0");
  const String expected = String("MX30|ACK|") + MX30_AUTH_TOKEN;
  // Bekannte, feste IP der Basis: UDP-Unicast statt Broadcast.

  for (int attempt = 0; attempt < 3; ++attempt) {
    if (udp.beginPacket(target, BASE_PORT)) {
      udp.print(msg);
      udp.endPacket();
    }
    uint32_t started = millis();
    while (millis() - started < ACK_TIMEOUT_MS) {
      int packetSize = udp.parsePacket();
      if (packetSize > 0 && packetSize < 160) {
        char buffer[160];
        int n = udp.read(buffer, sizeof(buffer) - 1);
        if (n > 0) {
          buffer[n] = '\0';
          if (udp.remoteIP() == target) {
            const String ack(buffer);
            if (ack == expected) {udp.stop();return true;} // Aeltere Basis ohne Konfigurationswert
            const String prefix = expected + "|S:";
            if (ack.startsWith(prefix)) {
              const String valText=ack.substring(prefix.length());
              bool numeric=!valText.isEmpty() && valText.length()<=3;
              for(size_t k=0;k<valText.length();++k)
                if(valText[k]<'0'||valText[k]>'9') numeric=false;
              if(numeric) {
                const int val=valText.toInt();
                if(val>=0 && val<=100) {
                  const bool applied=applyMotionSensitivity((uint8_t)val);
                  // Sofortiger Rueckkanal: Basis zeigt erfolgreiche Uebernahme ohne
                  // Warten auf die naechste 10-Minuten-Meldung.
                  const String confirm=String("MX30|CFG|")+MX30_AUTH_TOKEN+"|"+
                      String(motionSensitivity)+"|"+(applied?"1":"0");
                  if(udp.beginPacket(target,BASE_PORT)) {udp.print(confirm);udp.endPacket();}
                }
              }
              udp.stop();
              return true;
            }
          }
        }
      } else if (packetSize >= 160) {
        while (udp.available()) udp.read();
      }
      delay(25);
    }
  }
  udp.stop();
  return false;
}

void sleepNow(uint32_t seconds, bool armMotion) {
  Serial.printf("Schlafe %lu s; Bewegungs-Wakeup: %s\n",
      (unsigned long)seconds, armMotion ? "JA" : "NEIN");
  Serial.flush();
  if(mdnsReady) {MDNS.end(); mdnsReady=false;}
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  // Bei aktivem 1-min-Modus kein GPIO-Wakeup, um Wakeup-Stuerme
  // durch jede Bodenwelle zu vermeiden. Sensor zeichnet Events weiter auf.
  if (sensorReady && armMotion) {
    uint8_t discard = 0;
    sensorRead(0x1E, discard); // gelatchte Events vor Deep-Sleep loeschen
    delay(30);
    if (digitalRead((int)MOTION_PIN) == HIGH) {
      esp_err_t result = esp_deep_sleep_enable_gpio_wakeup(
          1ULL << (int)MOTION_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
      if (result != ESP_OK) Serial.printf("GPIO Wakeup Fehler: %d\n", (int)result);
    } else {
      Serial.println("INT1 bleibt LOW; dieses Mal nur Timer-Wakeup.");
    }
  }
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  if (rtcMagic != RTC_MAGIC) {
    rtcMagic = RTC_MAGIC;
    rtcCounter = 0;
    activeMinutesLeft = 0;
  }
  ++rtcCounter;
  loadNetwork();
  loadMotionSensitivity();
  pinMode((int)FORCE_SETUP_PIN, INPUT_PULLUP);
  delay(20);
  if(digitalRead((int)FORCE_SETUP_PIN)==LOW) {
    Serial.println("GPIO10 LOW: Setup-Modus erzwungen");
    startSetupPortal(0); // unbegrenzt bis WLAN gespeichert ist
  }
  if(wlanSsid.isEmpty()) {
    Serial.println("Noch kein Heim-WLAN gespeichert: Einrichtungsmodus");
    startSetupPortal(0); // Erstinbetriebnahme, bleibt bis zur Einrichtung erreichbar
  }
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  pinMode((int)MOTION_PIN, INPUT_PULLUP);
  bool latchedMotion = false;
  sensorReady = initMotionSensor(latchedMotion);
  const bool motion = sensorReady &&
      (wakeReason == ESP_SLEEP_WAKEUP_GPIO || latchedMotion);
  if (motion) activeMinutesLeft = ACTIVE_MINUTES_AFTER_MOTION;
  bool moving = activeMinutesLeft > 0;
  uint32_t nextSeconds = moving ? ACTIVE_INTERVAL_SEC : STILL_INTERVAL_SEC;
  Serial.printf("MX-30 NODE v2.11.2 | Wake: %d | Event: %s | Nr.: %lu\n",
      (int)wakeReason, motion ? "BEWEGUNG" : "Timer/Start", (unsigned long)rtcCounter);

  bool connected = wifiConnect();
  bool reported = false;
  if (connected) {
    apAlreadyOffered = true; // spaeteres Wegfahren darf keinen Setup-AP ausloesen
    reported = reportToBase(moving, nextSeconds, wakeReason);
    Serial.printf("Basis-Bestaetigung: %s\n", reported ? "JA (zu Hause)" : "NEIN");
  } else {
    Serial.println("Heim-WLAN nicht erreichbar (unterwegs / Signalstoerung).");
    // Einmal pro Einschalten/Reset Setup anbieten, nicht nach jedem Deep-Sleep.
    if(!apAlreadyOffered) {
      apAlreadyOffered=true;
      startSetupPortal(PORTAL_WITH_SAVED_WIFI_MS);
    }
  }
  if (activeMinutesLeft) --activeMinutesLeft;
  sleepNow(nextSeconds, activeMinutesLeft == 0);
}

void loop() {}

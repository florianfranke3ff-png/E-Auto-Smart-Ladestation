/*
 * MX-30 Smart Charging 2.11.1 | Basisstation, ESP32-WROOM-32D
 * WLAN + UDP-Empfang + lokales Webinterface + Shelly Pro 1PM (Gen2-RPC).
 * WICHTIG: Shelly Control ist standardmaessig AUS und wird NUR im Webinterface aktiviert.
 * Keine automatische Leerlaufabschaltung, solange das Auto zu Hause ist.
 * Nach erkannter Abwesenheit 10 Min. warten; danach 6x im Abstand von
 * 5 Sekunden aktuell messen; nur wenn alle Werte unter 20 W sind ausschalten.
 * Eine Ladepause kann damit NICHT sicher von einem Ladeende unterschieden werden.
 * Ohne neue Aktion wird ein spaeteres Ladeende nicht automatisch erkannt.
 * WLAN-Zugang per Einrichtung/AP; DHCP- oder statische IP im Webinterface.
 * Persistenter Fahrzeugverlauf, optimierte Webantworten und Shelly-Timeouts.
 * v2.6: Node-Timeout-Diagnose und Wiederherstellung des letzten Status nach Neustart.
 * v2.7: Shelly-IP im Webinterface / NVS.
 * v2.9: Shelly-Kommunikation nur bei Konfiguration, Neustart oder Ereignissen.
 * v2.10: Mehrfachmessung vor OFF, dauerhaft vorgemerkter Sollzustand und
 *        Retry-Warteschlange, separate persistente Neustart-Historie.
 *        KEIN allgemeines Shelly-Polling.
 * v2.11: Sensorempfindlichkeit 0-100 via Webinterface und UDP-ACK an den Node.
 * Ladungsschutz: Bei Shelly ohne Messung KEIN automatisches AUS bei Abwesenheit!
 * Arduino-ESP32 3.x, ArduinoJson 7.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <time.h>
#include <math.h>
// Gleicher lokaler UDP-Schluessel in beiden Sketches. Keine Heim-WLAN-Daten im Quelltext.
// ACHTUNG: Das Setup-WLAN ist absichtlich OFFEN; nur in sicherer Umgebung verwenden.
#include "local_secret.h"
constexpr char SETUP_AP_SSID[] = "MX30-Basis-Setup";


constexpr uint16_t UDP_PORT = 42100;
String shellyIp;  // Leere IP = Shelly deaktiviert / nicht eingerichtet.
IPAddress baseIp(192,168,178,200), gatewayIp(192,168,178,1);
IPAddress subnetIp(255,255,255,0), dnsIp(192,168,178,1);
String wlanSsid, wlanPassword;
bool fixedIp=false, setupApOn=false, restartPending=false;
uint32_t restartAt=0, wifiStartedMs=0, wifiDownSinceMs=0;
constexpr uint32_t SETUP_FALLBACK_MS=16000;

String esc(const String &s) {
  String r;
  for(size_t i=0;i<s.length();++i) {
    switch(s[i]) {case '&':r+="&amp;";break;case '<':r+="&lt;";break;
      case '>':r+="&gt;";break;case '\"':r+="&quot;";break;case '\'':r+="&#39;";break;
      default:r+=s[i];}
  }
  return r;
}
void loadNetwork() {
  Preferences p; p.begin("mx30-wifi",true);
  wlanSsid=p.getString("ssid","");wlanPassword=p.getString("pass","");
  fixedIp=p.getBool("fixed",false);
  IPAddress t;
  if(t.fromString(p.getString("ip","192.168.178.200")))baseIp=t;
  if(t.fromString(p.getString("gw","192.168.178.1")))gatewayIp=t;
  if(t.fromString(p.getString("mask","255.255.255.0")))subnetIp=t;
  if(t.fromString(p.getString("dns","192.168.178.1")))dnsIp=t;
  p.end();
}
void loadShellyConfig() {
  Preferences sp;
  if (sp.begin("mx30-shelly", true)) {
    String storedIp = sp.getString("ip", "");
    IPAddress checked;
    if (!storedIp.isEmpty() && checked.fromString(storedIp) &&
        checked[0] != 0 && checked[0] != 127 && checked[0] < 224) {
      shellyIp = checked.toString();
    } else shellyIp = "";
    sp.end();
  }
}
void logEvent(const String &msg);

void startAp() {
  if(setupApOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  setupApOn=WiFi.softAP(SETUP_AP_SSID);
  if(setupApOn) {
    logEvent(String("Setup-WLAN (offen): ") + SETUP_AP_SSID);
    Serial.printf("SETUP-IP: %s | Webinterface: http://192.168.4.1/\n",
                  WiFi.softAPIP().toString().c_str());
  }
}
void beginSta() {
  if(wlanSsid.isEmpty())return;
  WiFi.mode(setupApOn?WIFI_AP_STA:WIFI_STA);
  if(fixedIp) {
    if(!WiFi.config(baseIp,gatewayIp,subnetIp,dnsIp)) Serial.println("FEHLER: IP-Konfiguration");
  } else {
    WiFi.config(INADDR_NONE,INADDR_NONE,INADDR_NONE);
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wlanSsid.c_str(),wlanPassword.c_str());
  wifiStartedMs=millis();
  Serial.printf("WLAN-Verbindungsversuch: %s\n",wlanSsid.c_str());
}
constexpr int32_t SHELLY_CONNECT_TIMEOUT_MS = 400; // Verbindung max. 400 ms
constexpr uint16_t SHELLY_READ_TIMEOUT_MS = 850;   // Antwort max. 850 ms
constexpr uint32_t BASE_PRESENCE_CHECK_MS = 10000; // Basis prueft Anwesenheit alle 10 s
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t WIFI_RESTART_AFTER_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t OTA_WINDOW_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t COMMAND_RETRY_MS = 15000;
constexpr uint32_t MAX_COMMAND_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t ABSENT_OFF_GRACE_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t OFF_SAMPLE_INTERVAL_MS = 5000;
constexpr uint8_t OFF_SAMPLES_REQUIRED = 6; // 6 Messwerte ueber mindestens 25 Sekunden
constexpr float IDLE_LIMIT_W = 20.0f;
constexpr int LOG_MAX = 10;
constexpr int TRIP_LOG_MAX = 60;  // Ausschliesslich Ankunft und Abwesenheit
constexpr int BOOT_LOG_MAX = 20;  // Neustarts separat vom Fahrzeugverlauf
constexpr uint8_t DEFAULT_MOTION_SENSITIVITY = 50;  // 0=traege, 100=sehr sensibel
uint8_t motionSensitivity = DEFAULT_MOTION_SENSITIVITY;
bool nodeSensitivityKnown = false, nodeSensorOK = false;
uint8_t nodeAppliedSensitivity = 0;
bool motionSyncPending = true; // Nur bestaetigte Node-Werte im UI als synchron anzeigen.

void loadMotionSensitivity() {
  Preferences cfg;
  if (cfg.begin("mx30-motion", true)) {
    motionSensitivity = min((unsigned int)cfg.getUChar("level", DEFAULT_MOTION_SENSITIVITY), 100U);
    cfg.end();
  }
}


WiFiUDP udp;
WebServer web(80);
Preferences prefs;
Preferences tripPrefs;  // Eigener NVS-Namespace fuer den Fahrzeugverlauf
Preferences bootPrefs;  // Eigener NVS-Namespace fuer System-Neustarts
bool udpStarted = false, lastWifiOnline = false;
bool controlEnabled = false;   // NVS: Werkszustand AUS
bool carEverSeen = false, carHome = false;
// Ein ESP32 im Deep-Sleep ist nicht aktiv per WLAN erreichbar.
// Nach Basis-Neustart stammt ein wiederhergestellter Status nur aus NVS,
// bis entweder ein frischer Heartbeat eingeht oder der Timeout ablaeuft.
bool carStateRestored = false, nodeContactSinceBoot = false;
bool overdueLogged = false;
uint16_t savedCarSleepSeconds = 600;
uint32_t carLastMs = 0, rxCount = 0, lastCarSequence = 0;
uint16_t nextCarSleepSeconds = 600;
bool carMoving = false;
int carRssi = 0, carWakeCause = 0;
uint32_t wifiAttemptMs = 0;
uint32_t presenceCheckMs = 0;
// Shelly wird nur bei Ereignissen kontaktiert. OFF wird nach
// 10-Minuten-Frist mittels kurzer Messreihe geprueft (kein Dauer-Polling).
enum ShellyAction : uint8_t { ACTION_NONE, ACTION_ON, ACTION_OFF };
ShellyAction pendingAction = ACTION_NONE;
uint32_t actionDueMs = 0;
uint32_t offGraceDeadlineMs = 0; // Schonfrist niemals durch WLAN-Reconnect verkuerzen
uint8_t actionAttempts = 0; // Anzahl fehlgeschlagener Abgleichversuche (mit Backoff)
uint8_t offSafeSamples = 0; // sichere Leerlauf-Messwerte dieser OFF-Pruefung
bool bootShellyProbeDone = false;
String shellyDecision = "Noch keine Aktion";
uint32_t shellyRequestCount = 0;
bool shellyOnline = false, shellyOutput = false, shellyStatusSeen = false, shellyHasMeter = false;
uint32_t lastShellyPollDurationMs = 0;  // Dauer der letzten ereignisgesteuerten Abfrage
bool shellyCommandInProgress = false;
float shellyPowerW = 0; // Nur intern: nie einen aktiven Ladezyklus absichtlich unterbrechen.
uint32_t shellyStatusMs = 0;
bool desiredInitialized = false, desiredOutput = false;
String logLines[LOG_MAX];
uint8_t logHead = 0, logSize = 0;
String tripLines[TRIP_LOG_MAX];
uint8_t tripHead = 0, tripSize = 0;
String bootLines[BOOT_LOG_MAX];
uint8_t bootHead = 0, bootSize = 0;
// 0 = bisher unbekannt, 1 = zuletzt zuhause, 2 = zuletzt als abwesend eingestuft.
// In NVS gespeichert, um nach einem Neustart keine falsche "Ankunft" zu protokollieren.
uint8_t lastTripState = 0;
uint8_t bootLogIndex = 0;
bool bootTimePending = false;
String bootLogMessage;
uint32_t bootMs=0, bootCount=0, wifiReconnectCount=0, wifiDisconnectCount=0;
bool recoveryArmed=false, wifiEverOnline=false, otaStarted=false, otaInProgress=false;
String otaPassword;

String resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC / CRASH";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return String("Reset-Code ") + (int)esp_reset_reason();
  }
}

void loadOtaPassword() {
  Preferences p;
  if (!p.begin("mx30-ota", false)) return;
  otaPassword = p.getString("pw", "");
  if (otaPassword.length() < 12) {
    // Individuelles Passwort pro Basis, bei erstem Boot persistent erzeugt.
    char pw[25];
    snprintf(pw, sizeof(pw), "%08lx%08lx%08lx",
      (unsigned long)esp_random(), (unsigned long)esp_random(),
      (unsigned long)esp_random());
    otaPassword = pw;
    p.putString("pw", otaPassword);
  }
  p.end();
}

void startOta() {
  if (otaStarted || WiFi.status() != WL_CONNECTED || setupApOn ||
      (uint32_t)(millis()-bootMs) >= OTA_WINDOW_MS) return;
  ArduinoOTA.setHostname("mx30-basis");
  ArduinoOTA.setPort(3232);
  if (!otaPassword.isEmpty()) ArduinoOTA.setPassword(otaPassword.c_str());
  ArduinoOTA.onStart([]() { otaInProgress=true; Serial.println("OTA Upload gestartet"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA Upload abgeschlossen"); otaInProgress=false; });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Fehler: %u\n", (unsigned)error);
    otaInProgress=false;
  });
  ArduinoOTA.begin();
  otaStarted=true;
  Serial.printf("OTA aktiv, nur bis Boot + 5 Min. (Port 3232)\n");
  Serial.printf("OTA-Passwort (nur seriell): %s\n", otaPassword.c_str());
}

void serviceOta() {
  const bool expired=(uint32_t)(millis()-bootMs)>=OTA_WINDOW_MS;
  if (otaStarted) {
    if ((expired || WiFi.status()!=WL_CONNECTED || setupApOn) && !otaInProgress) {
      ArduinoOTA.end();
      otaStarted=false;
      Serial.println(expired ? "OTA-Fenster geschlossen (5 Minuten)" : "OTA wegen WLAN/AP gestoppt");
      return;
    }
    ArduinoOTA.handle();
  } else if (!expired) startOta();
}


String localClock() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%d.%m. %H:%M:%S", &t);
    return String(stamp);
  }
  return String("Laufzeit ") + (millis() / 1000) + " s";
}
void logEvent(const String &msg) {
  String entry = localClock() + "  " + msg;
  Serial.println(entry);
  logLines[logHead] = entry;
  prefs.putString((String("l") + logHead).c_str(), entry);
  logHead = (logHead + 1) % LOG_MAX;
  if (logSize < LOG_MAX) ++logSize;
  prefs.putUChar("head", logHead);
  prefs.putUChar("count", logSize);
}
void loadLog() {
  logHead = prefs.getUChar("head", 0) % LOG_MAX;
  logSize = min((int)prefs.getUChar("count", 0), LOG_MAX);
  for (int i = 0; i < LOG_MAX; ++i) {
    logLines[i] = prefs.getString((String("l") + i).c_str(), "");
  }
}

// Dieses Protokoll enthaelt ausschliesslich Aenderungen von "zuhause" / "weg".
// Eintrag ist der Zeitpunkt der ERKENNUNG, nicht zwingend der realen Abfahrt.
String tripClock() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%d.%m.%Y %H:%M", &t);
    return String(stamp);
  }
  return String("Uhrzeit unbekannt (Laufzeit ") + (millis() / 1000) + " s)";
}
void loadTripLog() {
  tripHead = tripPrefs.getUChar("head", 0) % TRIP_LOG_MAX;
  tripSize = min((int)tripPrefs.getUChar("count", 0), TRIP_LOG_MAX);
  lastTripState = tripPrefs.getUChar("state", 0);
  if (lastTripState > 2) lastTripState = 0;
  savedCarSleepSeconds = tripPrefs.getUShort("interval", 600);
  if (savedCarSleepSeconds < 30 || savedCarSleepSeconds > 900) savedCarSleepSeconds = 600;
  for (int i = 0; i < TRIP_LOG_MAX; ++i) {
    tripLines[i] = tripPrefs.getString((String("t") + i).c_str(), "");
  }
}
void addTripEvent(const String &event) {
  String entry = tripClock() + "  " + event;
  Serial.println(String("FAHRZEUGVERLAUF: ") + entry);
  tripLines[tripHead] = entry;
  tripPrefs.putString((String("t") + tripHead).c_str(), entry);
  tripHead = (tripHead + 1) % TRIP_LOG_MAX;
  if (tripSize < TRIP_LOG_MAX) ++tripSize;
  tripPrefs.putUChar("head", tripHead);
  tripPrefs.putUChar("count", tripSize);
}
void setTripState(uint8_t state) {
  if (state == lastTripState) return;
  if (state == 1 && lastTripState == 2) {
    addTripEvent("MX-30 ANGEKOMMEN (WLAN-Meldung empfangen)");
  } else if (state == 2 && lastTripState == 1) {
    addTripEvent("MX-30 WEG (vermutet: WLAN-Timeout)");
  }
  // Erster Kontakt nach Installation ist nur der Ausgangszustand,
  // keine erfundene Ankunft. Zustand ueber ESP32-Neustarts behalten.
  lastTripState = state;
  tripPrefs.putUChar("state", lastTripState);
}

// Eigenstaendiger, im Flash gespeicherter Systemstart-Verlauf.
// Die 60 Fahrzeug-Eintraege bleiben dadurch AUSSCHLIESSLICH Ankunft/Abfahrt.
void loadBootLog() {
  bootHead=bootPrefs.getUChar("head",0)%BOOT_LOG_MAX;
  bootSize=min((int)bootPrefs.getUChar("count",0),BOOT_LOG_MAX);
  for(int i=0;i<BOOT_LOG_MAX;++i)
    bootLines[i]=bootPrefs.getString((String("b")+i).c_str(),"");
}
void addBootEvent(const String &event) {
  const String entry=tripClock()+"  "+event;
  Serial.println(String("SYSTEMVERLAUF: ")+entry);
  bootLines[bootHead]=entry;
  bootPrefs.putString((String("b")+bootHead).c_str(),entry);
  bootHead=(bootHead+1)%BOOT_LOG_MAX;
  if(bootSize<BOOT_LOG_MAX)++bootSize;
  bootPrefs.putUChar("head",bootHead);
  bootPrefs.putUChar("count",bootSize);
}
// Die Systemzeit ist beim Einschalten oft noch ungueltig. Erst protokollieren,
// nach NTP-Synchronisierung die eigentliche Bootzeit zurueckrechnen.
void recordBootEvent() {
  bootLogMessage=String("Basis-Neustart #")+bootCount+" ("+resetReasonName()+")";
  bootLogIndex=bootHead;
  addBootEvent(bootLogMessage);
  bootTimePending=true;
}
void completeBootTimestamp() {
  if(!bootTimePending)return;
  time_t now=time(nullptr);
  if(now<=1700000000)return;
  if(!bootLines[bootLogIndex].endsWith(bootLogMessage)) {
    bootTimePending=false;
    return;
  }
  const time_t startedAt=now-(time_t)((millis()-bootMs)/1000UL);
  struct tm t;
  localtime_r(&startedAt,&t);
  char stamp[24];
  strftime(stamp,sizeof(stamp),"%d.%m.%Y %H:%M",&t);
  bootLines[bootLogIndex]=String(stamp)+"  "+bootLogMessage;
  bootPrefs.putString((String("b")+bootLogIndex).c_str(),bootLines[bootLogIndex]);
  bootTimePending=false;
  Serial.println(String("SYSTEM: Bootzeit nach NTP korrigiert: ")+bootLines[bootLogIndex]);
}

// Basis-Neustart: den zuletzt bekannten Zustand aus NVS uebernehmen,
// aber bis zu einer frischen Node-Meldung keine Shelly-Schaltbefehle senden.
// Ohne erneute Meldung wird ein zuvor anwesender Node nach dem normalen
// Timeout als "vermutlich weg" erkannt (10-Min-Schlaf => 21 Minuten).
void restorePresenceAfterRestart() {
  if (lastTripState == 0) return;  // Node wurde bisher nie beobachtet.
  carEverSeen = true;
  carStateRestored = true;
  nodeContactSinceBoot = false;
  nextCarSleepSeconds = savedCarSleepSeconds;
  carMoving = nextCarSleepSeconds <= 90;
  carLastMs = millis();  // Vorsichtig neue Wartezeit ab Start, keine falsche Abfahrt.
  carHome = lastTripState == 1;
  logEvent(carHome ?
    "Letzter Node-Status: ZUHAUSE (aus Speicher, noch unbestaetigt)" :
    "Letzter Node-Status: WEG (aus Speicher, noch keine frische Meldung)");
}

bool shellyGet() {
  if (shellyIp.isEmpty()) { shellyOnline=false; shellyHasMeter=false; return false; }
  if (WiFi.status() != WL_CONNECTED) { shellyOnline = false; return false; }
  ++shellyRequestCount;
  uint32_t started = millis();
  HTTPClient http;
  // Beide Timeouts benoetigt: setTimeout allein begrenzt nicht das TCP-Connect.
  http.setConnectTimeout(SHELLY_CONNECT_TIMEOUT_MS);
  http.setTimeout(SHELLY_READ_TIMEOUT_MS);
  String url = String("http://") + shellyIp + "/rpc/Switch.GetStatus?id=0";
  if (!http.begin(url)) { shellyOnline = false; lastShellyPollDurationMs = millis() - started; return false; }
  int status = http.GET();
  String response = (status == 200) ? http.getString() : "";
  http.end();
  lastShellyPollDurationMs = millis() - started;
  if (status != 200) { shellyOnline = false; return false; }
  JsonDocument json;
  if (deserializeJson(json, response) || json["output"].isNull()) {
    shellyOnline = false;
    return false;
  }
  bool actual = json["output"].as<bool>();
  if (shellyStatusSeen && actual != shellyOutput && !shellyCommandInProgress) {
    logEvent(String("Shelly-Zustand: ") + (actual ? "EIN" : "AUS") + " (Status gelesen)");
  }
  shellyOutput = actual;
  // Optionaler Lastschutz bei PM-Modellen. Ohne Messwert duerfen wir
  // einen eventuell aktiven Ladezyklus nicht per Automatik trennen.
  shellyHasMeter = !json["apower"].isNull() && !json["apower"].is<const char*>();
  shellyPowerW = shellyHasMeter ? json["apower"].as<float>() : 0.0f;
  if (shellyHasMeter && (!isfinite(shellyPowerW) || shellyPowerW < 0.0f)) {
    shellyHasMeter=false;  // Nie mit unplausibler Leistungsaufnahme abschalten.
  }
  shellyOnline = true;
  shellyStatusSeen = true;
  shellyStatusMs = millis();
  if (!desiredInitialized) {
    desiredOutput = shellyOutput;
    desiredInitialized = true;
  }
  return true;
}

bool shellySet(bool on) {
  // caller hat unmittelbar vorher Switch.GetStatus erfolgreich abgefragt.
  if (!controlEnabled || !shellyOnline || WiFi.status() != WL_CONNECTED) return false;
  if (!on && (!shellyHasMeter || shellyPowerW >= IDLE_LIMIT_W || carHome ||
      !shellyStatusSeen || (uint32_t)(millis()-shellyStatusMs)>2500UL)) {
    logEvent("AUS gesperrt: frische sichere Leerlaufmessung fehlt");
    return false;
  }
  HTTPClient http;
  http.setConnectTimeout(SHELLY_CONNECT_TIMEOUT_MS);
  http.setTimeout(SHELLY_READ_TIMEOUT_MS);
  String url = String("http://") + shellyIp +
      "/rpc/Switch.Set?id=0&on=" + (on ? "true" : "false");
  if (!http.begin(url)) return false;
  ++shellyRequestCount;
  int status = http.GET();
  http.end();
  if (status != 200) {
    shellyOnline=false;
    logEvent(String("Shelly-Befehl fehlgeschlagen (HTTP ") + status + ")");
    return false;
  }
  // Nur bei Schaltvorgaengen den tatsaechlichen Zustand ruecklesen.
  shellyCommandInProgress = true;
  bool confirmed = shellyGet() && shellyOutput == on;
  shellyCommandInProgress = false;
  if (!confirmed) {
    logEvent("Shelly-Schaltbefehl nicht bestaetigt");
    return false;
  }
  logEvent(String("Shelly ") + (on ? "EIN" : "AUS") + " - vom ESP32 geschaltet");
  return true;
}

// Ereignisse werden in der Hauptschleife verarbeitet; niemals aus der
// Webserver-Statusabfrage heraus und niemals durch periodisches Polling.
// Eine echte Zustandsaenderung legt einen neuen Sollzustand an.
// Bei Strom-/Netzausfall bleibt die Warteschlange in RAM bestehen. Nach
// Basis-Neustart ist aus Sicherheitsgruenden eine FRISCHE Node-Meldung noetig.
void scheduleShellyAction(bool home, bool waitForAwayGrace=true) {
  // Nur bei tatsaechlich geaendertem Sollzustand in NVS schreiben.
  if (!desiredInitialized || desiredOutput!=home) {
    prefs.putBool("desired",home);
    prefs.putBool("desiredValid",true);
  }
  desiredInitialized=true;
  desiredOutput=home;
  pendingAction=home ? ACTION_ON : ACTION_OFF;
  offGraceDeadlineMs=home ? 0UL :
      millis() + (waitForAwayGrace ? ABSENT_OFF_GRACE_MS : 0UL);
  actionDueMs=home ? millis() : offGraceDeadlineMs;
  actionAttempts=0;
  offSafeSamples=0;
  shellyDecision=home ? "Ankunft: EIN-Abgleich vorgemerkt" :
      (waitForAwayGrace ? "Abwesenheit: OFF-Pruefung nach 10 Min" :
       "Abwesenheit: OFF-Abgleich vorgemerkt");
  if (!controlEnabled) {
    logEvent(home ? "TEST: Shelly waere EIN" :
                    "TEST: Shelly waere nach Sicherheitspruefung AUS");
    pendingAction=ACTION_NONE;
  }
}

// Kein regelmaessiger Shelly-Health-Check: Es werden NUR fehlgeschlagene
// konkrete Schaltwuensche erneut probiert. Zunehmender Abstand reduziert Last
// und Flash-Schreibvorgaenge. Nach 5 Min. bleibt die Warteschlange bestehen.
void postponeShellyAction(const String &reason) {
  offSafeSamples=0;  // Eine spaetere Messung darf keine alte Freigabe uebernehmen.
  if (actionAttempts < 250) ++actionAttempts;
  uint32_t waitMs=COMMAND_RETRY_MS;
  for (uint8_t i=1; i<actionAttempts && i<6; ++i) {
    waitMs*=2;
    if (waitMs>=MAX_COMMAND_RETRY_MS) { waitMs=MAX_COMMAND_RETRY_MS; break; }
  }
  actionDueMs=millis()+waitMs;
  shellyDecision=reason+"; neuer Versuch in "+String(waitMs/1000)+" s";
  // Nur wenige Fehlermeldungen in NVS statt alle 5 Minuten fuer immer.
  if (actionAttempts==1 || actionAttempts==2 || actionAttempts==4 ||
      actionAttempts==8 || actionAttempts==16) logEvent(shellyDecision);
}

void processShellyAction() {
  if (pendingAction==ACTION_NONE || !controlEnabled) return;
  if ((int32_t)(millis()-actionDueMs)<0) return;

  // Kein Schalten auf Basis unbestaetigter, aus NVS rekonstruierter Daten.
  if (carStateRestored || !nodeContactSinceBoot) {
    pendingAction=ACTION_NONE;
    offSafeSamples=0;
    shellyDecision="Frische Node-Meldung fehlt: Steuerung gesperrt";
    return;
  }
  const bool wantedOn = pendingAction==ACTION_ON;
  if ((wantedOn && !carHome) || (!wantedOn && carHome)) {
    pendingAction=ACTION_NONE;
    offSafeSamples=0;
    shellyDecision="Autozustand veraendert: alte Aktion verworfen";
    return;
  }
  if (WiFi.status()!=WL_CONNECTED || shellyIp.isEmpty()) {
    shellyOnline=false;
    if (shellyIp.isEmpty()) {
      pendingAction=ACTION_NONE;
      shellyDecision="Keine Shelly-IP, Steuerung nicht moeglich";
    } else postponeShellyAction("Basis WLAN offline");
    return;
  }

  // Jeder Stichprobenwert muss FRISCH vom Shelly stammen.
  if (!shellyGet()) {
    postponeShellyAction("Shelly nicht erreichbar");
    return;
  }
  if (shellyOutput==wantedOn) {
    shellyDecision=wantedOn ? "Soll EIN bestaetigt" : "Soll AUS bestaetigt";
    pendingAction=ACTION_NONE;
    offSafeSamples=0;
    actionAttempts=0;
    return;
  }
  if (wantedOn) {
    if (shellySet(true)) {
      shellyDecision="Shelly EIN bestaetigt";
      pendingAction=ACTION_NONE;
      actionAttempts=0;
    } else postponeShellyAction("Shelly EIN nicht bestaetigt");
    return;
  }

  // SCHUTZ: Nur bei wirklich abwesendem Node, eingeschaltetem Shelly und
  // 6 aufeinanderfolgenden gueltigen Messungen unter 20 W darf OFF erfolgen.
  if (!shellyHasMeter) {
    shellyDecision="OFF gesperrt: keine gueltige Leistungsmessung";
    logEvent(shellyDecision);
    pendingAction=ACTION_NONE;
    offSafeSamples=0;
    return;
  }
  if (shellyPowerW>=IDLE_LIMIT_W) {
    shellyDecision="OFF gesperrt: Ladevorgang/Last erkannt (>= 20 W)";
    if (offSafeSamples>0) logEvent("OFF-Pruefung abgebrochen: Last waehrend Messreihe");
    else logEvent(shellyDecision);
    pendingAction=ACTION_NONE; // Kein Dauer-Polling nach erkannter Last.
    offSafeSamples=0;
    return;
  }
  if (offSafeSamples < OFF_SAMPLES_REQUIRED) ++offSafeSamples;
  shellyDecision=String("OFF-Leerlaufpruefung: ")+String(offSafeSamples)+"/"+String(OFF_SAMPLES_REQUIRED)+
                 " sicher (<20 W)";
  if (offSafeSamples < OFF_SAMPLES_REQUIRED) {
    actionDueMs=millis()+OFF_SAMPLE_INTERVAL_MS;
    return;
  }
  // Letzter Wert ist frisch (<2,5s) und ShellySet prueft ihn ebenfalls.
  // Auch mehrere Werte beweisen jedoch NICHT das Ende einer geplanten Ladepause.
  if (shellySet(false)) {
    shellyDecision="Shelly AUS bestaetigt (6 Leerlaufwerte)";
    pendingAction=ACTION_NONE;
    offSafeSamples=0;
    actionAttempts=0;
  } else postponeShellyAction("OFF-Schaltbefehl nicht bestaetigt");
}

// Letzte erwartete Meldung + tolerierte verpasste Meldungen.
uint32_t awayTimeoutMs() {
  if (nextCarSleepSeconds <= 90) return 210000UL;  // ca. 3,5 Minuten
  return 2UL * (uint32_t)nextCarSleepSeconds * 1000UL + 60000UL; // 21 Minuten bei 10-min-Ruhe
}
void evaluatePresence() {
  if (!carEverSeen || !carHome) return;
  uint32_t elapsed = (uint32_t)(millis() - carLastMs);
  const uint32_t deadline = awayTimeoutMs();
  if (elapsed >= deadline) {
    carHome = false;
    carStateRestored = false;
    logEvent("MX-30 vermutlich WEG (Node-Timeout erreicht)");
    setTripState(2);
    if (nodeContactSinceBoot) scheduleShellyAction(false);
  } else if (!overdueLogged &&
             elapsed >= ((uint32_t)nextCarSleepSeconds * 1000UL + 30000UL)) {
    overdueLogged = true;
    logEvent("MX-30: erwartete Node-Meldung ueberfaellig; warte Timeout");
  }
}

void incomingHeartbeat(uint32_t counter, bool moving, uint16_t plannedSleep,
                       int rssi, int wakeReason) {
  bool wasHome = carHome;
  bool wasMoving = carMoving;
  bool firstFreshAfterRestart = !nodeContactSinceBoot;
  carEverSeen = true;
  carHome = true;
  carStateRestored = false;
  nodeContactSinceBoot = true;
  overdueLogged = false;
  carLastMs = millis();
  nextCarSleepSeconds = plannedSleep;
  // Nur bei Intervallwechsel in Flash schreiben, nicht bei jedem Heartbeat.
  if (savedCarSleepSeconds != plannedSleep) {
    savedCarSleepSeconds = plannedSleep;
    tripPrefs.putUShort("interval", savedCarSleepSeconds);
  }
  carMoving = moving;
  carRssi = rssi;
  carWakeCause = wakeReason;
  rxCount++;
  lastCarSequence = counter;
  if (firstFreshAfterRestart || !wasHome) scheduleShellyAction(true);
  if (!wasHome) {
    logEvent("MX-30 ERKANNT: zu Hause");
    setTripState(1);
  }
  else if (wasMoving != carMoving)
    logEvent(String("MX-30 ") + (moving ? "Bewegung erkannt" : "steht / Ruhemodus"));
}
void receiveUdp() {
  if (!udpStarted) return;
  for (int run = 0; run < 4; ++run) {
    int size = udp.parsePacket();
    if (!size) break;
    if (size >= 240 || size < 15) {
      while (udp.available()) udp.read();
      continue;
    }
    // DHCP / wechselnde Heimnetze: Sender ueber gemeinsamen UDP-Token pruefen, nicht feste IP.
    char buffer[240];
    int len = udp.read(buffer, sizeof(buffer)-1);
    if (len <= 0) continue;
    buffer[len] = '\0';
    String value(buffer);
    String fields[10];
    int count = 0, begin = 0;
    for (int i = 0; i <= value.length() && count < 10; ++i) {
      if (i == value.length() || value.charAt(i) == '|') {
        fields[count++] = value.substring(begin, i);
        begin = i + 1;
      }
    }
    if (fields[0] != "MX30" || fields[2] != MX30_AUTH_TOKEN) continue;
    if (count == 5 && fields[1] == "CFG") {
      // Rueckmeldung vom Node NACH erfolgreicher Anwendung auf dem Sensor.
      const int applied = fields[3].toInt();
      if (applied < 0 || applied > 100 ||
          fields[3] != String(applied) ||
          (fields[4] != "0" && fields[4] != "1")) continue;
      nodeSensitivityKnown = true;
      nodeAppliedSensitivity = (uint8_t)applied;
      nodeSensorOK = fields[4] == "1";
      if (nodeSensorOK && nodeAppliedSensitivity == motionSensitivity) {
        if (motionSyncPending) logEvent("Node bestaetigt Sensorwert " + String(motionSensitivity) + "/100");
        motionSyncPending = false;
      }
      continue;
    }
    if ((count != 8 && count != 10) || fields[1] != "HB") continue;
    uint32_t sequence = (uint32_t)fields[3].toInt();
    bool moving = fields[4] == "1";
    int interval = fields[5].toInt();
    if (interval < 30 || interval > 900 || (fields[4] != "0" && !moving)) continue;
    int rssi = fields[6].toInt();
    int wakeReason = fields[7].toInt();
    // Alte Node-Firmware bleibt erreichbar, kann aber keine Einstellungen empfangen.
    if (count == 10) {
      const int sensed = fields[8].toInt();
      if (sensed < 0 || sensed > 100 || fields[8] != String(sensed) ||
          (fields[9] != "0" && fields[9] != "1")) continue;
      nodeSensitivityKnown = true;
      nodeAppliedSensitivity = (uint8_t)sensed;
      nodeSensorOK = fields[9] == "1";
      if (nodeSensorOK && nodeAppliedSensitivity == motionSensitivity) motionSyncPending = false;
    } else {
      nodeSensitivityKnown = false;
      nodeSensorOK = false;
      motionSyncPending = true;
    }
    // ACK mit gewuenschtem Sensorwert. Keine zweite WLAN-Verbindung noetig.
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.print(String("MX30|ACK|") + MX30_AUTH_TOKEN + "|S:" + String(motionSensitivity));
    udp.endPacket();
    incomingHeartbeat(sequence, moving, (uint16_t)interval, rssi, wakeReason);
  }
}

void ensureWifi() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    wifiDownSinceMs=0;
    if (!lastWifiOnline) {
      const bool wasReconnect=wifiEverOnline;
      if (wifiEverOnline) ++wifiReconnectCount;
      wifiEverOnline=true;
      lastWifiOnline=true;
      if (!recoveryArmed) { recoveryArmed=true; prefs.putBool("recover", true); }
      logEvent(String("Basis im WLAN: ") + WiFi.localIP().toString());
      Serial.printf("BASIS IP: %s | Gateway: %s | RSSI: %d dBm\n",
        WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
      Serial.printf("Webinterface: http://%s/\n", WiFi.localIP().toString().c_str());
      if (setupApOn) {
        WiFi.softAPdisconnect(true);
        setupApOn=false;
      }
      // Server nach Netzausfall erneut binden, falls die alte Socket-Bindung hing.
      web.stop(); web.begin();
      MDNS.end();
      if (!MDNS.begin("mx30-basis")) logEvent("mDNS konnte nicht starten");
      else MDNS.addService("http", "tcp", 80);
      // Nach Wiederverbindung den aktuellen Sollzustand erneut abgleichen.
      // Nur bei nach DIESEM Boot bestaetigter Anwesenheitsinformation.
      // Eine bestehende Aktion bleibt bestehen; sie wird nur sofort faellig.
      if (wasReconnect && controlEnabled && nodeContactSinceBoot && !carStateRestored) {
        if (pendingAction != ACTION_NONE) {
          offSafeSamples=0; // Nach Netzunterbrechung Messreihe neu beginnen.
          actionDueMs=(pendingAction==ACTION_OFF &&
              (int32_t)(millis()-offGraceDeadlineMs)<0) ?
              offGraceDeadlineMs : millis(); // 10-Min-Schonfrist erhalten.
          actionAttempts=0;
          logEvent("WLAN wieder online: ausstehende Shelly-Aktion wird erneut geprueft");
        } else if (carHome && (uint32_t)(millis()-carLastMs)<awayTimeoutMs()) {
          scheduleShellyAction(true);
          logEvent("WLAN wieder online: Sollzustand EIN abgleichen");
        } else if (!carHome) {
          // Neue, vorsichtige 10-Minuten-Schonfrist vor jeder OFF-Messreihe.
          scheduleShellyAction(false,true);
          logEvent("WLAN wieder online: Sollzustand AUS spaeter sicher pruefen");
        }
      }
    }
    if (!udpStarted) {
      udpStarted=udp.begin(UDP_PORT);
      if(!udpStarted) logEvent("UDP-Empfaenger konnte nicht starten");
    }
    return;
  }

  if (lastWifiOnline) {
    ++wifiDisconnectCount;
    logEvent("Basis-WLAN getrennt; Wiederverbindung laeuft");
    if (otaStarted && !otaInProgress) {
      ArduinoOTA.end(); otaStarted=false;
    }
    MDNS.end();
  }
  if (!wifiDownSinceMs) wifiDownSinceMs=millis() ? millis() : 1;
  lastWifiOnline=false;
  shellyOnline=false;
  if (udpStarted) { udp.stop(); udpStarted=false; }
  if (wlanSsid.isEmpty()) {
    if (!setupApOn) startAp();
    return;
  }
  const uint32_t offlineMs=(uint32_t)(millis()-wifiDownSinceMs);
  if (offlineMs>=SETUP_FALLBACK_MS && !setupApOn) startAp();
  // WiFi.reconnect() statt staendigem WiFi.begin()/WiFi.config().
  if ((!wifiAttemptMs || (uint32_t)(millis()-wifiAttemptMs)>=WIFI_RETRY_MS)) {
    wifiAttemptMs=millis();
    WiFi.reconnect();
    Serial.printf("WLAN-Reconnect (Status %d), offline seit %lu s\n",
      (int)WiFi.status(), (unsigned long)(offlineMs/1000));
  }
  // Einmaliger Rettungs-Neustart bei anhaltendem WLAN-Ausfall.
  // Das Flag wird erst nach erneut erfolgreicher Verbindung wieder scharf.
  if (offlineMs>=WIFI_RESTART_AFTER_MS && recoveryArmed && !otaInProgress) {
    recoveryArmed=false;
    prefs.putBool("recover", false);
    logEvent("WLAN seit 10 Min. offline; einmaliger Recovery-Neustart");
    Serial.flush();
    delay(70);
    ESP.restart();
  }
}

// Statischer Web-Client; Status und Log kommen ohne Seitenreload ueber JSON.
static const char DASHBOARD[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MX-30 Smart Charging</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}body{font:15px/1.48 system-ui,-apple-system,sans-serif;background:#101720;color:#edf3f8;margin:0;padding:18px}
main{max-width:820px;margin:auto}h1{font-size:23px;margin:8px 0 3px}p.sub{color:#94a9bb;margin-top:0}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.card{background:#1d2936;padding:16px;border-radius:16px;margin:12px 0}.grid .card{margin:0}.lbl{font-size:12px;color:#a1b4c5}.val{font-size:23px;font-weight:700;margin-top:4px}
.ok{color:#72e5af}.warn{color:#ffca71}.muted{color:#a8b7c5}.row{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}
button{background:#dceaf5;color:#142230;border:0;border-radius:10px;padding:11px 16px;font-weight:700;cursor:pointer}button.danger{background:#f6b6ab}
small{color:#a5b5c4}ul{list-style:none;padding:0;margin:8px 0}li{padding:8px 0;border-bottom:1px solid #3a4854;overflow-wrap:anywhere}
code{color:#cce1ee}.tag{font-size:13px;padding:4px 9px;border-radius:30px;background:#37434b}
@media(max-width:520px){.val{font-size:20px}}
</style></head><body><main>
<h1>MX-30 Smart Charging</h1><p class="sub">Lokale Basisstation · ESP32-WROOM-32D · v2.11.1</p>
<div class="card"><div class="row"><div><div class="lbl">Shelly Control</div><div class="val" id="control">Lade…</div><small id="controlNote">Im Testmodus werden keine Befehle gesendet.</small></div><button id="controlButton" onclick="toggleControl()" disabled>…</button></div></div>
<div class="grid">
<div class="card"><div class="lbl">MX-30</div><div class="val" id="car">–</div><small id="carAge">Keine Daten</small></div>
<div class="card"><div class="lbl">Shelly-Hardware</div><div class="val" id="shelly">–</div><small id="shellyOnline">Status nur nach Ereignissen aktualisiert</small></div>
<div class="card"><div class="lbl">Shelly-Typ</div><div class="val" id="meter">–</div><small>Leistungsmessung nur intern als Ladeschutz, falls vorhanden</small></div>
<div class="card"><div class="lbl">OTA-Fenster</div><div class="val" id="ota">–</div><small>Nur 5 Minuten nach jedem Neustart</small></div>
</div>
<div class="card"><strong>Systemdiagnose</strong><div id="systemDiagnostics" class="muted" style="margin-top:9px;line-height:1.9">Warte auf Werte…</div><div style="margin-top:12px"><button onclick="rebootForOta()">Basis neu starten / OTA-Fenster oeffnen</button></div></div>
<div class="card"><strong>Diagnose</strong><div id="diagnostics" class="muted" style="margin-top:9px;line-height:1.9">Warte auf Werte…</div></div>
<div class="card"><strong>Bewegungssensor MMA8452Q</strong>
<p class="muted">Empfindlichkeit einstellen: 0 = sehr unempfindlich, 100 = sehr empfindlich. Der Node uebernimmt den Wert bei der naechsten WLAN-Meldung.</p>
<div class="row" style="margin-top:12px">
<input id="motionSlider" type="range" min="0" max="100" value="50" oninput="editMotion(this.value)" style="flex:1;min-width:150px;accent-color:#72e5af">
<input id="motionNumber" type="number" min="0" max="100" step="1" value="50" oninput="editMotion(this.value)" style="width:84px;padding:9px;border-radius:8px">
<button id="motionSave" onclick="saveMotion()">Speichern</button>
</div><p><small id="motionMessage">Warte auf Einstellung...</small></p>
<div class="muted" id="motionNodeStatus">Node-Bestaetigung ausstehend</div>
</div>
<div class="card"><div class="row"><strong>Fahrzeugverlauf – nur Ankunft & Abfahrt</strong><small>Letzte 60 Wechsel · gespeichert</small></div>
<p class="muted">Abfahrtszeit = Zeitpunkt der Timeout-Erkennung, nicht zwingend der wirkliche Fahrtbeginn.</p>
<ul id="tripEvents"><li class="muted">Noch kein Wechsel erfasst.</li></ul></div>
<div class="card"><div class="row"><strong>System-Neustarts</strong><small>Letzte 20 Starts · gespeichert</small></div>
<p class="muted">Neustarts und Reset-Grund (z. B. Watchdog, Brownout, Software-Reset). Uhrzeit wird nach NTP-Synchronisierung nachgetragen.</p>
<ul id="bootEvents"><li class="muted">Noch kein Start erfasst.</li></ul></div>
<div class="card"><strong>Shelly-Einstellungen</strong>
<div style="margin:9px 0"><button onclick="testShelly()">Shelly-Verbindung jetzt testen</button> <button onclick="recheckAbsent()">Abwesenheit neu bewerten</button><small id="shellyTestNote" style="margin-left:9px">Kein automatisches Polling</small></div>
<p class="muted">Aktuell konfiguriert: <code id="shellyCurrentIp">–</code>. Hier kannst du die Shelly-IP nach Austausch, Reset oder Routerwechsel aendern. Ohne Neustart.</p>
<form id="shellyForm" onsubmit="return saveShellyIp(event)">
<label for="shellyIp" class="lbl">Shelly-IP-Adresse (IPv4)</label>
<div class="row" style="margin-top:7px"><input name="shellyIp" id="shellyIp" type="text" inputmode="decimal" autocomplete="off" maxlength="15" placeholder="192.168.178.40" style="flex:1;min-width:180px;padding:10px"><button type="submit" id="shellySave">Shelly-IP speichern</button><button type="button" class="danger" onclick="clearShellyIp()">Shelly-IP loeschen</button></div>
</form><p><small id="shellySaveNote">Wenn keine IP gespeichert ist, werden keine Shelly-Befehle gesendet. Bei Aenderung: Steuerung AUS (Testmodus).</small></p></div>
<div class="card"><strong>WLAN-Einstellungen</strong><p class="muted">Aktuelles WLAN: <span id="currentSsid">–</span>. Zum Aendern neue Daten speichern; die Basis startet danach neu. DHCP funktioniert auch in anderen Heimnetzen.</p>
<form method="POST" action="/wifi/save">
<label class="lbl">WLAN-SSID (2,4 GHz)</label><input name="ssid" id="ssid" required maxlength="32" placeholder="WLAN-Name" style="display:block;width:100%;padding:10px;margin:5px 0 12px">
<label class="lbl">WLAN-Passwort (leer = bisheriges behalten)</label><input type="password" name="pass" maxlength="63" style="display:block;width:100%;padding:10px;margin:5px 0 12px">
<label><input type="checkbox" name="fixed" id="fixed" value="1"> Feste IP-Adresse (sonst DHCP)</label>
<div class="grid">
<div><label class="lbl">Basis-IP</label><input name="ip" id="ip" required style="width:100%;padding:9px"></div>
<div><label class="lbl">Gateway</label><input name="gw" id="gw" required style="width:100%;padding:9px"></div>
<div><label class="lbl">Subnetz</label><input name="mask" id="mask" required style="width:100%;padding:9px"></div>
<div><label class="lbl">DNS</label><input name="dns" id="dns" required style="width:100%;padding:9px"></div>
</div><button type="submit" onclick="return confirm('WLAN-Daten speichern und die Basis neu starten?')">WLAN speichern & neu starten</button>
</form><p><small>Setup-AP bei fehlendem WLAN: MX30-Basis-Setup, http://192.168.4.1/</small></p></div>
<div class="card"><div class="row"><strong>Letzte 10 Aktivitäten</strong><small>Auch nach Neustart gespeichert</small></div><ul id="events"></ul></div>
<p class="sub">Die Webseite aktualisiert sich alle 4 Sekunden, fragt dabei aber NICHT den Shelly ab. Ohne Login. OTA ist durch ein individuelles Passwort und ein 5-Minuten-Fenster begrenzt. Setup-WLAN ist offen: keine Portfreigabe!</p>
</main><script>
let current=false, canEnable=false, statusInFlight=false, tripsInFlight=false, bootsInFlight=false;
let motionEditing=false;
function editMotion(v){
  motionEditing=true;
  if(String(v).trim()==='') return;
  const n=Number(v);
  if(!Number.isFinite(n)) return;
  const bounded=Math.max(0,Math.min(100,Math.round(n)));
  el('motionSlider').value=bounded;
  if(document.activeElement!==el('motionNumber')) el('motionNumber').value=bounded;
}
async function saveMotion(){
  const val=Number(el('motionNumber').value);
  const message=el('motionMessage');
  if(!Number.isInteger(val)||val<0||val>100){message.textContent='Bitte ganze Zahl von 0 bis 100 eingeben.';return;}
  el('motionSave').disabled=true;
  try{
    const r=await fetch('/api/motion',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'value='+encodeURIComponent(val)});
    if(!r.ok)throw Error(await r.text());
    motionEditing=false;
    message.textContent='Gespeichert. Node uebernimmt den Wert bei seiner naechsten Meldung.';
    await update();
  }catch(e){message.textContent='Fehler: '+e.message;}
  finally{el('motionSave').disabled=false;}
}

const el=(id)=>document.getElementById(id);
async function update(){if(statusInFlight)return;statusInFlight=true;const start=performance.now();try{
 const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw Error('HTTP '+response.status);
 const d=await response.json();current=d.controlEnabled;canEnable=d.canEnable;
 el('control').textContent=d.controlEnabled?'AKTIV – Live':'AUS – Testmodus';
 el('control').className='val '+(d.controlEnabled?'warn':'ok');
 el('controlButton').disabled=!d.controlEnabled&&!d.canEnable;
 el('controlButton').textContent=d.controlEnabled?'Shelly Control deaktivieren':'Shelly Control aktivieren';
 el('controlButton').className=d.controlEnabled?'danger':'';
 el('controlNote').textContent=d.controlEnabled?(d.shellyHasMeter?'Automatik aktiv; Ladeleistung wird intern zum Abschaltschutz geprueft.':'Automatik aktiv; ohne Leistungsmessung ist automatisches AUS gesperrt.'):'Keine Schaltbefehle; nur beobachten und simulieren.';
 el('car').textContent=d.carState;
 el('carAge').textContent=d.lastSeenAge+' | '+d.nodeSignal;
 if(!motionEditing){el('motionSlider').value=d.motionSensitivity;el('motionNumber').value=d.motionSensitivity;}
 el('motionNodeStatus').textContent=!d.nodeSensitivityKnown
   ?'Noch keine Rueckmeldung der neuen Node-Firmware (Node v2.11 erforderlich).'
   :!d.nodeSensorOK
   ?'Node meldet MMA8452Q nicht bereit; Sensor pruefen.'
   :d.motionSyncPending || d.nodeAppliedSensitivity!==d.motionSensitivity
   ?'Basis: '+d.motionSensitivity+'/100 | Node zuletzt: '+d.nodeAppliedSensitivity+'/100 – Uebernahme ausstehend.'
   :'Synchronisiert: '+d.nodeAppliedSensitivity+'/100 – vom Node bestaetigt.';
 el('shelly').textContent=!d.shellyIp?'Nicht eingerichtet':d.shellyOnline?(d.shellyOutput?'EIN (zuletzt)':'AUS (zuletzt)'):'Status unbekannt';el('shellyOnline').textContent=!d.shellyIp?'Shelly-IP nicht konfiguriert':d.shellyOnline?'Letzte Abfrage erfolgreich vor '+d.shellyStatusAge+' s (kein Live-Status)':'Letzte Abfrage fehlgeschlagen / noch nicht geprueft';
 el('meter').textContent=d.shellyOnline?(d.shellyHasMeter?'PM-Modell':'Ohne PM'):'–';
 el('ota').textContent=d.otaActive ? (d.otaRemaining+' s') : 'Geschlossen';
 el('systemDiagnostics').textContent=`Uptime: ${d.uptimeSec} s | Freier Heap: ${d.freeHeap} Bytes | Neustarts: ${d.bootCount} | Letzter Reset: ${d.resetReason} | WLAN RSSI: ${d.baseRssi} dBm | Reconnects: ${d.reconnectCount} | WLAN-Abbrueche: ${d.disconnectCount} | OTA: ${d.otaActive?'aktiv':'aus'} | OTA-Passwort steht im seriellen Monitor.`;
 el('currentSsid').textContent=d.ssid||'nicht konfiguriert';
 el('shellyCurrentIp').textContent=d.shellyIp||'nicht konfiguriert';
 if(!el('shellyIp').dataset.init){el('shellyIp').value=d.shellyIp||'';el('shellyIp').dataset.init='1';}
 if(!document.getElementById('ssid').dataset.init){ el('ssid').value=d.ssid||'';el('fixed').checked=!!d.fixedIp;el('ip').value=d.configIp;el('gw').value=d.gateway;el('mask').value=d.mask;el('dns').value=d.dns;document.getElementById('ssid').dataset.init='1';}
 el('diagnostics').textContent=`Basis-IP: ${d.baseIp} | WLAN: ${d.wifi?'online':'offline'} | Basis-Pruefung in: ${d.nextBaseCheck} (alle ${d.baseCheckInterval} s) | Naechste Node-Meldung ca.: ${d.nextExpected} | Abwesenheits-Timeout: ${d.absenceTimeoutSec} s | Bis "WEG": ${d.untilAbsent} | Signalstatus: ${d.nodeSignal} | Funkmodus: ${d.motion?'Bewegung':'Ruhe'} | ESP32-RSSI: ${d.rssi} dBm | Meldungen: ${d.rxCount} | Shelly-Anfragen seit Neustart: ${d.shellyRequestCount} | Shelly-HTTP zuletzt: ${d.shellyPollMs} ms | Shelly-Aktion: ${d.pendingAction} | OFF-Proben: ${d.offSafeSamples}/${d.offSamplesRequired} | Naechste Aktion: ${d.nextShellyAction} | Retry: ${d.actionAttempts} | Shelly-Entscheidung: ${d.shellyDecision} | Soll-Zustand Shelly: ${d.desiredOutput?'EIN':'AUS'} | Grund: ${d.reason} | Statusantwort: ${Math.round(performance.now()-start)} ms`;
 const ul=el('events');ul.replaceChildren();for(const e of d.events){const li=document.createElement('li');li.textContent=e;ul.appendChild(li)}
}catch(e){el('carAge').textContent='Keine Verbindung zur Basis: '+e.message;}finally{statusInFlight=false;}}
async function updateTrips(){if(tripsInFlight)return;tripsInFlight=true;try{
 const response=await fetch('/api/trips',{cache:'no-store'});if(!response.ok)throw Error('HTTP '+response.status);
 const d=await response.json();const tripUl=el('tripEvents');tripUl.replaceChildren();
 if(d.tripEvents.length===0){const li=document.createElement('li');li.className='muted';li.textContent='Noch kein Wechsel zwischen Zuhause und Abwesend erfasst.';tripUl.appendChild(li)}
 for(const e of d.tripEvents){const li=document.createElement('li');li.textContent=e;tripUl.appendChild(li)}
}catch(e){console.warn('Fahrzeugverlauf:',e.message)}finally{tripsInFlight=false;}}
async function updateBoots(){if(bootsInFlight)return;bootsInFlight=true;try{
 const r=await fetch('/api/boots',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);
 const d=await r.json(), ul=el('bootEvents');ul.replaceChildren();
 if(d.bootEvents.length===0){const li=document.createElement('li');li.textContent='Noch kein Neustart erfasst.';ul.appendChild(li)}
 for(const e of d.bootEvents){const li=document.createElement('li');li.textContent=e;ul.appendChild(li)}
}catch(e){console.warn('Systemstarts:',e.message)}finally{bootsInFlight=false;}}
async function saveShellyIp(ev){
 ev.preventDefault();
 const newIp=el('shellyIp').value.trim();
 const note=el('shellySaveNote');
 if(!/^\d{1,3}(\.\d{1,3}){3}$/.test(newIp) || newIp.split('.').some(x=>Number(x)>255)){
  note.textContent='Bitte eine gueltige IPv4-Adresse eintragen.';return false;
 }
 if(!confirm('Shelly-IP zu '+newIp+' aendern? Falls die IP wechselt, wird Shelly Control zur Sicherheit deaktiviert.'))return false;
 el('shellySave').disabled=true;
 try{
  const body='ip='+encodeURIComponent(newIp);
  const response=await fetch('/api/shelly/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  if(!response.ok)throw Error(await response.text());
  const result=await response.json();
  el('shellyIp').value=result.ip;
  note.textContent=result.changed?'Gespeichert. Shelly Control AUS; bitte Shelly-Verbindung pruefen.':'IP unveraendert, Einstellungen bereits gespeichert.';
  await update();
 }catch(e){note.textContent='Speichern fehlgeschlagen: '+e.message;}
 finally{el('shellySave').disabled=false;}
 return false;
}
async function testShelly(){
 const note=el('shellyTestNote');note.textContent='Verbindung wird einmalig getestet…';
 try {const r=await fetch('/api/shelly/test',{method:'POST'});
  if(!r.ok) throw Error(await r.text());
  const d=await r.json();note.textContent=d.ok?'Shelly erreichbar (Status gelesen)':'Shelly nicht erreichbar';
  await update();
 }catch(e){note.textContent='Test fehlgeschlagen: '+e.message;}
}
async function recheckAbsent(){
 if(!current){alert('Zuerst Shelly Control aktivieren.');return;}
 if(!confirm('Abwesenheit neu bewerten? Nach Freigabe werden 6 Messwerte mit jeweils 5 Sekunden Abstand benoetigt, bevor bei nachgewiesen geringem Verbrauch AUS geschaltet wird.'))return;
 const note=el('shellyTestNote');
 try{const r=await fetch('/api/shelly/recheck',{method:'POST'});
  if(!r.ok)throw Error(await r.text());note.textContent='Einmalige Abschaltpruefung angefordert';await update();
 }catch(e){note.textContent='Pruefung nicht moeglich: '+e.message;}
}
async function clearShellyIp(){
 if(!confirm('Shelly-IP wirklich loeschen? Shelly Control wird deaktiviert und kein Relais mehr geschaltet.'))return;
 const note=el('shellySaveNote');
 try{
  const response=await fetch('/api/shelly/clear',{method:'POST'});
  if(!response.ok)throw Error(await response.text());
  el('shellyIp').value='';note.textContent='Shelly-IP geloescht. Control ist AUS.';
  await update();
 }catch(e){note.textContent='Loeschen fehlgeschlagen: '+e.message;}
}
async function rebootForOta(){
 if(!confirm('Basis neu starten? Ein laufender OTA-Upload oder Web-Zugriff wird unterbrochen. Der Shelly bleibt in seiner aktuellen Stellung.'))return;
 try{
  const response=await fetch('/api/reboot',{method:'POST'});
  if(!response.ok)throw Error(await response.text());
  alert('Basis startet neu. OTA ist in den ersten 5 Minuten nach dem Start verfuegbar (sofern WLAN verbunden).');
 }catch(e){alert('Neustart: '+e.message);}
}
async function toggleControl(){const requested=!current;
 if(requested&&!confirm('ECHTES Shelly-Schalten aktivieren? Vorher nur Testmodus.'))return;
 if(!requested&&!confirm('Automatische Shelly-Steuerung deaktivieren? Relais bleibt in seiner aktuellen Stellung.'))return;
 try{const response=await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'enable='+(requested?'1':'0')});
 if(!response.ok)throw Error(await response.text());await update();}catch(e){alert('Umschalten fehlgeschlagen: '+e.message)}
}
update();updateTrips();updateBoots();setInterval(update,4000);setInterval(updateTrips,30000);setInterval(updateBoots,30000);
</script></body></html>
)HTML";

void handleSaveMotion() {
  if (!web.hasArg("value")) {web.send(400,"text/plain","Wert fehlt");return;}
  String entered=web.arg("value");
  if(entered.isEmpty() || entered.length()>3) {web.send(400,"text/plain","Wert 0..100");return;}
  for(size_t i=0;i<entered.length();++i) {
    if (entered[i]<'0' || entered[i]>'9') {web.send(400,"text/plain","Ungueltiger Wert");return;}
  }
  unsigned int val=(unsigned int)entered.toInt();
  if(val>100U) {web.send(400,"text/plain","Nur 0..100");return;}
  if((uint8_t)val != motionSensitivity) {
    Preferences cfg;
    if(!cfg.begin("mx30-motion", false)) {web.send(500,"text/plain","NVS nicht verfuegbar");return;}
    size_t stored=cfg.putUChar("level", (uint8_t)val);
    cfg.end();
    if(stored!=1) {web.send(500,"text/plain","Speichern fehlgeschlagen");return;}
    motionSensitivity=(uint8_t)val;
    motionSyncPending=true;
    logEvent("Sensor-Empfindlichkeit gespeichert: " + String(val) + "/100; warte Node");
  }
  JsonDocument response;
  response["ok"]=true;
  response["value"]=motionSensitivity;
  String text;serializeJson(response,text);
  web.sendHeader("Cache-Control","no-store");
  web.send(200,"application/json",text);
}
void handleStatus() {
  JsonDocument json;
  json["controlEnabled"] = controlEnabled;
  json["motionSensitivity"] = motionSensitivity;
  json["nodeSensitivityKnown"] = nodeSensitivityKnown;
  json["nodeAppliedSensitivity"] = nodeAppliedSensitivity;
  json["nodeSensorOK"] = nodeSensorOK;
  json["motionSyncPending"] = motionSyncPending;
  json["canEnable"] = !shellyIp.isEmpty() && nodeContactSinceBoot && WiFi.status() == WL_CONNECTED;
  json["carState"] = !carEverSeen ? "Noch nicht erkannt" :
      carStateRestored ? (carHome ? "Zuhause (unbestaetigt)" : "Weg (vor Neustart)") :
      carHome ? "Zuhause" : "Vermutlich weg";
  json["lastSeenAge"] = !carEverSeen ? "Noch keine Meldung" :
      !nodeContactSinceBoot ? "Seit Neustart noch keine frische Meldung" :
      String((millis()-carLastMs)/1000) + " Sekunden seit letzter Meldung";
  json["baseIp"] = WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString() : (setupApOn?"192.168.4.1":"offline");
  json["ssid"] = wlanSsid;
  json["fixedIp"] = fixedIp;
  json["configIp"] = baseIp.toString();
  json["gateway"] = gatewayIp.toString();
  json["mask"] = subnetIp.toString();
  json["dns"] = dnsIp.toString();
  json["wifi"] = WiFi.status() == WL_CONNECTED;
  json["shellyOnline"] = shellyOnline;
  json["shellyIp"] = shellyIp;
  json["shellyHasMeter"] = shellyHasMeter;
  json["bootCount"] = bootCount;
  json["resetReason"] = resetReasonName();
  json["freeHeap"] = ESP.getFreeHeap();
  json["uptimeSec"] = (uint32_t)((millis()-bootMs)/1000);
  json["reconnectCount"] = wifiReconnectCount;
  json["disconnectCount"] = wifiDisconnectCount;
  json["baseRssi"] = WiFi.status()==WL_CONNECTED ? WiFi.RSSI() : 0;
  json["otaActive"] = otaStarted;
  json["otaRemaining"] = otaStarted && (uint32_t)(millis()-bootMs) < OTA_WINDOW_MS ?
    (OTA_WINDOW_MS - (uint32_t)(millis()-bootMs) + 999) / 1000 : 0;
  json["shellyPollMs"] = lastShellyPollDurationMs;
  json["shellyRequestCount"] = shellyRequestCount;
  json["shellyDecision"] = shellyDecision;
  json["pendingAction"] = pendingAction==ACTION_ON ? "EIN" :
      pendingAction==ACTION_OFF ? "AUS" : "keine";
  json["offSafeSamples"] = offSafeSamples;
  json["offSamplesRequired"] = OFF_SAMPLES_REQUIRED;
  json["actionAttempts"] = actionAttempts;
  json["nextShellyAction"] = pendingAction==ACTION_NONE ? "-" :
      String((int32_t)(millis()-actionDueMs)>=0 ? 0UL :
      ((uint32_t)(actionDueMs-millis())+999)/1000)+" s";
  json["shellyStatusAge"] = shellyStatusSeen ? (uint32_t)(millis()-shellyStatusMs)/1000 : 0;
  json["shellyOutput"] = shellyOutput;
  json["desiredOutput"] = desiredOutput;
  json["motion"] = carMoving;
  json["rxCount"] = rxCount;
  json["rssi"] = carRssi;
  uint32_t elapsed = carEverSeen ? (uint32_t)(millis()-carLastMs)/1000 : 0;
  const uint32_t absenceTimeoutSec = awayTimeoutMs()/1000;
  json["absenceTimeoutSec"] = absenceTimeoutSec;
  json["untilAbsent"] = !carEverSeen ? String("-") : !carHome ? String("bereits als weg eingestuft") :
      String(elapsed >= absenceTimeoutSec ? 0 : absenceTimeoutSec-elapsed) + " s";
  String nodeSignal;
  if (!carEverSeen) nodeSignal = "Noch nie eine Node-Meldung empfangen";
  else if (carStateRestored) nodeSignal = "Letzter Status aus Speicher, warte Node";
  else if (!carHome) nodeSignal = "Node-Timeout erreicht (Abwesenheit vermutet)";
  else if (elapsed > (uint32_t)nextCarSleepSeconds + 30UL)
    nodeSignal = "Erwartete Meldung ueberfaellig";
  else nodeSignal = "Meldung im erwarteten Zeitfenster";
  json["nodeSignal"] = nodeSignal;
  json["baseCheckInterval"] = BASE_PRESENCE_CHECK_MS / 1000;
  uint32_t sinceBaseCheck = presenceCheckMs ? (uint32_t)(millis() - presenceCheckMs) : BASE_PRESENCE_CHECK_MS;
  json["nextBaseCheck"] = String(sinceBaseCheck >= BASE_PRESENCE_CHECK_MS ? 0 :
      (BASE_PRESENCE_CHECK_MS - sinceBaseCheck + 999) / 1000) + " s";
  json["nextExpected"] = !carEverSeen ? String("-") :
      String(elapsed >= nextCarSleepSeconds ? 0 : nextCarSleepSeconds-elapsed) + " s";
  String reason;
  if (!carEverSeen) reason="Warte auf erste Fahrzeugmeldung";
  else if (carStateRestored) reason="Status nach Neustart unbestaetigt";
  else if (carHome) reason="Auto zu Hause: Shelly soll EIN sein";
  else if (pendingAction==ACTION_OFF) reason="Abwesend: einmalige OFF-Pruefung geplant";
  else reason=shellyDecision;
  json["reason"] = reason;
  JsonArray entries = json["events"].to<JsonArray>();
  for (int i = 0; i < logSize; ++i) {
    int idx = (logHead + LOG_MAX - 1 - i) % LOG_MAX;
    entries.add(logLines[idx]);
  }
  String body;
  serializeJson(json, body);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json; charset=utf-8", body);
}

// Grosses Fahrzeugprotokoll separat laden, statt alle 4 Sekunden erneut
// mit dem kleinen Live-Status zu uebertragen.
void handleTrips() {
  JsonDocument json;
  JsonArray trips = json["tripEvents"].to<JsonArray>();
  for (int i = 0; i < tripSize; ++i) {
    int idx = (tripHead + TRIP_LOG_MAX - 1 - i) % TRIP_LOG_MAX;
    trips.add(tripLines[idx]);
  }
  String body;
  serializeJson(json, body);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json; charset=utf-8", body);
}

void handleBoots() {
  JsonDocument json;
  JsonArray a=json["bootEvents"].to<JsonArray>();
  for(int i=0;i<bootSize;++i) {
    const int idx=(bootHead+BOOT_LOG_MAX-1-i)%BOOT_LOG_MAX;
    a.add(bootLines[idx]);
  }
  String body;
  serializeJson(json,body);
  web.sendHeader("Cache-Control","no-store");
  web.send(200,"application/json; charset=utf-8",body);
}

void handleControl() {
  bool wantEnabled = web.arg("enable") == "1";
  if (wantEnabled) {
    if (shellyIp.isEmpty() || !nodeContactSinceBoot || WiFi.status()!=WL_CONNECTED) {
      web.send(409,"text/plain; charset=utf-8",
          "Nicht aktiviert: Shelly-IP, WLAN und frische Fahrzeugmeldung erforderlich.");
      return;
    }
    // Einmalige verbindliche Statusabfrage bei der Freigabe.
    if (!shellyGet()) {
      web.send(409,"text/plain; charset=utf-8","Shelly nicht erreichbar - zuerst Verbindung testen.");
      return;
    }
  }
  if (controlEnabled != wantEnabled) {
    controlEnabled = wantEnabled;
    prefs.putBool("control", controlEnabled);
    logEvent(String("Shelly Control: ") + (controlEnabled ? "AKTIVIERT" : "DEAKTIVIERT"));
    pendingAction=ACTION_NONE;
    offSafeSamples=0;
    if (controlEnabled) scheduleShellyAction(carHome); // Neue bewusste Freigabe ist ein Ereignis.
  }
  web.send(200, "text/plain; charset=utf-8", "OK");
}

void handleRecheckAbsent() {
  if (!controlEnabled || !nodeContactSinceBoot || carStateRestored || carHome ||
      WiFi.status()!=WL_CONNECTED || shellyIp.isEmpty()) {
    web.send(409,"text/plain; charset=utf-8",
             "Erneute OFF-Pruefung nur bei Live-Control, bestätigter Abwesenheit und WLAN moeglich.");
    return;
  }
  scheduleShellyAction(false,false);  // Messreihe beginnt ohne neue 10-Minuten-Frist.
  logEvent("Manuelle OFF-Messreihe (6 Werte) angefordert");
  web.send(200,"text/plain; charset=utf-8","OK");
}

void handleTestShelly() {
  if (shellyIp.isEmpty() || WiFi.status()!=WL_CONNECTED) {
    web.send(409,"text/plain; charset=utf-8","Keine Shelly-IP oder Basis-WLAN offline");return;
  }
  bool success=shellyGet();
  logEvent(success ? "Manueller Shelly-Verbindungstest: OK" :
                     "Manueller Shelly-Verbindungstest: FEHLER");
  web.send(200,"application/json",success ? "{\"ok\":true}" : "{\"ok\":false}");
}

// Separate Shelly-Konfiguration: validierte IPv4; keine Zugangsdaten im Code.
// Bei IP-Wechsel laufende Automatik abschalten, Statusdaten verwerfen und
// die neue Adresse erst wieder auslesen, bevor Live-Control moeglich ist.
void updateShellyIp(const String &normalized) {
  // Erst persistieren, dann Status der alten Shelly-Hardware verwerfen.
  Preferences sp;
  if (!sp.begin("mx30-shelly", false)) {
    web.send(500, "text/plain; charset=utf-8", "NVS nicht verfuegbar");
    return;
  }
  size_t saved=sp.putString("ip", normalized);
  sp.end();
  // putString("", "") liefert 0, auch wenn erfolgreich gespeichert.
  if (!normalized.isEmpty() && saved == 0) {
    web.send(500, "text/plain; charset=utf-8", "Speichern fehlgeschlagen");
    return;
  }
  controlEnabled=false;
  prefs.putBool("control",false);
  shellyIp=normalized;
  shellyOnline=false;
  shellyStatusSeen=false;
  shellyHasMeter=false;
  shellyOutput=false;
  shellyPowerW=0;
  shellyStatusMs=0;
  desiredInitialized=false;
  desiredOutput=false;
  prefs.putBool("desiredValid",false);
  pendingAction=ACTION_NONE;
  actionAttempts=0;
  offSafeSamples=0;
  lastShellyPollDurationMs=0;
  shellyDecision=normalized.isEmpty() ? "Shelly-IP geloescht" : "Shelly-IP geaendert";
  logEvent(normalized.isEmpty() ? "Shelly-IP geloescht; Control AUS" :
    (String("Shelly-IP gespeichert: ")+normalized+"; Control AUS"));
  // Einmalige automatische Verbindungspruefung nur nach Speichern der IP.
  if (!normalized.isEmpty() && WiFi.status()==WL_CONNECTED) {
    bool reachable=shellyGet();
    logEvent(reachable ? "Neue Shelly-IP getestet: OK" :
                         "Neue Shelly-IP: Verbindung fehlgeschlagen");
  }
  JsonDocument json;
  json["ok"]=true;
  json["changed"]=true;
  json["ip"]=shellyIp;
  String body; serializeJson(json,body);
  web.send(200,"application/json; charset=utf-8",body);
}

void handleSaveShelly() {
  String candidate=web.arg("ip");
  candidate.trim();
  IPAddress parsed;
  if(candidate.isEmpty() || candidate.length()>15 || !parsed.fromString(candidate) ||
     parsed[0]==0 || parsed[0]==127 || parsed[0]>=224) {
    web.send(400,"text/plain; charset=utf-8","Ungueltige Shelly-IP");
    return;
  }
  const String normalized=parsed.toString();
  if(normalized==shellyIp) {
    JsonDocument json;
    json["ok"]=true; json["changed"]=false; json["ip"]=shellyIp;
    String body;serializeJson(json,body);
    web.send(200,"application/json; charset=utf-8",body);
    return;
  }
  updateShellyIp(normalized);
}

void handleClearShelly() {
  if (shellyIp.isEmpty()) {
    web.send(200,"application/json", "{\"ok\":true,\"changed\":false,\"ip\":\"\"}");
    return;
  }
  updateShellyIp("");
}

void handleReboot() {
  logEvent("Basis-Neustart manuell angefordert (OTA-Fenster)");
  web.send(200, "text/plain; charset=utf-8", "Basis startet neu");
  restartPending=true;
  restartAt=millis()+1300;
}

void handleSaveWifi() {
  String ssid=web.arg("ssid");ssid.trim();
  String pass=web.arg("pass");
  if(ssid.isEmpty() || ssid.length()>32 || pass.length()>63) {
    web.send(400,"text/plain","Ungueltige SSID / Passwort");return;
  }
  IPAddress newIp,newGw,newMask,newDns;
  if(!newIp.fromString(web.arg("ip")) || !newGw.fromString(web.arg("gw")) ||
     !newMask.fromString(web.arg("mask")) || !newDns.fromString(web.arg("dns"))) {
    web.send(400,"text/plain","IP-Felder enthalten ungueltige Werte");return;
  }
  // Passwort bleibt bei derselben SSID erhalten, falls Eingabe leer ist.
  if(ssid!=wlanSsid || !pass.isEmpty())wlanPassword=pass;
  wlanSsid=ssid;
  Preferences p;p.begin("mx30-wifi",false);
  p.putString("ssid",wlanSsid);
  p.putString("pass",wlanPassword);
  p.putBool("fixed",web.hasArg("fixed"));
  p.putString("ip",newIp.toString());p.putString("gw",newGw.toString());
  p.putString("mask",newMask.toString());p.putString("dns",newDns.toString());
  p.end();
  logEvent("WLAN-Konfiguration geaendert; Neustart folgt");
  web.send(200,"text/html; charset=utf-8","<h2>WLAN gespeichert</h2><p>Basis startet neu. Bei fehlendem WLAN: MX30-Basis-Setup / http://192.168.4.1/</p>");
  restartPending=true;restartAt=millis()+1600;
}

void setup() {
  bootMs=millis();
  Serial.begin(115200);delay(250);
  prefs.begin("mx30-base",false);
  bootCount=prefs.getUInt("boots",0)+1;
  prefs.putUInt("boots",bootCount);
  recoveryArmed=prefs.getBool("recover",false);
  loadLog();
  tripPrefs.begin("mx30-trip", false);
  loadTripLog();
  bootPrefs.begin("mx30-boot", false);
  loadBootLog();
  recordBootEvent();
  // Firmware-Upgrade: Live-Schalten bewusst zurueck in den Testmodus setzen.
  if(prefs.getUInt("configver",0)<210) {
    // Neue Schaltlogik: beim Upgrade sicherheitshalber Testmodus.
    prefs.putBool("control",false);
    prefs.putUInt("configver",210);
  }
  controlEnabled=prefs.getBool("control",false);
  desiredInitialized=prefs.getBool("desiredValid",false);
  desiredOutput=prefs.getBool("desired",false);
  // NVS-Sollwert ist nur Diagnose, niemals Schaltfreigabe ohne frische Node-Meldung.
  restorePresenceAfterRestart();
  loadNetwork();
  loadShellyConfig();
  loadMotionSensitivity();
  loadOtaPassword();
  Serial.printf("\nMX-30 BASIS v2.11.1 | WLAN: %s | Shelly Control: %s\n",
       wlanSsid.isEmpty()?"noch nicht eingerichtet":wlanSsid.c_str(),controlEnabled?"AKTIV":"AUS (Test)");
  setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1);tzset();
  configTime(0,0,"pool.ntp.org");
  WiFi.setHostname("mx30-basis");
  WiFi.mode(WIFI_STA);
  web.on("/",HTTP_GET,[](){web.send_P(200,"text/html; charset=utf-8",DASHBOARD);});
  web.on("/api/health",HTTP_GET,[](){
    web.sendHeader("Cache-Control", "no-store");
    web.send(200,"text/plain; charset=utf-8",String("OK | uptime=")+
      String((millis()-bootMs)/1000)+" s | heap="+String(ESP.getFreeHeap())+
      " | wifi="+String(WiFi.status()==WL_CONNECTED?"online":"offline")+
      " | reset="+resetReasonName());
  });
  web.on("/api/status",HTTP_GET,handleStatus);
  web.on("/api/motion",HTTP_POST,handleSaveMotion);
  web.on("/api/trips",HTTP_GET,handleTrips);
  web.on("/api/boots",HTTP_GET,handleBoots);
  web.on("/api/control",HTTP_POST,handleControl);
  web.on("/api/shelly/save",HTTP_POST,handleSaveShelly);
  web.on("/api/shelly/test",HTTP_POST,handleTestShelly);
  web.on("/api/shelly/recheck",HTTP_POST,handleRecheckAbsent);
  web.on("/api/shelly/clear",HTTP_POST,handleClearShelly);
  web.on("/api/reboot",HTTP_POST,handleReboot);
  web.on("/wifi/save",HTTP_POST,handleSaveWifi);
  web.onNotFound([](){web.send(404,"text/plain","Nicht gefunden");});
  web.begin();
  logEvent(String("Basis gestartet; Shelly Control ")+(controlEnabled?"AKTIV":"AUS"));
  Serial.printf("Konfigurierte Shelly-IP: %s\n", shellyIp.isEmpty()?"keine":shellyIp.c_str());
  Serial.printf("Boot #%lu | Reset: %s | OTA bis +5 Min. | OTA-Passwort: %s\n",
      (unsigned long)bootCount, resetReasonName().c_str(), otaPassword.c_str());
  if(wlanSsid.isEmpty())startAp();
  else {beginSta();wifiAttemptMs=millis();}
}

void loop() {
  // Boot-Eintrag nach Netzzeit-Synchronisierung dauerhaft mit Datum versehen.
  completeBootTimestamp();
  web.handleClient();
  serviceOta();
  if(restartPending && (int32_t)(millis()-restartAt)>=0)ESP.restart();
  ensureWifi();
  receiveUdp();
  // Permanente UDP-Annahme; Anwesenheits-Timeout nur alle 10 Sekunden bewerten.
  if (!presenceCheckMs || (uint32_t)(millis() - presenceCheckMs) >= BASE_PRESENCE_CHECK_MS) {
    presenceCheckMs = millis();
    evaluatePresence();
  }
  // KEINE periodischen Shelly-HTTP-Aufrufe.
  // Einmalige Erreichbarkeitspruefung nach Start, sobald WLAN verbunden ist.
  if (!bootShellyProbeDone && WiFi.status()==WL_CONNECTED) {
    bootShellyProbeDone=true;
    if (!shellyIp.isEmpty()) {
      const bool ok=shellyGet();
      logEvent(ok ? "Shelly beim Start erreichbar" : "Shelly beim Start nicht erreichbar");
    }
  }
  // Das Anwesenheitsereignis legt den einzigen ausstehenden Schaltversuch an.
  processShellyAction();
  delay(5);
}

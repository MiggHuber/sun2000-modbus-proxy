#include <Arduino.h>
#include <WiFi.h>
#include <ModbusIP_ESP8266.h> 
#include "secrets.h"

// ===================== User Config =====================
static const char* ssid = WIFI_SSID;
static const char* pass = WIFI_PASS;

// Huawei WR (Modbus TCP)
static IPAddress huaweiIP(192, 168, 178, 192);
static const uint16_t HUAWEI_PORT = 502;
static const uint8_t  HUAWEI_UNIT = 1; // Falls Timeout bleibt, hier 0 testen

// Proxy Server (ESP32)
static const uint16_t PROXY_PORT = 502;

// Timing Parameter (WICHTIG für Huawei Stabilität)
static const uint32_t POLL_GAP_MS = 2000;      // 3 Sek. Pause zwischen Blöcken
static const uint32_t REQ_TIMEOUT_MS = 10000;  // 10 Sek. warten auf Antwort (Meter/Batterie sind langsam)
static const uint32_t RECONNECT_DELAY_MS = 10000; // 10 Sek. warten nach Disconnect

static const uint16_t HR_SIZE = 220; 

// ===================== Modbus Objects =====================
ModbusIP mbServer;   // Der Proxy (für Home Assistant / Node-RED)
ModbusIP mbClient;   // Der Client (zum Huawei WR)

struct Block {
  uint16_t proxyStart;
  uint16_t huaweiStart;
  uint16_t count;
  bool     writable;
};

// Deine vollständige Register-Tabelle
static Block blocks[] = {
  {  0, 32064, 22, false }, 
  { 30, 32016,  6, false }, 
  { 40, 37119,  6, false }, 
  { 60, 32114,  6, false }, 
  { 70, 32106,  2, false }, 
  { 80, 37001,  2, false }, 
  { 90, 37004,  2, false }, 
  {100, 37066,  4, false }, 
  {110, 37113,  2, false }, 
  {120, 37015,  4, false }, 
 /* {130, 47102,  2, true  }, // Backup SOC
  {140, 47087,  1, true  }, // Charge from AC
  {150, 47242,  2, true  }, // Max Charge Grid
  {160, 37101,  6, false }, // Grid Meter
*/
  {170, 47247,  2, true  }, 
  {180, 47075,  2, true  }
};

static const size_t NUM_BLOCKS = sizeof(blocks) / sizeof(blocks[0]);

// ===================== State Management =====================
static uint32_t lastWifiCheck = 0;
static uint32_t lastConnectAttempt = 0;
static bool requestInFlight = false;
static uint32_t requestStartedAt = 0;
static size_t pollIndex = 0;
static uint32_t lastPollKick = 0;
static bool wasConnected = false;
static uint32_t connectionGraceTimer = 0;

static uint16_t ioBuf[64];
static uint16_t shadowHR[HR_SIZE];

struct WriteJob {
  bool used;
  uint16_t proxyStart;
  uint16_t huaweiStart;
  uint16_t count;
};
static WriteJob writeJobs[8];

// ===================== Helpers =====================
static void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastConnectAttempt < 5000) return;
  lastConnectAttempt = millis();
  Serial.println("[WiFi] Verbinde...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
}

static bool isHuaweiConnected() {
  return mbClient.isConnected(huaweiIP);
}

static void mirrorToServer(uint16_t proxyStart, uint16_t *data, uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    if (proxyStart + i < HR_SIZE) mbServer.Hreg(proxyStart + i, data[i]);
  }
}

static void updateShadowFromServer(uint16_t proxyStart, uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    if (proxyStart + i < HR_SIZE) shadowHR[proxyStart + i] = mbServer.Hreg(proxyStart + i);
  }
}

static bool enqueueWriteJob(uint16_t proxyStart, uint16_t huaweiStart, uint16_t count) {
  for (auto &j : writeJobs) {
    if (j.used && j.proxyStart == proxyStart) return false;
  }
  for (auto &j : writeJobs) {
    if (!j.used) {
      j.used = true;
      j.proxyStart = proxyStart;
      j.huaweiStart = huaweiStart;
      j.count = count;
      return true;
    }
  }
  return false;
}

static bool popWriteJob(WriteJob &out) {
  for (auto &j : writeJobs) {
    if (j.used) {
      out = j;
      j.used = false;
      return true;
    }
  }
  return false;
}

static void detectWritesAndQueue() {
  for (size_t i = 0; i < NUM_BLOCKS; i++) {
    if (!blocks[i].writable) continue;
    for (uint16_t k = 0; k < blocks[i].count; k++) {
      uint16_t addr = blocks[i].proxyStart + k;
      if (addr < HR_SIZE && mbServer.Hreg(addr) != shadowHR[addr]) {
        if (enqueueWriteJob(blocks[i].proxyStart, blocks[i].huaweiStart, blocks[i].count)) {
          updateShadowFromServer(blocks[i].proxyStart, blocks[i].count);
        }
      }
    }
  }
}

// ===================== Modbus Transactions =====================
static void startReadBlock(const Block &b) {
  if (requestInFlight || !isHuaweiConnected()) return;

  requestInFlight = true;
  requestStartedAt = millis();

  mbClient.readHreg(huaweiIP, b.huaweiStart, ioBuf, b.count, [b](Modbus::ResultCode event, uint16_t, void*) -> bool {
    requestInFlight = false;
    
    if (event == Modbus::EX_SUCCESS) {
      // Alles okay, Daten spiegeln
      Serial.printf("[RD] OK    H:%u\n", b.huaweiStart);
      mirrorToServer(b.proxyStart, ioBuf, b.count);
      lastPollKick = millis(); 
    } 
    // --- HIER KOMMT DER BLOCK REIN ---
    else if ((int)event == 6) { 
      Serial.printf("[RD] BUSY  H:%u (Warte 4s...)\n", b.huaweiStart);
      // Wir geben dem Dongle 4 Sekunden echte Ruhepause im loop()
      lastPollKick = millis() + 4000; 
    }
    else if ((int)event == 228) { 
      // Timeout (meistens Batterie-Suche)
      Serial.printf("[RD] WAIT  H:%u (Timeout - bleibe verbunden)\n", b.huaweiStart);
      lastPollKick = millis() + 3000; 
    }
    else {
      // Alle anderen Fehler
      Serial.printf("[RD] SKIP  H:%u (Fehler %d)\n", b.huaweiStart, (int)event);
      lastPollKick = millis() + 5000;
    }
    return true;
  }, HUAWEI_UNIT);
}


static void startWriteJob(const WriteJob &j) {
  if (requestInFlight) return;
  requestInFlight = true;
  requestStartedAt = millis();
  for (uint16_t i = 0; i < j.count; i++) ioBuf[i] = mbServer.Hreg(j.proxyStart + i);
  
  mbClient.writeHreg(huaweiIP, j.huaweiStart, ioBuf, j.count, [j](Modbus::ResultCode event, uint16_t, void*) -> bool {
      requestInFlight = false;
      if (event == Modbus::EX_SUCCESS) {
        Serial.printf("[WR] ERFOLG H:%u\n", j.huaweiStart);
        lastPollKick = millis() + 10000; // 10s Ruhe nach Schreibvorgang
      } else {
        Serial.printf("[WR] FEHLER: %d H:%u. Disconnect.\n", (int)event, j.huaweiStart);
        mbClient.disconnect(huaweiIP);
        lastPollKick = millis() + RECONNECT_DELAY_MS;
      }
      return true;
  }, HUAWEI_UNIT);
}

// ===================== Arduino Core =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== SUN2000 Modbus Proxy (ESP32-C3) ===");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  mbServer.server(PROXY_PORT);
  mbServer.begin();
  mbServer.addHreg(0, 0, HR_SIZE);
  for (uint16_t i = 0; i < HR_SIZE; i++) shadowHR[i] = 0;
}

void loop() {
  // 1. Modbus Tasks (WICHTIG: Immer zuerst!)
  mbServer.task();
  mbClient.task();

  // 2. WiFi & Cloud-Keepalive
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    wifiEnsureConnected();
  }

  // 3. Huawei Verbindungshandling (Ohne harten Reset bei Fehlern)
  bool connected = isHuaweiConnected();

  if (!connected) {
    wasConnected = false;
    // Wenn nicht verbunden, alle 10s neu versuchen
    if (millis() - lastConnectAttempt > 10000) { 
      lastConnectAttempt = millis();
      Serial.println("[Huawei] TCP Connect Versuch...");
      mbClient.connect(huaweiIP, HUAWEI_PORT);
    }
    return; // Solange TCP nicht steht, bricht der Loop hier ab
  }

  // 4. Gnadenfrist nach erfolgreichem TCP-Connect
  if (!wasConnected) {
    wasConnected = true;
    connectionGraceTimer = millis();
    Serial.println("[Huawei] TCP OK! Warte 5s Gnadenfrist für Dongle...");
  }

  // Während der Gnadenfrist (5s) nichts an den Dongle senden
  if (millis() - connectionGraceTimer < 5000) return;

  // 5. "Sanfte" Timeout-Recovery (Falls ein Request stecken bleibt)
  // Wir setzen nur den Status zurück, ohne die TCP-Leitung zu kappen (Crash-Schutz!)
  if (requestInFlight && (millis() - requestStartedAt > REQ_TIMEOUT_MS)) {
    Serial.println("[Modbus] Request hakt... überspringe Block (Kein Disconnect)");
    requestInFlight = false; 
    lastPollKick = millis() + 5000; // 5s Abkühlpause für den Dongle
    return;
  }

  // 6. Polling & Schreibvorgänge (Nur wenn kein Request läuft und Pause um ist)
  if (!requestInFlight && (millis() - lastPollKick > POLL_GAP_MS)) {
    
    WriteJob wj;
    // Haben wir einen Schreibauftrag (z.B. vom Proxy-Client)?
    if (popWriteJob(wj)) {
      startWriteJob(wj);
    } 
    // Ansonsten: Nächsten Lese-Block aus der Tabelle abfragen
    else {
      startReadBlock(blocks[pollIndex]);
      
      // Index für den nächsten Durchlauf erhöhen
      pollIndex++;
      if (pollIndex >= NUM_BLOCKS) {
        pollIndex = 0;
      }
      
      // Timer für die Pause zwischen den Blöcken (POLL_GAP_MS) setzen
      lastPollKick = millis();
    }
  }

  // 7. Proxy-Server auf Änderungen prüfen (für Schreibbefehle)
  detectWritesAndQueue();

  // 8. Kleiner Delay für den C3 Stack (CPU entlasten)
  delay(1);
}

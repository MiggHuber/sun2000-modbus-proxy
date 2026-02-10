#include <Arduino.h>
#include <WiFi.h>
#include <ModbusTCP.h>
#include "secrets.h"

static const char* ssid = WIFI_SSID;
static const char* pass = WIFI_PASS;


// -------- Huawei --------
IPAddress huaweiIP(192,168,178,192);
bool huaweiConnected = false;

const uint16_t HUAWEI_PORT = 502;
const uint8_t  HUAWEI_UNIT = 1;

// -------- Modbus --------
ModbusTCP mbServer;   // für Modbus Poll / HA
ModbusTCP mbClient;   // für Huawei

uint32_t lastPoll = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("BOOT OK");

  // ---- WLAN ----
  WiFi.begin(ssid, pass);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK");
  Serial.println(WiFi.localIP());

  // ---- Modbus Server ----
  mbServer.server();
  mbServer.addHreg(0, 0);   // Spiegelregister (Low-Word)
  mbServer.addHreg(1, 0);   // Spiegelregister (High-Word)

  // ---- Modbus Client ----
  mbClient.client();
  mbClient.connect(huaweiIP, HUAWEI_PORT);
}

uint16_t regs[2];

void loop() {
  mbServer.task();
  mbClient.task();

  if (!mbClient.isConnected(huaweiIP)) {
    Serial.println("Connecting to Huawei...");
    if (!mbClient.connect(huaweiIP, HUAWEI_PORT)) {
      Serial.println("Connection failed");
      delay(3000);
      return;
    }
    Serial.println("Huawei connected");
  }

  if (millis() - lastPoll > 5000) {
    lastPoll = millis();

    mbClient.readHreg(
      huaweiIP,
      32080,
      regs,
      2,
      [](Modbus::ResultCode event, uint16_t, void*) -> bool {
        if (event == Modbus::EX_SUCCESS) {
          int32_t power = ((int32_t)regs[0] << 16) | regs[1];
          Serial.print("Huawei Power [W]: ");
          Serial.println(power);
        } else {
          Serial.print("Read error: ");
          Serial.println((int)event);
        }
        return true;
      },
      HUAWEI_UNIT
    );
  }
}

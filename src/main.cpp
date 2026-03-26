#include <Arduino.h>
#include <ArduinoJson.h>

#include "lora20_device.hpp"
#include "serial_rpc.hpp"

lora20::DeviceStateStore g_state;
lora20::SerialRpcServer g_rpc(Serial, g_state);
bool g_ready = false;

void setup() {
  Serial.begin(115200);

  const unsigned long waitStarted = millis();
  while (!Serial && (millis() - waitStarted) < 4000) {
    delay(10);
  }

  String error;
  if (!g_state.begin(error)) {
    DynamicJsonDocument fatalDoc(256);
    fatalDoc["type"] = "fatal";
    fatalDoc["message"] = error;
    serializeJson(fatalDoc, Serial);
    Serial.println();
    g_ready = false;
    return;
  }

  g_rpc.begin();
  g_ready = true;
}

void loop() {
  if (!g_ready) {
    delay(250);
    return;
  }

  g_rpc.poll();
  delay(2);
}

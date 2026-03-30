#pragma once

#include <Arduino.h>

#include "lora20_device.hpp"
#include "lorawan_client.hpp"

namespace lora20 {

class SerialRpcServer {
 public:
  SerialRpcServer(Stream &serial, DeviceStateStore &state, LoRaWanClient &lorawan);

  void begin();
  void poll();

 private:
  void handleLine(const String &line);
  void sendBootEvent();

  Stream &serial_;
  DeviceStateStore &state_;
  LoRaWanClient &lorawan_;
  String buffer_;
};

}  // namespace lora20

#pragma once

#include <Arduino.h>

#include "lora20_device.hpp"

namespace lora20 {

class SerialRpcServer {
 public:
  SerialRpcServer(Stream &serial, DeviceStateStore &state);

  void begin();
  void poll();

 private:
  void handleLine(const String &line);
  void sendBootEvent();

  Stream &serial_;
  DeviceStateStore &state_;
  String buffer_;
};

}  // namespace lora20

#pragma once

#include <Arduino.h>

#include "boot_control.hpp"
#include "connectivity_manager.hpp"
#include "lora20_device.hpp"
#include "lorawan_client.hpp"

namespace lora20 {

class SerialRpcServer {
 public:
  SerialRpcServer(Stream &serial,
                  DeviceStateStore &state,
                  LoRaWanClient &lorawan,
                  BootControl &boot,
                  ConnectivityManager &connectivity);

  void begin();
  void poll();
  bool processRequestLine(const String &line, String &response, bool &rebootRequested, String &error);

 private:
  void handleLine(const String &line);
  void sendBootEvent();

  Stream &serial_;
  DeviceStateStore &state_;
  LoRaWanClient &lorawan_;
  BootControl &boot_;
  ConnectivityManager &connectivity_;
  String buffer_;
};

}  // namespace lora20

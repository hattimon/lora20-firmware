#pragma once

#include <Arduino.h>

#include "lora20_device.hpp"
#include "lorawan_client.hpp"

namespace lora20 {

class WifiBridge;

enum class RpcTransport {
  kUnknown = 0,
  kSerial,
  kBle,
  kWifi
};

class RpcProcessor {
 public:
  RpcProcessor(DeviceStateStore &state, LoRaWanClient &lorawan);
  void setWifiBridge(WifiBridge *bridge);

  // Returns true when a response should be sent.
  bool handleLine(const String &line,
                  String &response,
                  bool requireAuth = false,
                  RpcTransport transport = RpcTransport::kUnknown);
  void buildBootEvent(String &response);

 private:
  DeviceStateStore &state_;
  LoRaWanClient &lorawan_;
  WifiBridge *wifiBridge_ = nullptr;
};

}  // namespace lora20

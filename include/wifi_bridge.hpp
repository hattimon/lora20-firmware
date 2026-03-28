#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "rpc_processor.hpp"

namespace lora20 {

class WifiBridge {
 public:
  explicit WifiBridge(RpcProcessor &processor);

  void begin();
  void poll();

 private:
  void handleRpc();
  void handleOptions();
  void handleHealth();
  void sendJson(int code, const String &payload);
  void sendCorsHeaders();

  RpcProcessor &processor_;
  WebServer server_;
  String ssid_;
};

}  // namespace lora20

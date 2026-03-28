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
  void applyConfig(const ConnectionConfig &config);

  String ipAddress() const;
  String modeLabel() const;

 private:
  enum class Mode {
    kOff,
    kAp,
    kStaConnecting,
    kStaConnected
  };

  void startAp();
  void startSta();
  void stopWifi();
  void ensureServer();
  void updateConnectionState();
  void handleRpc();
  void handleOptions();
  void handleHealth();
  void sendJson(int code, const String &payload);
  void sendCorsHeaders();

  RpcProcessor &processor_;
  WebServer server_;
  bool enabled_ = false;
  bool serverStarted_ = false;
  bool wantsSta_ = false;
  unsigned long connectStartedMs_ = 0;
  Mode mode_ = Mode::kOff;
  String apSsid_;
  String staSsid_;
  String staPass_;
};

}  // namespace lora20

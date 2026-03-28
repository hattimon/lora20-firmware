#pragma once

#include <Arduino.h>

#include "rpc_processor.hpp"

namespace lora20 {

class SerialRpcServer {
 public:
  SerialRpcServer(Stream &serial, RpcProcessor &processor);

  void begin();
  void poll();
  unsigned long lastActivityMs() const;

 private:
  void handleLine(const String &line);
  void sendBootEvent();

  Stream &serial_;
  RpcProcessor &processor_;
  String buffer_;
  unsigned long lastActivityMs_ = 0;
};

}  // namespace lora20

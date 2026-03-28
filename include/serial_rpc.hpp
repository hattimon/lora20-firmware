#pragma once

#include <Arduino.h>

#include "rpc_processor.hpp"

namespace lora20 {

class SerialRpcServer {
 public:
  SerialRpcServer(Stream &serial, RpcProcessor &processor);

  void begin();
  void poll();

 private:
  void handleLine(const String &line);
  void sendBootEvent();

  Stream &serial_;
  RpcProcessor &processor_;
  String buffer_;
};

}  // namespace lora20

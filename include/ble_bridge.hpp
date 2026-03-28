#pragma once

#include <Arduino.h>
#include <BLEDevice.h>

#include "rpc_processor.hpp"

namespace lora20 {

class BleBridge {
 public:
  explicit BleBridge(RpcProcessor &processor);

  void begin();
  void poll();
  bool connected() const;
  void setEnabled(bool enabled);

 private:
  friend class BleServerCallbacks;
  void handleRxChunk(const std::string &value);
  void sendLine(const String &line);
  void sendChunk(const uint8_t *data, size_t length);

  RpcProcessor &processor_;
  BLEServer *server_;
  BLECharacteristic *tx_;
  BLECharacteristic *rx_;
  bool connected_;
  bool enabled_;
  bool initialized_;
  String rxBuffer_;
};

}  // namespace lora20

#include "ble_bridge.hpp"

#include <BLE2902.h>

constexpr const char *kServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *kRxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *kTxUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr size_t kChunkSize = 20;

namespace lora20 {

class BleServerCallbacks : public BLEServerCallbacks {
 public:
  explicit BleServerCallbacks(BleBridge &bridge) : bridge_(bridge) {}

  void onConnect(BLEServer *) override {
    bridge_.connected_ = true;
    bridge_.lastActivityMs_ = millis();
  }

  void onDisconnect(BLEServer *server) override {
    bridge_.connected_ = false;
    bridge_.lastActivityMs_ = millis();
    if (bridge_.enabled_) {
      server->startAdvertising();
    }
  }

 private:
  BleBridge &bridge_;
};

class BleRxCallbacks : public BLECharacteristicCallbacks {
 public:
  explicit BleRxCallbacks(BleBridge &bridge) : bridge_(bridge) {}

  void onWrite(BLECharacteristic *characteristic) override {
    const std::string value = characteristic->getValue();
    if (!value.empty()) {
      bridge_.lastActivityMs_ = millis();
      bridge_.handleRxChunk(value);
    }
  }

 private:
  BleBridge &bridge_;
};

String buildBleName() {
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08llx", static_cast<unsigned long long>(chipId & 0xFFFFFFFFULL));
  String name = String("LORA20-") + String(suffix).substring(4);
  return name;
}

BleBridge::BleBridge(RpcProcessor &processor)
    : processor_(processor),
      server_(nullptr),
      tx_(nullptr),
      rx_(nullptr),
      connected_(false),
      enabled_(false),
      initialized_(false),
      lastActivityMs_(0),
      rxBuffer_("") {}

void BleBridge::begin() {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  BLEDevice::init(buildBleName().c_str());
  server_ = BLEDevice::createServer();
  server_->setCallbacks(new BleServerCallbacks(*this));

  BLEService *service = server_->createService(kServiceUuid);
  tx_ = service->createCharacteristic(kTxUuid, BLECharacteristic::PROPERTY_NOTIFY);
  tx_->addDescriptor(new BLE2902());

  rx_ = service->createCharacteristic(kRxUuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx_->setCallbacks(new BleRxCallbacks(*this));

  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  if (enabled_) {
    advertising->start();
  }
}

void BleBridge::poll() {
  // BLE callbacks are event-driven.
}

bool BleBridge::connected() const {
  return connected_;
}

bool BleBridge::isEnabled() const {
  return enabled_;
}

unsigned long BleBridge::lastActivityMs() const {
  return lastActivityMs_;
}

void BleBridge::setEnabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }
  enabled_ = enabled;
  if (!initialized_ && enabled_) {
    begin();
  }
  if (!initialized_) {
    return;
  }
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (enabled_) {
    if (advertising) {
      advertising->start();
    }
  } else {
    if (advertising) {
      advertising->stop();
    }
    connected_ = false;
  }
}

void BleBridge::handleRxChunk(const std::string &value) {
  if (!enabled_) {
    return;
  }
  rxBuffer_ += String(value.c_str());

  int newlineIndex = rxBuffer_.indexOf('\n');
  while (newlineIndex >= 0) {
    String line = rxBuffer_.substring(0, newlineIndex);
    line.replace("\r", "");
    rxBuffer_ = rxBuffer_.substring(newlineIndex + 1);

    String response;
    if (processor_.handleLine(line, response, true)) {
      sendLine(response + "\n");
    }

    newlineIndex = rxBuffer_.indexOf('\n');
  }
}

void BleBridge::sendLine(const String &line) {
  if (!tx_) return;
  const size_t length = line.length();
  const uint8_t *data = reinterpret_cast<const uint8_t *>(line.c_str());
  size_t offset = 0;
  while (offset < length) {
    const size_t chunk = length - offset > kChunkSize ? kChunkSize : (length - offset);
    sendChunk(data + offset, chunk);
    offset += chunk;
    delay(5);
  }
}

void BleBridge::sendChunk(const uint8_t *data, size_t length) {
  if (!tx_) return;
  tx_->setValue(const_cast<uint8_t *>(data), length);
  tx_->notify();
}

}  // namespace lora20

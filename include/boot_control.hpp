#pragma once

#include <Arduino.h>

#include <array>

namespace lora20 {

constexpr size_t kBootSlotCount = 3;

struct BootSlotInfo {
  String protocol;
  String partitionLabel;
  String subtype;
  String projectName;
  String version;
  uint32_t address = 0;
  uint32_t sizeBytes = 0;
  bool partitionPresent = false;
  bool validImage = false;
  bool running = false;
  bool bootTarget = false;
};

struct BootControlStatus {
  bool supported = false;
  String currentProtocol;
  String bootProtocol;
  String runningPartitionLabel;
  String bootPartitionLabel;
  String buttonHint;
  std::array<BootSlotInfo, kBootSlotCount> slots{};
};

class BootControl {
 public:
  explicit BootControl(Stream &serial);

  bool begin(String &error);
  void poll();

  const BootControlStatus &status();
  bool switchToProtocol(const String &protocol, bool rebootNow, String &error);

 private:
  void refreshStatus();
  void emitButtonHint(const String &protocol, unsigned long holdMs);
  void emitSwitchEvent(const char *stage,
                       const String &protocol,
                       const String &message,
                       bool rebootNow);

  Stream &serial_;
  BootControlStatus status_;
  bool statusDirty_ = true;
  bool buttonPressed_ = false;
  unsigned long buttonDownSinceMs_ = 0;
  uint8_t buttonHintStage_ = 0;
  bool switchArmActive_ = false;
  String armedProtocol_;
  bool ignoreUntilRelease_ = false;
};

}  // namespace lora20

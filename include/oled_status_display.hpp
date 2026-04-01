#pragma once

#include <Arduino.h>

#include "boot_control.hpp"
#include "lorawan_client.hpp"

namespace lora20 {

class OledStatusDisplay {
 public:
  explicit OledStatusDisplay(Stream &serial);

  bool begin(const BootControlStatus &bootStatus, const LoRaWanRuntimeStatus &lorawanStatus, String &error);
  void poll(const BootControlStatus &bootStatus, const LoRaWanRuntimeStatus &lorawanStatus);
  void selfTest();

 private:
  void applyBrightnessProfile();
  void trackLoRaActivity(const LoRaWanRuntimeStatus &lorawanStatus, unsigned long nowMs);
  void renderIdleScreen(const BootControlStatus &bootStatus, const LoRaWanRuntimeStatus &lorawanStatus);
  String protocolLabel(const String &protocol) const;
  String joinStateLabel(const LoRaWanRuntimeStatus &lorawanStatus) const;
  String regionLabel(const LoRaWanRuntimeStatus &lorawanStatus) const;

  Stream &serial_;
  bool available_ = false;
  bool idleScreenVisible_ = false;
  bool lastSeenJoined_ = false;
  String lastSeenEvent_;
  uint32_t lastSeenAcceptedSendMs_ = 0;
  unsigned long lastLoRaActivityMs_ = 0;
  String lastRenderedProtocol_;
  String lastRenderedRegion_;
  String lastRenderedJoinState_;
};

}  // namespace lora20

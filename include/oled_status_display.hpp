#pragma once

#include <Arduino.h>

#include "boot_control.hpp"
#include "connectivity_manager.hpp"
#include "lorawan_client.hpp"

namespace lora20 {

class OledStatusDisplay {
 public:
  explicit OledStatusDisplay(Stream &serial);

  bool begin(const BootControlStatus &bootStatus,
             const LoRaWanRuntimeStatus &lorawanStatus,
             const ConnectivityRuntimeStatus &connectivityStatus,
             String &error);
  void poll(const BootControlStatus &bootStatus,
            const LoRaWanRuntimeStatus &lorawanStatus,
            const ConnectivityRuntimeStatus &connectivityStatus);
  void selfTest();

 private:
  void applyBrightnessProfile();
  void sleepDisplay();
  void wakeDisplay();
  void recordActivity(unsigned long nowMs);
  void trackWakeButton(unsigned long nowMs);
  void trackLoRaActivity(const LoRaWanRuntimeStatus &lorawanStatus, unsigned long nowMs);
  void trackConnectivityActivity(const ConnectivityRuntimeStatus &connectivityStatus, unsigned long nowMs);
  void renderScreen(const BootControlStatus &bootStatus,
                    const LoRaWanRuntimeStatus &lorawanStatus,
                    const ConnectivityRuntimeStatus &connectivityStatus,
                    unsigned long nowMs);
  void drawWrappedLine(int16_t x, int16_t y, int16_t maxWidth, const String &text);
  String protocolLabel(const String &protocol) const;
  String joinStateLabel(const LoRaWanRuntimeStatus &lorawanStatus) const;
  String regionLabel(const LoRaWanRuntimeStatus &lorawanStatus) const;
  String connectionLabel(const ConnectivityRuntimeStatus &connectivityStatus) const;
  String batteryLabel(const ConnectivityRuntimeStatus &connectivityStatus) const;
  String clockLabel(unsigned long nowMs, const ConnectivityRuntimeStatus &connectivityStatus) const;

  Stream &serial_;
  bool available_ = false;
  bool sleeping_ = false;
  bool lastPrgPressed_ = false;
  bool lastSeenJoined_ = false;
  String lastSeenEvent_;
  uint32_t lastSeenAcceptedSendMs_ = 0;
  uint32_t lastSeenConnectivityActivity_ = 0;
  unsigned long lastLoRaActivityMs_ = 0;
  unsigned long lastActivityMs_ = 0;
  String lastRenderedSignature_;
};

}  // namespace lora20

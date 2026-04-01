#pragma once

#include <Arduino.h>

#include <array>

#include "boot_control.hpp"
#include "connectivity_manager.hpp"
#include "lora20_device.hpp"
#include "lorawan_client.hpp"

namespace lora20 {

struct AutoMintRuntimeStatus {
  bool enabled = false;
  bool queueMode = false;
  bool singleMode = false;
  size_t totalProfiles = 0;
  size_t enabledProfiles = 0;
  uint32_t intervalSeconds = 0;
  String defaultTick;
  String defaultAmount;
  String lastError;
  std::array<String, 3> recentPackets{};
};

class OledStatusDisplay {
 public:
  OledStatusDisplay(Stream &serial, DeviceStateStore &state, ConnectivityManager &connectivity);

  bool begin(const BootControlStatus &bootStatus,
             const LoRaWanRuntimeStatus &lorawanStatus,
             const ConnectivityRuntimeStatus &connectivityStatus,
             const AutoMintRuntimeStatus &autoMintStatus,
             String &error);
  void poll(const BootControlStatus &bootStatus,
            const LoRaWanRuntimeStatus &lorawanStatus,
            const ConnectivityRuntimeStatus &connectivityStatus,
            const AutoMintRuntimeStatus &autoMintStatus);
  void selfTest();

 private:
  enum class MenuItemType : uint8_t {
    MainStatus,
    LinksStatus,
    ToggleBluetooth,
    ToggleWifi,
    CycleSleep,
    AutomationStatus,
    ToggleAutomation,
    CycleInterval,
    ToggleProfile,
    RecentAutoMints,
    ExitMenu,
  };

  struct MenuItem {
    MenuItemType type = MenuItemType::MainStatus;
    int profileIndex = -1;
  };

  static constexpr size_t kMaxMenuItems = 20;

  void applyBrightnessProfile();
  void sleepDisplay();
  void wakeDisplay();
  void recordActivity(unsigned long nowMs);
  void handleMenuButton(unsigned long nowMs, const AutoMintRuntimeStatus &autoMintStatus);
  void trackLoRaActivity(const LoRaWanRuntimeStatus &lorawanStatus, unsigned long nowMs);
  void trackConnectivityActivity(const ConnectivityRuntimeStatus &connectivityStatus, unsigned long nowMs);
  size_t buildMenuItems(const DeviceSnapshot &snapshot, std::array<MenuItem, kMaxMenuItems> &items) const;
  void handleShortPress(const DeviceSnapshot &snapshot);
  void handleLongPress(const DeviceSnapshot &snapshot, const AutoMintRuntimeStatus &autoMintStatus);
  bool applyConnectivityChange(ConnectivityConfig &config, const String &successLabel);
  bool applyDeviceConfigChange(DeviceConfig &config, const String &successLabel);
  void logUiEvent(const char *stage, const String &message);
  uint32_t nextSleepPreset(uint32_t current) const;
  uint32_t nextIntervalPreset(uint32_t current) const;
  String intervalLabel(uint32_t seconds) const;
  void drawTopBar(const ConnectivityRuntimeStatus &connectivityStatus, unsigned long nowMs);
  void drawConnectionIcons(const ConnectivityRuntimeStatus &connectivityStatus);
  void drawUsbIcon(int16_t x, int16_t y, bool active);
  void drawBleIcon(int16_t x, int16_t y, bool active);
  void drawWifiIcon(int16_t x, int16_t y, bool active);
  void drawInactiveSlash(int16_t x, int16_t y, int16_t width, int16_t height);
  void drawBatteryStatus(const ConnectivityRuntimeStatus &connectivityStatus);
  void renderScreen(const BootControlStatus &bootStatus,
                    const LoRaWanRuntimeStatus &lorawanStatus,
                    const ConnectivityRuntimeStatus &connectivityStatus,
                    const AutoMintRuntimeStatus &autoMintStatus,
                    unsigned long nowMs);
  void drawWrappedLine(int16_t x, int16_t y, int16_t maxWidth, const String &text);
  String protocolLabel(const String &protocol) const;
  String joinStateLabel(const LoRaWanRuntimeStatus &lorawanStatus) const;
  String regionLabel(const LoRaWanRuntimeStatus &lorawanStatus) const;
  String clockLabel(unsigned long nowMs, const ConnectivityRuntimeStatus &connectivityStatus) const;
  String menuItemTitle(const MenuItem &item,
                       const DeviceSnapshot &snapshot,
                       const AutoMintRuntimeStatus &autoMintStatus) const;
  String menuItemLine1(const MenuItem &item,
                       const BootControlStatus &bootStatus,
                       const LoRaWanRuntimeStatus &lorawanStatus,
                       const ConnectivityRuntimeStatus &connectivityStatus,
                       const DeviceSnapshot &snapshot,
                       const AutoMintRuntimeStatus &autoMintStatus) const;
  String menuItemLine2(const MenuItem &item,
                       const LoRaWanRuntimeStatus &lorawanStatus,
                       const ConnectivityRuntimeStatus &connectivityStatus,
                       const DeviceSnapshot &snapshot,
                       const AutoMintRuntimeStatus &autoMintStatus) const;
  String menuItemLine3(const MenuItem &item,
                       const ConnectivityRuntimeStatus &connectivityStatus,
                       const DeviceSnapshot &snapshot,
                       const AutoMintRuntimeStatus &autoMintStatus) const;
  String menuItemFooter(const MenuItem &item) const;

  Stream &serial_;
  DeviceStateStore &state_;
  ConnectivityManager &connectivity_;
  bool available_ = false;
  bool sleeping_ = false;
  bool buttonPressed_ = false;
  bool ignoreReleaseAfterWake_ = false;
  unsigned long buttonDownSinceMs_ = 0;
  bool lastSeenJoined_ = false;
  String lastSeenEvent_;
  uint32_t lastSeenAcceptedSendMs_ = 0;
  uint32_t lastSeenConnectivityActivity_ = 0;
  unsigned long lastActivityMs_ = 0;
  size_t selectedMenuIndex_ = 0;
  String lastRenderedSignature_;
};

}  // namespace lora20

#pragma once

#include <Arduino.h>

#include "lora20_device.hpp"

namespace lora20 {

class SerialRpcServer;

struct BluetoothRuntimeStatus {
  bool enabled = false;
  bool available = false;
  bool connected = false;
  bool pairing = false;
  uint32_t pin = 0;
  String deviceName;
  String state = "disabled";
  String lastError;
};

struct WifiRuntimeStatus {
  bool enabled = false;
  bool configured = false;
  bool connected = false;
  bool connecting = false;
  bool dhcpReady = false;
  bool apFallback = false;
  int32_t rssi = 0;
  String state = "disabled";
  String ssid;
  String ipAddress;
  String hostname;
  String lastError;
};

struct BatteryRuntimeStatus {
  bool available = true;
  uint16_t millivolts = 0;
  uint8_t percent = 0;
};

struct ConnectivityRuntimeStatus {
  String activeInterface = "none";
  String preferredInterface = "auto";
  BluetoothRuntimeStatus bluetooth;
  WifiRuntimeStatus wifi;
  BatteryRuntimeStatus battery;
  bool tokenSet = false;
  bool wifiConfigured = false;
  bool usbConnected = false;
  uint32_t displaySleepSeconds = 0;
  uint32_t bridgeWindowSeconds = 300;
  uint32_t activityCounter = 0;
  uint8_t powerSaveLevel = 1;
  int16_t utcOffsetMinutes = 0;
  String lastError;
};

class ConnectivityManager {
 public:
  explicit ConnectivityManager(DeviceStateStore &state);

  void attachRpcServer(SerialRpcServer &rpc);
  bool begin(String &error);
  void poll();

  const ConnectivityRuntimeStatus &status() const;
  const ConnectivityConfig &config() const;

 bool updateConfig(const ConnectivityConfig &config, String &error);

 private:
  bool ensureConfigDefaults(String &error);
  bool applyConfig(bool fromBoot, String &error);
  void refreshRuntime();
  void refreshBattery();
  void refreshUsbState();
  void refreshWifiState(unsigned long nowMs);
  void refreshBluetoothState(unsigned long nowMs);
  bool startBluetooth(String &error);
  void stopBluetooth();
  bool startWifi(String &error);
  void stopWifi();
  void configureWifiPowerSave();
  void handleWifiHttp();
  bool processRpcPayload(const String &payload, String &response, bool &rebootRequested, String &error);
  void scheduleRestart(unsigned long delayMs);
  void maybeRestart(unsigned long nowMs);
  void markActivity();
  void setWifiErrorState(const String &state, const String &error);
  void clearWifiErrorState();
  void setBluetoothErrorState(const String &state, const String &error);
  void clearBluetoothErrorState();
  String resolveBluetoothName() const;
  String resolveWifiHostname() const;
  uint32_t resolveBluetoothPin() const;
  String chipSuffix() const;

  DeviceStateStore &state_;
  SerialRpcServer *rpc_ = nullptr;
  ConnectivityRuntimeStatus status_;
  bool bleStarted_ = false;
  bool wifiServerStarted_ = false;
  bool wifiConnectRequested_ = false;
  bool wifiWasConnected_ = false;
  unsigned long wifiConnectStartedMs_ = 0;
  unsigned long nextWifiRetryMs_ = 0;
  unsigned long nextBatterySampleMs_ = 0;
  unsigned long scheduledRestartAtMs_ = 0;
};

}  // namespace lora20

#include "connectivity_manager.hpp"

#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <time.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "serial_rpc.hpp"

namespace {

constexpr char kBleServiceUuid[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kBleRxUuid[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kBleTxUuid[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr unsigned long kBleAdvertRestartDelayMs = 1000UL;
constexpr unsigned long kWifiConnectTimeoutMs = 20000UL;
constexpr unsigned long kWifiDhcpTimeoutMs = 12000UL;
constexpr unsigned long kWifiRetryDelayMs = 5000UL;
constexpr unsigned long kBatterySamplePeriodMs = 15000UL;

lora20::ConnectivityManager *g_activeConnectivity = nullptr;
BLEServer *g_bleServer = nullptr;
BLEService *g_bleService = nullptr;
BLECharacteristic *g_bleTxCharacteristic = nullptr;
BLECharacteristic *g_bleRxCharacteristic = nullptr;
bool g_bleDeviceConnected = false;
bool g_bleAuthenticated = false;
bool g_blePairing = false;
bool g_bleIncomingActivity = false;
uint16_t g_bleLastConnId = 0;
unsigned long g_bleAdvertRestartAtMs = 0;
String g_bleIncomingBuffer;
WebServer g_rpcServer(80);
bool g_rpcServerConfigured = false;

String truncateToLength(const String &value, size_t maxLength) {
  if (value.length() <= maxLength) {
    return value;
  }
  return value.substring(0, maxLength);
}

String sanitizeHostname(String value) {
  value.trim();
  value.toLowerCase();

  String output;
  output.reserve(value.length());
  for (size_t index = 0; index < value.length(); ++index) {
    const char ch = value[index];
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
      output += ch;
    } else if (ch == ' ' || ch == '_' || ch == '.') {
      output += '-';
    }
  }

  while (output.startsWith("-")) {
    output.remove(0, 1);
  }
  while (output.endsWith("-")) {
    output.remove(output.length() - 1, 1);
  }

  if (output.length() == 0) {
    return "lora20-device";
  }

  if (output.length() > lora20::kMaxHostnameLength) {
    output.remove(lora20::kMaxHostnameLength);
  }

  return output;
}

template <size_t N>
void copyStringToArray(const String &value, char (&target)[N]) {
  std::memset(target, 0, sizeof(target));
  truncateToLength(value, N - 1).toCharArray(target, N);
}

String chipSuffixFromEfuse() {
  const uint64_t chipId = ESP.getEfuseMac();
  char buffer[17];
  snprintf(buffer,
           sizeof(buffer),
           "%04llx%08llx",
           static_cast<unsigned long long>((chipId >> 32) & 0xFFFFULL),
           static_cast<unsigned long long>(chipId & 0xFFFFFFFFULL));
  String text(buffer);
  return text.length() > 4 ? text.substring(text.length() - 4) : text;
}

uint32_t generatePairingPin() {
  uint32_t pin = esp_random() % 1000000UL;
  if (pin < 100000UL) {
    pin += 100000UL;
  }
  return pin;
}

bool isAllowedDisplaySleep(uint32_t value) {
  return value == 0 || value == 60 || value == 120 || value == 240 || value == 480;
}

uint8_t batteryPercentFromMillivolts(uint16_t millivolts) {
  constexpr uint16_t kEmptyMv = 3300;
  constexpr uint16_t kFullMv = 4200;
  if (millivolts <= kEmptyMv) {
    return 0;
  }
  if (millivolts >= kFullMv) {
    return 100;
  }
  return static_cast<uint8_t>(((millivolts - kEmptyMv) * 100U) / (kFullMv - kEmptyMv));
}

bool hasValidIpAddress(const IPAddress &address) {
  return address[0] != 0 || address[1] != 0 || address[2] != 0 || address[3] != 0;
}

void notifyBleText(const String &text) {
  if (g_bleTxCharacteristic == nullptr || !g_bleAuthenticated) {
    return;
  }

  const String payload = text.endsWith("\n") ? text : text + "\n";
  for (size_t offset = 0; offset < payload.length(); offset += 20) {
    const String chunk = payload.substring(offset, std::min(payload.length(), offset + 20));
    g_bleTxCharacteristic->setValue(std::string(chunk.c_str(), chunk.length()));
    g_bleTxCharacteristic->notify();
    delay(10);
  }
}

class ConnectivityBleSecurityCallbacks : public BLESecurityCallbacks {
 public:
  uint32_t onPassKeyRequest() override {
    if (g_activeConnectivity != nullptr) {
      return g_activeConnectivity->status().bluetooth.pin;
    }
    return 123456;
  }

  void onPassKeyNotify(uint32_t) override {
    g_blePairing = true;
  }

  bool onConfirmPIN(uint32_t) override {
    return true;
  }

  bool onSecurityRequest() override {
    g_blePairing = true;
    return true;
  }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    g_blePairing = false;
    g_bleAuthenticated = cmpl.success;
    if (!cmpl.success && g_bleServer != nullptr) {
      g_bleServer->disconnect(g_bleLastConnId);
      g_bleAdvertRestartAtMs = millis() + kBleAdvertRestartDelayMs;
    }
  }
};

class ConnectivityBleServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    if (param != nullptr) {
      g_bleLastConnId = param->connect.conn_id;
    }
    g_bleDeviceConnected = true;
    g_blePairing = true;
  }

  void onDisconnect(BLEServer *) override {
    g_bleDeviceConnected = false;
    g_bleAuthenticated = false;
    g_blePairing = false;
    g_bleAdvertRestartAtMs = millis() + kBleAdvertRestartDelayMs;
  }
};

class ConnectivityBleCharacteristicCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *) override {
    if (characteristic == nullptr) {
      return;
    }

    const uint8_t *data = characteristic->getData();
    const size_t length = characteristic->getLength();
    if (data == nullptr || length == 0) {
      return;
    }

    String chunk;
    chunk.reserve(length);
    for (size_t index = 0; index < length; ++index) {
      chunk += static_cast<char>(data[index]);
    }
    g_bleIncomingBuffer += chunk;
    g_bleIncomingActivity = true;
  }
};

ConnectivityBleSecurityCallbacks g_bleSecurityCallbacks;
ConnectivityBleServerCallbacks g_bleServerCallbacks;
ConnectivityBleCharacteristicCallbacks g_bleCharacteristicCallbacks;

}  // namespace

namespace lora20 {

ConnectivityManager::ConnectivityManager(DeviceStateStore &state) : state_(state) {}

void ConnectivityManager::attachRpcServer(SerialRpcServer &rpc) {
  rpc_ = &rpc;
}

bool ConnectivityManager::begin(String &error) {
  error = "";
  g_activeConnectivity = this;

  if (!ensureConfigDefaults(error)) {
    return false;
  }

  refreshUsbState();
  refreshBattery();
  return applyConfig(true, error);
}

void ConnectivityManager::poll() {
  const unsigned long nowMs = millis();
  refreshUsbState();
  refreshBluetoothState(nowMs);
  refreshWifiState(nowMs);
  refreshBattery();
  refreshRuntime();
  maybeRestart(nowMs);
}

const ConnectivityRuntimeStatus &ConnectivityManager::status() const {
  return status_;
}

const ConnectivityConfig &ConnectivityManager::config() const {
  return state_.snapshot().connectivity;
}

bool ConnectivityManager::updateConfig(const ConnectivityConfig &config, String &error) {
  if (!state_.updateConnectivityConfig(config, error)) {
    return false;
  }
  markActivity();
  return applyConfig(false, error);
}

bool ConnectivityManager::ensureConfigDefaults(String &error) {
  ConnectivityConfig next = state_.snapshot().connectivity;
  bool changed = false;

  if (next.bluetoothPin < 100000UL || next.bluetoothPin > 999999UL) {
    next.bluetoothPin = generatePairingPin();
    changed = true;
  }

  if (String(next.bluetoothName).length() == 0) {
    copyStringToArray(resolveBluetoothName(), next.bluetoothName);
    changed = true;
  }

  if (String(next.wifiHostname).length() == 0) {
    copyStringToArray(resolveWifiHostname(), next.wifiHostname);
    changed = true;
  }

  if (!isAllowedDisplaySleep(next.displaySleepSeconds)) {
    next.displaySleepSeconds = 60;
    changed = true;
  }

  if (next.powerSaveLevel > 2) {
    next.powerSaveLevel = 1;
    changed = true;
  }

  if (!changed) {
    return true;
  }

  return state_.updateConnectivityConfig(next, error);
}

bool ConnectivityManager::applyConfig(bool, String &error) {
  error = "";

  if (config().bluetoothEnabled) {
    if (!startBluetooth(error)) {
      return false;
    }
  } else {
    stopBluetooth();
  }

  if (config().wifiEnabled && String(config().wifiSsid).length() > 0) {
    if (!startWifi(error)) {
      return false;
    }
  } else {
    stopWifi();
  }

  refreshRuntime();
  return true;
}

void ConnectivityManager::refreshRuntime() {
  status_.preferredInterface = "auto";
  status_.bluetooth.enabled = config().bluetoothEnabled;
  status_.bluetooth.deviceName = resolveBluetoothName();
  status_.bluetooth.pin = resolveBluetoothPin();
  status_.wifi.enabled = config().wifiEnabled;
  status_.wifi.configured = String(config().wifiSsid).length() > 0;
  status_.wifi.apFallback = config().wifiApFallback;
  status_.wifi.ssid = String(config().wifiSsid);
  status_.wifi.hostname = resolveWifiHostname();
  status_.tokenSet = String(config().rpcToken).length() > 0;
  status_.wifiConfigured = status_.wifi.configured;
  status_.displaySleepSeconds = config().displaySleepSeconds;
  status_.bridgeWindowSeconds = config().bridgeWindowSeconds;
  status_.powerSaveLevel = config().powerSaveLevel;

  if (status_.wifi.connected) {
    status_.activeInterface = "wifi";
  } else if (status_.bluetooth.connected) {
    status_.activeInterface = "bluetooth";
  } else if (status_.usbConnected) {
    status_.activeInterface = "usb";
  } else {
    status_.activeInterface = "none";
  }

  if (status_.wifi.lastError.length() > 0) {
    status_.lastError = status_.wifi.lastError;
  } else if (status_.bluetooth.lastError.length() > 0) {
    status_.lastError = status_.bluetooth.lastError;
  } else {
    status_.lastError = "";
  }
}

void ConnectivityManager::refreshBattery() {
  const unsigned long nowMs = millis();
  if (nextBatterySampleMs_ != 0 && nowMs < nextBatterySampleMs_) {
    return;
  }

#if defined(PIN_ADC_CTRL) && defined(PIN_VBAT_READ)
  analogReadResolution(10);
  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, HIGH);
  delay(10);

  uint32_t raw = 0;
  for (int index = 0; index < 8; ++index) {
    raw += analogRead(PIN_VBAT_READ);
  }
  raw /= 8;
  digitalWrite(PIN_ADC_CTRL, LOW);

  const float voltage = (5.42f * (3.3f / 1024.0f) * static_cast<float>(raw)) * 1000.0f;
  status_.battery.available = true;
  status_.battery.millivolts = static_cast<uint16_t>(voltage);
  status_.battery.percent = batteryPercentFromMillivolts(status_.battery.millivolts);
#else
  status_.battery.available = false;
  status_.battery.millivolts = 0;
  status_.battery.percent = 0;
#endif

  nextBatterySampleMs_ = nowMs + kBatterySamplePeriodMs;
}

void ConnectivityManager::refreshUsbState() {
  status_.usbConnected = static_cast<bool>(Serial);
}

void ConnectivityManager::refreshWifiState(unsigned long nowMs) {
  const String previousState = status_.wifi.state;
  const bool previousConnected = status_.wifi.connected;
  const String previousIp = status_.wifi.ipAddress;

  handleWifiHttp();

  if (!config().wifiEnabled) {
    status_.wifi.state = "disabled";
    status_.wifi.connected = false;
    status_.wifi.connecting = false;
    status_.wifi.dhcpReady = false;
    status_.wifi.ipAddress = "";
    status_.wifi.lastError = "";
    wifiWasConnected_ = false;
  } else if (!status_.wifi.configured) {
    status_.wifi.state = "not_configured";
    status_.wifi.connected = false;
    status_.wifi.connecting = false;
    status_.wifi.dhcpReady = false;
    status_.wifi.ipAddress = "";
    status_.wifi.lastError = "Wi-Fi SSID is missing";
    wifiWasConnected_ = false;
  } else {
    const wl_status_t wifiStatus = WiFi.status();
    const IPAddress localIp = WiFi.localIP();
    const bool hasIp = hasValidIpAddress(localIp);

    status_.wifi.rssi = wifiStatus == WL_CONNECTED ? WiFi.RSSI() : 0;
    status_.wifi.ipAddress = hasIp ? localIp.toString() : "";

    if (wifiStatus == WL_CONNECTED && hasIp) {
      status_.wifi.state = "connected";
      status_.wifi.connected = true;
      status_.wifi.connecting = false;
      status_.wifi.dhcpReady = true;
      status_.wifi.lastError = "";
      wifiConnectRequested_ = false;
      if (!wifiWasConnected_) {
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        wifiWasConnected_ = true;
      }
    } else {
      wifiWasConnected_ = false;
      status_.wifi.connected = false;
      status_.wifi.dhcpReady = false;

      if (wifiStatus == WL_CONNECTED && !hasIp) {
        status_.wifi.state = "dhcp_wait";
        status_.wifi.connecting = true;
        if (wifiConnectStartedMs_ != 0 && (nowMs - wifiConnectStartedMs_) > kWifiDhcpTimeoutMs) {
          setWifiErrorState("no_dhcp", "DHCP timed out");
          nextWifiRetryMs_ = nowMs + kWifiRetryDelayMs;
          WiFi.disconnect(false, false);
          wifiConnectRequested_ = false;
        }
      } else {
        switch (wifiStatus) {
          case WL_NO_SSID_AVAIL:
            setWifiErrorState("no_ssid", "Configured Wi-Fi network is not visible");
            nextWifiRetryMs_ = nowMs + kWifiRetryDelayMs;
            wifiConnectRequested_ = false;
            break;
          case WL_CONNECT_FAILED:
            setWifiErrorState("auth_failed", "Wi-Fi authentication failed");
            nextWifiRetryMs_ = nowMs + kWifiRetryDelayMs;
            wifiConnectRequested_ = false;
            break;
          case WL_CONNECTION_LOST:
            setWifiErrorState("connection_lost", "Wi-Fi connection lost");
            nextWifiRetryMs_ = nowMs + kWifiRetryDelayMs;
            wifiConnectRequested_ = false;
            break;
          case WL_IDLE_STATUS:
          case WL_DISCONNECTED:
          default:
            if (wifiConnectRequested_) {
              if (wifiConnectStartedMs_ != 0 && (nowMs - wifiConnectStartedMs_) > kWifiConnectTimeoutMs) {
                setWifiErrorState("timeout", "Wi-Fi association timed out");
                nextWifiRetryMs_ = nowMs + kWifiRetryDelayMs;
                wifiConnectRequested_ = false;
              } else {
                status_.wifi.state = "connecting";
                status_.wifi.connecting = true;
              }
            } else if (config().wifiReconnect && nowMs >= nextWifiRetryMs_) {
              String error;
              startWifi(error);
            } else {
              status_.wifi.state = "disconnected";
              status_.wifi.connecting = false;
            }
            break;
        }
      }
    }
  }

  if (previousState != status_.wifi.state ||
      previousConnected != status_.wifi.connected ||
      previousIp != status_.wifi.ipAddress) {
    markActivity();
  }
}

void ConnectivityManager::refreshBluetoothState(unsigned long nowMs) {
  const String previousState = status_.bluetooth.state;
  const bool previousConnected = status_.bluetooth.connected;
  const bool previousPairing = status_.bluetooth.pairing;

  if (!config().bluetoothEnabled) {
    status_.bluetooth.state = "disabled";
    status_.bluetooth.available = false;
    status_.bluetooth.connected = false;
    status_.bluetooth.pairing = false;
  } else {
    if (g_bleAdvertRestartAtMs != 0 && nowMs >= g_bleAdvertRestartAtMs && g_bleServer != nullptr) {
      g_bleServer->getAdvertising()->start();
      g_bleAdvertRestartAtMs = 0;
    }

    status_.bluetooth.available = bleStarted_;
    status_.bluetooth.connected = g_bleAuthenticated;
    status_.bluetooth.pairing = g_blePairing || (g_bleDeviceConnected && !g_bleAuthenticated);

    if (status_.bluetooth.connected) {
      status_.bluetooth.state = "connected";
      clearBluetoothErrorState();
    } else if (status_.bluetooth.pairing) {
      status_.bluetooth.state = "pairing";
    } else if (bleStarted_) {
      status_.bluetooth.state = "ready";
    } else {
      status_.bluetooth.state = "disabled";
    }
  }

  while (true) {
    const int newlineIndex = g_bleIncomingBuffer.indexOf('\n');
    if (newlineIndex < 0) {
      break;
    }

    const String line = g_bleIncomingBuffer.substring(0, newlineIndex);
    g_bleIncomingBuffer.remove(0, newlineIndex + 1);
    const String trimmed = line;
    if (trimmed.length() == 0) {
      continue;
    }

    String response;
    String error;
    bool rebootRequested = false;
    if (!processRpcPayload(trimmed, response, rebootRequested, error)) {
      StaticJsonDocument<256> failure;
      failure["ok"] = false;
      JsonObject err = failure.createNestedObject("error");
      err["code"] = "ble_rpc_failed";
      err["message"] = error;
      serializeJson(failure, response);
    }

    notifyBleText(response);
    markActivity();
    if (rebootRequested) {
      scheduleRestart(180);
    }
  }

  if (g_bleIncomingActivity) {
    g_bleIncomingActivity = false;
    markActivity();
  }

  if (previousState != status_.bluetooth.state ||
      previousConnected != status_.bluetooth.connected ||
      previousPairing != status_.bluetooth.pairing) {
    markActivity();
  }
}

bool ConnectivityManager::startBluetooth(String &error) {
  error = "";
  if (bleStarted_) {
    return true;
  }

  const String name = resolveBluetoothName();
  const uint32_t pin = resolveBluetoothPin();

  BLEDevice::init(name.c_str());
  BLEDevice::setSecurityCallbacks(&g_bleSecurityCallbacks);
  BLEDevice::setMTU(247);

  BLESecurity security;
  security.setStaticPIN(pin);
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);

  g_bleServer = BLEDevice::createServer();
  if (g_bleServer == nullptr) {
    error = "Failed to create BLE server";
    setBluetoothErrorState("error", error);
    return false;
  }

  g_bleServer->setCallbacks(&g_bleServerCallbacks);
  g_bleService = g_bleServer->createService(kBleServiceUuid);
  g_bleTxCharacteristic = g_bleService->createCharacteristic(
      kBleTxUuid,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_bleTxCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  g_bleTxCharacteristic->addDescriptor(new BLE2902());

  g_bleRxCharacteristic = g_bleService->createCharacteristic(
      kBleRxUuid,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  g_bleRxCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  g_bleRxCharacteristic->setCallbacks(&g_bleCharacteristicCallbacks);

  g_bleServer->getAdvertising()->addServiceUUID(kBleServiceUuid);
  g_bleService->start();
  g_bleServer->getAdvertising()->start();

  bleStarted_ = true;
  g_bleIncomingBuffer = "";
  g_bleDeviceConnected = false;
  g_bleAuthenticated = false;
  g_blePairing = false;
  g_bleIncomingActivity = false;
  clearBluetoothErrorState();
  markActivity();
  return true;
}

void ConnectivityManager::stopBluetooth() {
  if (!bleStarted_) {
    return;
  }

  if (g_bleServer != nullptr) {
    g_bleServer->getAdvertising()->stop();
    if (g_bleLastConnId != 0) {
      g_bleServer->disconnect(g_bleLastConnId);
    }
  }
  if (g_bleService != nullptr) {
    g_bleService->stop();
  }

  g_bleIncomingBuffer = "";
  g_bleDeviceConnected = false;
  g_bleAuthenticated = false;
  g_blePairing = false;
  g_bleIncomingActivity = false;
  g_bleAdvertRestartAtMs = 0;
  bleStarted_ = false;
  markActivity();
}

bool ConnectivityManager::startWifi(String &error) {
  error = "";
  const String ssid = String(config().wifiSsid);
  if (ssid.length() == 0) {
    setWifiErrorState("not_configured", "Wi-Fi SSID is missing");
    error = status_.wifi.lastError;
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(20);
  WiFi.setAutoReconnect(config().wifiReconnect);
  WiFi.setHostname(resolveWifiHostname().c_str());
  configureWifiPowerSave();

  if (!wifiServerStarted_) {
    if (!g_rpcServerConfigured) {
      g_rpcServer.on(
          "/rpc",
          HTTP_POST,
          [this]() {
            markActivity();

            if (rpc_ == nullptr) {
              g_rpcServer.send(
                  503,
                  "application/json",
                  "{\"ok\":false,\"error\":{\"code\":\"rpc_unavailable\",\"message\":\"RPC server is unavailable\"}}");
              return;
            }

            const String payload = g_rpcServer.arg("plain");
            DynamicJsonDocument authDoc(256);
            const auto parseError = deserializeJson(authDoc, payload);
            if (parseError) {
              g_rpcServer.send(
                  400,
                  "application/json",
                  "{\"ok\":false,\"error\":{\"code\":\"invalid_json\",\"message\":\"Invalid JSON payload\"}}");
              return;
            }

            const String expectedToken = String(config().rpcToken);
            const String providedToken =
                authDoc["auth"].is<const char *>() ? String(authDoc["auth"].as<const char *>()) : "";
            if (expectedToken.length() > 0 && providedToken != expectedToken) {
              g_rpcServer.send(
                  401,
                  "application/json",
                  "{\"ok\":false,\"error\":{\"code\":\"unauthorized\",\"message\":\"Wi-Fi auth token is invalid\"}}");
              return;
            }

            String response;
            String errorText;
            bool rebootRequested = false;
            if (!processRpcPayload(payload, response, rebootRequested, errorText)) {
              StaticJsonDocument<256> failure;
              failure["ok"] = false;
              JsonObject err = failure.createNestedObject("error");
              err["code"] = "rpc_failed";
              err["message"] = errorText;
              serializeJson(failure, response);
              g_rpcServer.send(400, "application/json", response);
              return;
            }

            g_rpcServer.send(200, "application/json", response);
            if (rebootRequested) {
              scheduleRestart(180);
            }
          });
      g_rpcServer.onNotFound([this]() {
        markActivity();
        g_rpcServer.send(
            404,
            "application/json",
            "{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"No route for this path\"}}");
      });
      g_rpcServerConfigured = true;
    }

    g_rpcServer.begin();
    wifiServerStarted_ = true;
  }

  if (String(config().wifiPassword).length() > 0) {
    WiFi.begin(config().wifiSsid, config().wifiPassword);
  } else {
    WiFi.begin(config().wifiSsid);
  }
  wifiConnectRequested_ = true;
  wifiConnectStartedMs_ = millis();
  nextWifiRetryMs_ = millis() + kWifiRetryDelayMs;
  status_.wifi.state = "connecting";
  status_.wifi.connecting = true;
  clearWifiErrorState();
  markActivity();
  return true;
}

void ConnectivityManager::stopWifi() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  wifiConnectRequested_ = false;
  wifiConnectStartedMs_ = 0;
  nextWifiRetryMs_ = 0;
  wifiWasConnected_ = false;
  status_.wifi.state = "disabled";
  status_.wifi.connected = false;
  status_.wifi.connecting = false;
  status_.wifi.dhcpReady = false;
  status_.wifi.ipAddress = "";
  status_.wifi.lastError = "";
  markActivity();
}

void ConnectivityManager::configureWifiPowerSave() {
  switch (config().powerSaveLevel) {
    case 0:
      WiFi.setSleep(false);
      esp_wifi_set_ps(WIFI_PS_NONE);
      break;
    case 2:
      WiFi.setSleep(true);
      esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
      break;
    case 1:
    default:
      WiFi.setSleep(true);
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      break;
  }
}

void ConnectivityManager::handleWifiHttp() {
  if (!wifiServerStarted_) {
    return;
  }
  g_rpcServer.handleClient();
}

bool ConnectivityManager::processRpcPayload(const String &payload,
                                            String &response,
                                            bool &rebootRequested,
                                            String &error) {
  if (rpc_ == nullptr) {
    error = "RPC server is unavailable";
    return false;
  }
  return rpc_->processRequestLine(payload, response, rebootRequested, error);
}

void ConnectivityManager::scheduleRestart(unsigned long delayMs) {
  scheduledRestartAtMs_ = millis() + delayMs;
}

void ConnectivityManager::maybeRestart(unsigned long nowMs) {
  if (scheduledRestartAtMs_ == 0 || nowMs < scheduledRestartAtMs_) {
    return;
  }
  scheduledRestartAtMs_ = 0;
  delay(50);
  ESP.restart();
}

void ConnectivityManager::markActivity() {
  status_.activityCounter += 1;
  if (status_.activityCounter == 0) {
    status_.activityCounter = 1;
  }
}

void ConnectivityManager::setWifiErrorState(const String &state, const String &error) {
  status_.wifi.state = state;
  status_.wifi.connecting = false;
  status_.wifi.connected = false;
  status_.wifi.dhcpReady = false;
  status_.wifi.lastError = error;
}

void ConnectivityManager::clearWifiErrorState() {
  status_.wifi.lastError = "";
}

void ConnectivityManager::setBluetoothErrorState(const String &state, const String &error) {
  status_.bluetooth.state = state;
  status_.bluetooth.lastError = error;
}

void ConnectivityManager::clearBluetoothErrorState() {
  status_.bluetooth.lastError = "";
}

String ConnectivityManager::resolveBluetoothName() const {
  const String configured = String(config().bluetoothName);
  if (configured.length() > 0) {
    return truncateToLength(configured, kMaxBluetoothNameLength);
  }
  return truncateToLength(String("lora20-") + chipSuffix(), kMaxBluetoothNameLength);
}

String ConnectivityManager::resolveWifiHostname() const {
  const String configured = String(config().wifiHostname);
  if (configured.length() > 0) {
    return sanitizeHostname(configured);
  }
  return sanitizeHostname(String("lora20-") + chipSuffix());
}

uint32_t ConnectivityManager::resolveBluetoothPin() const {
  return config().bluetoothPin;
}

String ConnectivityManager::chipSuffix() const {
  return chipSuffixFromEfuse();
}

}  // namespace lora20

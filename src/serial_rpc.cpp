#include "serial_rpc.hpp"

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {

constexpr size_t kLineLimit = 3072;
constexpr size_t kResponseCapacity = 6144;

String u64ToString(uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
}

bool isAllowedDisplaySleepSeconds(uint32_t value) {
  return value == 0 || value == 60 || value == 120 || value == 240 || value == 480;
}

void writeProfiles(JsonArray target, const std::vector<lora20::MintProfile> &profiles) {
  for (const auto &profile : profiles) {
    JsonObject entry = target.createNestedObject();
    entry["tick"] = lora20::tickToString(profile.tick);
    entry["amount"] = u64ToString(profile.amount);
    entry["enabled"] = profile.enabled;
  }
}

bool readUint64Param(JsonVariantConst value, uint64_t &out);
bool readUint32Param(JsonVariantConst value, uint32_t &out);

template <size_t N>
void copyStringParam(const String &value, char (&target)[N]) {
  std::memset(target, 0, sizeof(target));
  value.substring(0, N - 1).toCharArray(target, N);
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

const char *resolveCommand(JsonDocument &request) {
  if (request["command"].is<const char *>()) {
    return request["command"].as<const char *>();
  }
  if (request["method"].is<const char *>()) {
    return request["method"].as<const char *>();
  }
  return "";
}

void sendDocument(Stream &serial, const JsonDocument &document) {
  serializeJson(document, serial);
  serial.println();
}

bool isHexChar(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

String toLowerAscii(const String &input) {
  String output = input;
  for (size_t index = 0; index < output.length(); ++index) {
    const char ch = output[index];
    if (ch >= 'A' && ch <= 'Z') {
      output.setCharAt(index, static_cast<char>(ch + ('a' - 'A')));
    }
  }
  return output;
}

bool normalizeHeltecLicenseInput(const String &rawInput, String &normalized) {
  String working = rawInput;
  working.trim();

  if (working.length() == 0) {
    normalized = "";
    return true;
  }

  const String lowered = toLowerAscii(working);
  if (lowered.startsWith("license=")) {
    working = working.substring(8);
    working.trim();
  }

  std::array<uint8_t, 16> parsed{};
  if (lora20::hexToBytes(working, parsed.data(), parsed.size())) {
    normalized = toLowerAscii(working);
    return true;
  }

  String combined;
  String token;

  auto flushToken = [&](String &value) -> bool {
    value.trim();
    if (value.length() == 0) {
      return true;
    }

    if (value.startsWith("0x") || value.startsWith("0X")) {
      value = value.substring(2);
    }

    if (value.length() != 8) {
      return false;
    }

    for (size_t index = 0; index < value.length(); ++index) {
      if (!isHexChar(value[index])) {
        return false;
      }
    }

    combined += value;
    value = "";
    return true;
  };

  for (size_t index = 0; index < working.length(); ++index) {
    const char ch = working[index];
    if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      if (!flushToken(token)) {
        return false;
      }
      continue;
    }
    token += ch;
  }

  if (!flushToken(token)) {
    return false;
  }

  if (!lora20::hexToBytes(combined, parsed.data(), parsed.size())) {
    return false;
  }

  normalized = toLowerAscii(combined);
  return true;
}

void writeConfig(JsonObject target, const lora20::DeviceConfig &config) {
  target["autoMintEnabled"] = config.autoMintEnabled;
  target["autoMintIntervalSeconds"] = config.autoMintIntervalSeconds;
  target["defaultTick"] = lora20::tickToString(config.defaultTick);
  target["defaultMintAmount"] = u64ToString(config.defaultMintAmount);
  target["schedulerMode"] = "round_robin";
  target["profileCount"] = config.mintProfiles.size();
  writeProfiles(target.createNestedArray("profiles"), config.mintProfiles);
}

bool readMintProfilesParam(JsonVariantConst value, std::vector<lora20::MintProfile> &profiles, String &error) {
  if (value.isNull()) {
    return true;
  }

  JsonArrayConst array = value.as<JsonArrayConst>();
  if (array.isNull()) {
    error = "profiles must be an array";
    return false;
  }

  profiles.clear();
  for (JsonObjectConst entry : array) {
    if (profiles.size() >= lora20::kMaxMintProfiles) {
      error = "profiles exceeds the device limit";
      return false;
    }

    if (!entry["tick"].is<const char *>()) {
      error = "each profile requires tick";
      return false;
    }

    lora20::MintProfile profile;
    if (!lora20::normalizeTick(String(entry["tick"].as<const char *>()), profile.tick, error)) {
      return false;
    }

    if (!readUint64Param(entry["amount"], profile.amount) || profile.amount == 0) {
      error = "each profile requires a positive amount";
      return false;
    }

    profile.enabled = entry["enabled"].is<bool>() ? entry["enabled"].as<bool>() : true;
    profiles.push_back(profile);
  }

  return true;
}

void writeLoRaWanConfig(JsonObject target, const lora20::LoRaWanConfig &config) {
  target["autoDevEui"] = config.autoDevEui;
  target["adr"] = config.adr;
  target["confirmedUplink"] = config.confirmedUplink;
  target["appPort"] = config.appPort;
  target["defaultDataRate"] = config.defaultDataRate;
  target["region"] = config.region;
  target["hasDevEui"] = config.hasDevEui;
  target["hasJoinEui"] = config.hasJoinEui;
  target["hasAppKey"] = config.hasAppKey;
  target["devEuiHex"] = config.hasDevEui ? lora20::toHex(config.devEui) : "";
  target["joinEuiHex"] = config.hasJoinEui ? lora20::toHex(config.joinEui) : "";
  target["appKeyHex"] = config.hasAppKey ? lora20::toHex(config.appKey) : "";
}

void writeLoRaWanStatus(JsonObject target, const lora20::LoRaWanRuntimeStatus &status) {
  target["hardwareReady"] = status.hardwareReady;
  target["initialized"] = status.initialized;
  target["configured"] = status.configured;
  target["joining"] = status.joining;
  target["joined"] = status.joined;
  target["queuePending"] = status.queuePending;
  target["lastSendAccepted"] = status.lastSendAccepted;
  target["activePort"] = status.activePort;
  target["queuedPayloadSize"] = status.queuedPayloadSize;
  target["lastAcceptedPayloadSize"] = status.lastAcceptedPayloadSize;
  target["lastJoinAttemptMs"] = status.lastJoinAttemptMs;
  target["lastAcceptedSendMs"] = status.lastAcceptedSendMs;
  target["lastDownlinkRssi"] = status.lastDownlinkRssi;
  target["lastDownlinkSnr"] = status.lastDownlinkSnr;
  target["lastDownlinkPort"] = status.lastDownlinkPort;
  target["region"] = status.region;
  target["chipIdHex"] = status.chipIdHex;
  target["lastEvent"] = status.lastEvent;
  target["lastError"] = status.lastError;
  target["lastDownlinkHex"] = status.lastDownlinkHex;
}

void writeHeltecLicense(JsonObject target,
                        const lora20::HeltecLicenseConfig &config,
                        const lora20::LoRaWanRuntimeStatus &runtime) {
  target["hasLicense"] = config.hasLicense;
  target["licenseHex"] = config.hasLicense ? lora20::toHex(config.value) : "";
  target["chipIdHex"] = runtime.chipIdHex;
}

void writeSnapshot(JsonObject target, const lora20::DeviceSnapshot &snapshot) {
  target["hasKey"] = snapshot.hasKey;
  target["deviceId"] = snapshot.hasKey ? lora20::toHex(snapshot.deviceId) : "";
  target["publicKeyHex"] = snapshot.hasKey ? lora20::toHex(snapshot.publicKey) : "";
  target["nextNonce"] = snapshot.nextNonce;
  writeConfig(target.createNestedObject("config"), snapshot.config);

  JsonObject connection = target.createNestedObject("connection");
  connection["bluetoothEnabled"] = snapshot.connectivity.bluetoothEnabled;
  connection["bluetoothName"] = snapshot.connectivity.bluetoothName;
  connection["bluetoothPin"] = snapshot.connectivity.bluetoothPin;
  connection["wifiEnabled"] = snapshot.connectivity.wifiEnabled;
  connection["wifiSsid"] = snapshot.connectivity.wifiSsid;
  connection["wifiHostname"] = snapshot.connectivity.wifiHostname;
  connection["tokenSet"] = snapshot.connectivity.rpcToken[0] != '\0';
  connection["wifiReconnect"] = snapshot.connectivity.wifiReconnect;
  connection["wifiApFallback"] = snapshot.connectivity.wifiApFallback;
  connection["displaySleepSeconds"] = snapshot.connectivity.displaySleepSeconds;
  connection["bridgeWindowSeconds"] = snapshot.connectivity.bridgeWindowSeconds;
  connection["powerSaveLevel"] = snapshot.connectivity.powerSaveLevel;

  writeLoRaWanConfig(target.createNestedObject("lorawan"), snapshot.loRaWan);
  target["heltecLicensePresent"] = snapshot.heltecLicense.hasLicense;
}

void writeConnectivityConfig(JsonObject target, const lora20::ConnectivityConfig &config) {
  target["bluetoothEnabled"] = config.bluetoothEnabled;
  target["bluetoothName"] = config.bluetoothName;
  target["bluetoothPin"] = config.bluetoothPin;
  target["wifiEnabled"] = config.wifiEnabled;
  target["wifiSsid"] = config.wifiSsid;
  target["wifiHostname"] = config.wifiHostname;
  target["tokenSet"] = config.rpcToken[0] != '\0';
  target["wifiReconnect"] = config.wifiReconnect;
  target["wifiApFallback"] = config.wifiApFallback;
  target["displaySleepSeconds"] = config.displaySleepSeconds;
  target["bridgeWindowSeconds"] = config.bridgeWindowSeconds;
  target["powerSaveLevel"] = config.powerSaveLevel;
}

void writeConnectivityRuntime(JsonObject target, const lora20::ConnectivityRuntimeStatus &status) {
  target["mode"] = status.activeInterface;
  target["activeInterface"] = status.activeInterface;
  target["preferredInterface"] = status.preferredInterface;
  target["usbConnected"] = status.usbConnected;
  target["bluetoothEnabled"] = status.bluetooth.enabled;
  target["bluetoothAvailable"] = status.bluetooth.available;
  target["bluetoothConnected"] = status.bluetooth.connected;
  target["bluetoothPairing"] = status.bluetooth.pairing;
  target["bluetoothPin"] = status.bluetooth.pin;
  target["bluetoothName"] = status.bluetooth.deviceName;
  target["bluetoothState"] = status.bluetooth.state;
  target["wifiEnabled"] = status.wifi.enabled;
  target["wifiConfigured"] = status.wifi.configured;
  target["wifiConnected"] = status.wifi.connected;
  target["wifiState"] = status.wifi.state;
  target["wifiSsid"] = status.wifi.ssid;
  target["wifiIp"] = status.wifi.ipAddress;
  target["wifiHostname"] = status.wifi.hostname;
  target["wifiMode"] = status.wifi.connected ? "sta_connected" : "sta_idle";
  target["wifiRssi"] = status.wifi.rssi;
  target["tokenSet"] = status.tokenSet;
  target["wifiApFallback"] = status.wifi.apFallback;
  target["displaySleepSeconds"] = status.displaySleepSeconds;
  target["bridgeWindowSeconds"] = status.bridgeWindowSeconds;
  target["powerSaveLevel"] = status.powerSaveLevel;
  target["batteryMv"] = status.battery.millivolts;
  target["batteryPercent"] = status.battery.percent;
  target["activityCounter"] = status.activityCounter;
  target["lastError"] = status.lastError;
}

void writeBootStatus(JsonObject target, const lora20::BootControlStatus &status) {
  target["supported"] = status.supported;
  target["currentProtocol"] = status.currentProtocol;
  target["bootProtocol"] = status.bootProtocol;
  target["runningPartitionLabel"] = status.runningPartitionLabel;
  target["bootPartitionLabel"] = status.bootPartitionLabel;
  target["buttonHint"] = status.buttonHint;

  JsonArray slots = target.createNestedArray("slots");
  for (const auto &slot : status.slots) {
    JsonObject entry = slots.createNestedObject();
    entry["protocol"] = slot.protocol;
    entry["partitionLabel"] = slot.partitionLabel;
    entry["subtype"] = slot.subtype;
    entry["address"] = slot.address;
    entry["sizeBytes"] = slot.sizeBytes;
    entry["partitionPresent"] = slot.partitionPresent;
    entry["validImage"] = slot.validImage;
    entry["running"] = slot.running;
    entry["bootTarget"] = slot.bootTarget;
    entry["projectName"] = slot.projectName;
    entry["version"] = slot.version;
  }
}

void writePreparedPayload(JsonObject target, const lora20::PreparedPayload &prepared) {
  target["nonce"] = prepared.nonce;
  target["committed"] = prepared.committed;
  target["unsignedPayloadHex"] = lora20::toHex(prepared.unsignedPayload);
  target["signatureHex"] = lora20::toHex(prepared.signature);
  target["payloadHex"] = lora20::toHex(prepared.payload);
  target["payloadSize"] = prepared.payload.size();
}

bool readUint64Param(JsonVariantConst value, uint64_t &out) {
  if (value.is<const char *>()) {
    return lora20::parseUint64(String(value.as<const char *>()), out);
  }
  if (value.is<uint64_t>()) {
    out = value.as<uint64_t>();
    return true;
  }
  if (value.is<uint32_t>()) {
    out = value.as<uint32_t>();
    return true;
  }
  return false;
}

bool readUint32Param(JsonVariantConst value, uint32_t &out) {
  if (value.is<const char *>()) {
    return lora20::parseUint32(String(value.as<const char *>()), out);
  }
  if (value.is<uint32_t>()) {
    out = value.as<uint32_t>();
    return true;
  }
  if (value.is<uint16_t>()) {
    out = value.as<uint16_t>();
    return true;
  }
  return false;
}

template <size_t N>
bool readHexArrayParam(JsonVariantConst value, std::array<uint8_t, N> &out, bool &hasValue) {
  if (!value.is<const char *>()) {
    return false;
  }

  const String hex = value.as<const char *>();
  if (hex.isEmpty()) {
    out.fill(0);
    hasValue = false;
    return true;
  }

  if (!lora20::hexToBytes(hex, out.data(), out.size())) {
    return false;
  }

  hasValue = true;
  return true;
}

bool readHexVectorParam(JsonVariantConst value, size_t maxBytes, std::vector<uint8_t> &out, String &error) {
  if (!value.is<const char *>()) {
    error = "messageHex must be a hex string";
    return false;
  }

  const String hex = value.as<const char *>();
  if (hex.length() == 0) {
    error = "messageHex must not be empty";
    return false;
  }
  if ((hex.length() % 2) != 0) {
    error = "messageHex must have an even number of characters";
    return false;
  }

  const size_t byteCount = hex.length() / 2;
  if (byteCount > maxBytes) {
    error = "messageHex exceeds the maximum packed size";
    return false;
  }

  out.clear();
  out.reserve(byteCount);
  for (size_t index = 0; index < hex.length(); index += 2) {
    const int high = hexNibble(hex[index]);
    const int low = hexNibble(hex[index + 1]);
    if (high < 0 || low < 0) {
      error = "messageHex contains a non-hex character";
      out.clear();
      return false;
    }
    out.push_back(static_cast<uint8_t>((high << 4) | low));
  }

  return true;
}

}  // namespace

namespace lora20 {

SerialRpcServer::SerialRpcServer(Stream &serial,
                                 DeviceStateStore &state,
                                 LoRaWanClient &lorawan,
                                 BootControl &boot,
                                 ConnectivityManager &connectivity)
    : serial_(serial),
      state_(state),
      lorawan_(lorawan),
      boot_(boot),
      connectivity_(connectivity) {}

void SerialRpcServer::begin() {
  buffer_.reserve(kLineLimit);
  sendBootEvent();
}

void SerialRpcServer::poll() {
  while (serial_.available() > 0) {
    const char ch = static_cast<char>(serial_.read());

    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      const String line = buffer_;
      buffer_.clear();
      handleLine(line);
      continue;
    }

    if (buffer_.length() >= kLineLimit) {
      buffer_.clear();
      DynamicJsonDocument response(256);
      response["ok"] = false;
      JsonObject error = response.createNestedObject("error");
      error["code"] = "line_too_long";
      error["message"] = "Incoming serial line exceeded the size limit";
      sendDocument(serial_, response);
      continue;
    }

    buffer_ += ch;
  }
}

void SerialRpcServer::handleLine(const String &line) {
  if (line.isEmpty()) {
    return;
  }

  String response;
  String error;
  bool rebootRequested = false;
  if (!processRequestLine(line, response, rebootRequested, error)) {
    if (error.length() > 0) {
      DynamicJsonDocument failure(256);
      failure["ok"] = false;
      JsonObject err = failure.createNestedObject("error");
      err["code"] = "rpc_internal_failure";
      err["message"] = error;
      serializeJson(failure, response);
    }
  }

  if (response.length() > 0) {
    serial_.println(response);
  }

  if (rebootRequested) {
    delay(50);
    serial_.flush();
    ESP.restart();
  }
}

bool SerialRpcServer::processRequestLine(const String &line,
                                         String &responseText,
                                         bool &rebootRequested,
                                         String &errorText) {
  responseText = "";
  rebootRequested = false;
  errorText = "";

  DynamicJsonDocument request(3072);
  const auto parseError = deserializeJson(request, line);
  if (parseError) {
    DynamicJsonDocument response(256);
    response["ok"] = false;
    JsonObject error = response.createNestedObject("error");
    error["code"] = "invalid_json";
    error["message"] = parseError.c_str();
    serializeJson(response, responseText);
    return true;
  }

  const char *command = resolveCommand(request);
  const char *requestId = request["id"].is<const char *>() ? request["id"].as<const char *>() : "";
  JsonVariantConst params = request["params"];

  DynamicJsonDocument response(kResponseCapacity);

  auto respondError = [&](const char *code, const String &message) -> bool {
    response.clear();
    if (requestId[0] != '\0') {
      response["id"] = requestId;
    }
    response["ok"] = false;
    JsonObject error = response.createNestedObject("error");
    error["code"] = code;
    error["message"] = message;
    serializeJson(response, responseText);
    return true;
  };

  auto respondSuccess = [&](const std::function<void(JsonObject)> &fillResult) -> bool {
    response.clear();
    if (requestId[0] != '\0') {
      response["id"] = requestId;
    }
    response["ok"] = true;
    JsonObject result = response.createNestedObject("result");
    fillResult(result);
    serializeJson(response, responseText);
    return true;
  };

  if (strlen(command) == 0) {
    return respondError("missing_command", "Request must contain command or method");
  }

  if (strcmp(command, "ping") == 0) {
    return respondSuccess([&](JsonObject result) {
      result["firmware"] = kFirmwareName;
      result["version"] = LORA20_FW_VERSION;
      result["uptimeMs"] = millis();
    });
  }

  if (strcmp(command, "get_info") == 0) {
    return respondSuccess([&](JsonObject result) {
      result["firmware"] = kFirmwareName;
      result["version"] = LORA20_FW_VERSION;
      result["chipModel"] = ESP.getChipModel();
      result["chipRevision"] = ESP.getChipRevision();
      result["flashSize"] = ESP.getFlashChipSize();
      result["freeHeap"] = ESP.getFreeHeap();
      result["uptimeMs"] = millis();
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
      writeLoRaWanStatus(result.createNestedObject("lorawanRuntime"), lorawan_.status());
      writeConnectivityRuntime(result.createNestedObject("connectivity"), connectivity_.status());
      writeBootStatus(result.createNestedObject("boot"), boot_.status());
    });
  }

  if (strcmp(command, "get_boot_control") == 0 || strcmp(command, "get_boot") == 0) {
    return respondSuccess([&](JsonObject result) {
      writeBootStatus(result, boot_.status());
    });
  }

  if (strcmp(command, "set_boot_target") == 0) {
    if (!params["protocol"].is<const char *>()) {
      return respondError("missing_protocol", "set_boot_target requires params.protocol");
    }

    const bool reboot = params["reboot"].is<bool>() ? params["reboot"].as<bool>() : true;
    const String protocol = String(params["protocol"].as<const char *>());
    String error;
    if (!boot_.switchToProtocol(protocol, false, error)) {
      return respondError("boot_switch_failed", error);
    }

    rebootRequested = reboot;
    return respondSuccess([&](JsonObject result) {
      result["protocol"] = protocol;
      result["rebootPending"] = reboot;
      result["message"] = reboot ? "Boot target updated; rebooting now" : "Boot target updated";
      writeBootStatus(result.createNestedObject("boot"), boot_.status());
    });
  }

  if (strcmp(command, "get_lorawan") == 0) {
    return respondSuccess([&](JsonObject result) {
      writeLoRaWanConfig(result.createNestedObject("config"), state_.snapshot().loRaWan);
      writeLoRaWanStatus(result.createNestedObject("runtime"), lorawan_.status());
      writeHeltecLicense(result.createNestedObject("heltec"), state_.snapshot().heltecLicense, lorawan_.status());
    });
  }

  if (strcmp(command, "get_heltec_license") == 0) {
    return respondSuccess([&](JsonObject result) {
      writeHeltecLicense(result, state_.snapshot().heltecLicense, lorawan_.status());
    });
  }

  if (strcmp(command, "get_config") == 0) {
    return respondSuccess([&](JsonObject result) {
      writeConfig(result, state_.snapshot().config);
    });
  }

  if (strcmp(command, "get_connectivity") == 0) {
    return respondSuccess([&](JsonObject result) {
      writeConnectivityConfig(result.createNestedObject("config"), state_.snapshot().connectivity);
      writeConnectivityRuntime(result.createNestedObject("runtime"), connectivity_.status());
      writeConnectivityRuntime(result, connectivity_.status());
    });
  }

  if (strcmp(command, "set_config") == 0) {
    DeviceConfig next = state_.snapshot().config;
    String error;

    if (params["autoMintEnabled"].is<bool>()) {
      next.autoMintEnabled = params["autoMintEnabled"].as<bool>();
    }

    if (!params["autoMintIntervalSeconds"].isNull()) {
      if (!readUint32Param(params["autoMintIntervalSeconds"], next.autoMintIntervalSeconds)) {
        return respondError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be a positive integer");
      }
    }

    if (params["defaultTick"].is<const char *>()) {
      if (!normalizeTick(String(params["defaultTick"].as<const char *>()), next.defaultTick, error)) {
        return respondError("invalid_tick", error);
      }
    }

    if (!params["defaultMintAmount"].isNull()) {
      if (!readUint64Param(params["defaultMintAmount"], next.defaultMintAmount) || next.defaultMintAmount == 0) {
        return respondError("invalid_default_mint_amount", "defaultMintAmount must be a positive integer");
      }
    }

    JsonVariantConst profilesParam = params["profiles"];
    if (profilesParam.isNull()) {
      profilesParam = params["mintProfiles"];
    }
    if (!readMintProfilesParam(profilesParam, next.mintProfiles, error)) {
      return respondError("invalid_profiles", error);
    }

    if (!next.mintProfiles.empty()) {
      const MintProfile *fallbackProfile = nullptr;
      for (const auto &profile : next.mintProfiles) {
        if (profile.enabled) {
          fallbackProfile = &profile;
          break;
        }
      }
      if (fallbackProfile == nullptr) {
        fallbackProfile = &next.mintProfiles.front();
      }
      std::memcpy(next.defaultTick, fallbackProfile->tick, sizeof(next.defaultTick));
      next.defaultMintAmount = fallbackProfile->amount;
    }

    if (next.autoMintEnabled && next.autoMintIntervalSeconds == 0) {
      return respondError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be > 0 when autoMintEnabled=true");
    }

    if (!state_.updateConfig(next, error)) {
      return respondError("config_persist_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writeConfig(result, state_.snapshot().config);
    });
  }

  if (strcmp(command, "set_connectivity") == 0) {
    ConnectivityConfig next = state_.snapshot().connectivity;
    String error;

    if (params["bluetoothEnabled"].is<bool>()) {
      next.bluetoothEnabled = params["bluetoothEnabled"].as<bool>();
    }

    if (params["bluetoothName"].is<const char *>()) {
      copyStringParam(String(params["bluetoothName"].as<const char *>()), next.bluetoothName);
    }

    uint32_t rawValue = 0;
    if (!params["bluetoothPin"].isNull()) {
      if (!readUint32Param(params["bluetoothPin"], rawValue) || rawValue < 100000UL || rawValue > 999999UL) {
        return respondError("invalid_bluetooth_pin", "bluetoothPin must be a 6-digit PIN");
      }
      next.bluetoothPin = rawValue;
    }

    if (params["wifiEnabled"].is<bool>()) {
      next.wifiEnabled = params["wifiEnabled"].as<bool>();
    }

    if (params["wifiSsid"].is<const char *>()) {
      copyStringParam(String(params["wifiSsid"].as<const char *>()), next.wifiSsid);
    }

    if (params["wifiPassword"].is<const char *>()) {
      copyStringParam(String(params["wifiPassword"].as<const char *>()), next.wifiPassword);
    }

    if (params["wifiHostname"].is<const char *>()) {
      copyStringParam(String(params["wifiHostname"].as<const char *>()), next.wifiHostname);
    }

    if (params["rpcToken"].is<const char *>()) {
      copyStringParam(String(params["rpcToken"].as<const char *>()), next.rpcToken);
    }

    if (params["wifiReconnect"].is<bool>()) {
      next.wifiReconnect = params["wifiReconnect"].as<bool>();
    }

    if (params["wifiApFallback"].is<bool>()) {
      next.wifiApFallback = params["wifiApFallback"].as<bool>();
    }

    if (!params["displaySleepSeconds"].isNull()) {
      if (!readUint32Param(params["displaySleepSeconds"], rawValue) ||
          !isAllowedDisplaySleepSeconds(rawValue)) {
        return respondError("invalid_display_sleep", "displaySleepSeconds must be one of: 0, 60, 120, 240, 480");
      }
      next.displaySleepSeconds = rawValue;
    }

    if (!params["bridgeWindowSeconds"].isNull()) {
      if (!readUint32Param(params["bridgeWindowSeconds"], rawValue) || rawValue < 30 || rawValue > 3600) {
        return respondError("invalid_bridge_window", "bridgeWindowSeconds must be between 30 and 3600");
      }
      next.bridgeWindowSeconds = rawValue;
    }

    if (!params["powerSaveLevel"].isNull()) {
      if (!readUint32Param(params["powerSaveLevel"], rawValue) || rawValue > 2) {
        return respondError("invalid_power_save_level", "powerSaveLevel must be 0, 1 or 2");
      }
      next.powerSaveLevel = static_cast<uint8_t>(rawValue);
    }

    if (params["mode"].is<const char *>()) {
      const String mode = String(params["mode"].as<const char *>());
      if (!(mode == "usb" || mode == "bluetooth" || mode == "ble" || mode == "wifi" || mode == "auto")) {
        return respondError("invalid_mode", "mode must be usb, bluetooth, ble, wifi or auto");
      }
    }

    if (!connectivity_.updateConfig(next, error)) {
      return respondError("connectivity_persist_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writeConnectivityConfig(result.createNestedObject("config"), state_.snapshot().connectivity);
      writeConnectivityRuntime(result.createNestedObject("runtime"), connectivity_.status());
      writeConnectivityRuntime(result, connectivity_.status());
    });
  }

  if (strcmp(command, "set_lorawan") == 0) {
    LoRaWanConfig next = state_.snapshot().loRaWan;
    String error;

    if (params["autoDevEui"].is<bool>()) {
      next.autoDevEui = params["autoDevEui"].as<bool>();
    }

    if (params["adr"].is<bool>()) {
      next.adr = params["adr"].as<bool>();
    }

    if (params["confirmedUplink"].is<bool>()) {
      next.confirmedUplink = params["confirmedUplink"].as<bool>();
    }

    uint32_t rawValue = 0;
    if (!params["appPort"].isNull()) {
      if (!readUint32Param(params["appPort"], rawValue) || rawValue == 0 || rawValue > 223) {
        return respondError("invalid_app_port", "appPort must be between 1 and 223");
      }
      next.appPort = static_cast<uint8_t>(rawValue);
    }

    if (!params["defaultDataRate"].isNull()) {
      if (!readUint32Param(params["defaultDataRate"], rawValue) || rawValue > 15) {
        return respondError("invalid_default_data_rate", "defaultDataRate must be between 0 and 15");
      }
      next.defaultDataRate = static_cast<uint8_t>(rawValue);
    }

    if (params["region"].is<const char *>()) {
      next.region = String(params["region"].as<const char *>());
    }

    if (!params["devEuiHex"].isNull() &&
        !readHexArrayParam(params["devEuiHex"], next.devEui, next.hasDevEui)) {
      return respondError("invalid_dev_eui", "devEuiHex must be 16 hex characters");
    }

    if (!params["joinEuiHex"].isNull() &&
        !readHexArrayParam(params["joinEuiHex"], next.joinEui, next.hasJoinEui)) {
      return respondError("invalid_join_eui", "joinEuiHex must be 16 hex characters");
    }

    if (!params["appKeyHex"].isNull() &&
        !readHexArrayParam(params["appKeyHex"], next.appKey, next.hasAppKey)) {
      return respondError("invalid_app_key", "appKeyHex must be 32 hex characters");
    }

    if (!state_.updateLoRaWanConfig(next, error)) {
      return respondError("lorawan_config_persist_failed", error);
    }

    lorawan_.reset();

    return respondSuccess([&](JsonObject result) {
      writeLoRaWanConfig(result.createNestedObject("config"), state_.snapshot().loRaWan);
      writeLoRaWanStatus(result.createNestedObject("runtime"), lorawan_.status());
    });
  }

  if (strcmp(command, "set_heltec_license") == 0) {
    if (!params["licenseHex"].is<const char *>()) {
      return respondError("missing_license", "set_heltec_license requires params.licenseHex");
    }

    HeltecLicenseConfig next = state_.snapshot().heltecLicense;
    String licenseHex;
    if (!normalizeHeltecLicenseInput(String(params["licenseHex"].as<const char *>()), licenseHex)) {
      return respondError("invalid_license", "licenseHex must be 32 hex characters or Heltec license= / 0x...,0x...,0x...,0x... format");
    }

    if (licenseHex.length() == 0) {
      next.hasLicense = false;
      next.value.fill(0);
    } else {
      if (!hexToBytes(licenseHex, next.value.data(), next.value.size())) {
        return respondError("invalid_license", "licenseHex must be 32 hex characters or Heltec license= / 0x...,0x...,0x...,0x... format");
      }
      next.hasLicense = true;
    }

    String error;
    if (!state_.updateHeltecLicense(next, error)) {
      return respondError("heltec_license_persist_failed", error);
    }

    lorawan_.reset();

    return respondSuccess([&](JsonObject result) {
      writeHeltecLicense(result, state_.snapshot().heltecLicense, lorawan_.status());
    });
  }

  if (strcmp(command, "generate_key") == 0) {
    const bool force = params["force"] | false;
    String error;
    if (!state_.generateKey(force, error)) {
      return respondError("generate_key_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
    });
  }

  if (strcmp(command, "get_public_key") == 0) {
    if (!state_.snapshot().hasKey) {
      return respondError("missing_key", "Device key has not been generated yet");
    }

    return respondSuccess([&](JsonObject result) {
      result["deviceId"] = toHex(state_.snapshot().deviceId);
      result["publicKeyHex"] = toHex(state_.snapshot().publicKey);
    });
  }

  if (strcmp(command, "export_backup") == 0) {
    if (!params["passphrase"].is<const char *>()) {
      return respondError("missing_passphrase", "export_backup requires params.passphrase");
    }

    BackupBlob backup;
    String error;
    if (!state_.exportBackup(String(params["passphrase"].as<const char *>()), backup, error)) {
      return respondError("backup_export_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      result["version"] = backup.version;
      result["algorithm"] = backup.algorithm;
      result["saltHex"] = backup.saltHex;
      result["ivHex"] = backup.ivHex;
      result["ciphertextHex"] = backup.ciphertextHex;
      result["tagHex"] = backup.tagHex;
      result["deviceId"] = backup.deviceId;
    });
  }

  if (strcmp(command, "import_backup") == 0) {
    if (!params["passphrase"].is<const char *>()) {
      return respondError("missing_passphrase", "import_backup requires params.passphrase");
    }

    JsonObjectConst backupObject = params["backup"].as<JsonObjectConst>();
    if (backupObject.isNull()) {
      return respondError("missing_backup", "import_backup requires params.backup");
    }

    BackupBlob backup;
    backup.version = backupObject["version"] | 0;
    backup.algorithm = backupObject["algorithm"].is<const char *>() ? backupObject["algorithm"].as<const char *>() : "";
    backup.saltHex = backupObject["saltHex"].is<const char *>() ? backupObject["saltHex"].as<const char *>() : "";
    backup.ivHex = backupObject["ivHex"].is<const char *>() ? backupObject["ivHex"].as<const char *>() : "";
    backup.ciphertextHex =
        backupObject["ciphertextHex"].is<const char *>() ? backupObject["ciphertextHex"].as<const char *>() : "";
    backup.tagHex = backupObject["tagHex"].is<const char *>() ? backupObject["tagHex"].as<const char *>() : "";
    backup.deviceId = backupObject["deviceId"].is<const char *>() ? backupObject["deviceId"].as<const char *>() : "";

    String error;
    if (!state_.importBackup(backup,
                             String(params["passphrase"].as<const char *>()),
                             params["overwrite"] | false,
                             error)) {
      return respondError("backup_import_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
    });
  }

  if (strcmp(command, "join_lorawan") == 0) {
    String error;
    if (!lorawan_.requestJoin(error)) {
      return respondError("lorawan_join_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writeLoRaWanStatus(result, lorawan_.status());
    });
  }

  if (strcmp(command, "lorawan_send") == 0) {
    if (!params["payloadHex"].is<const char *>()) {
      return respondError("missing_payload", "lorawan_send requires params.payloadHex");
    }

    uint8_t port = state_.snapshot().loRaWan.appPort;
    if (!params["port"].isNull()) {
      uint32_t rawPort = 0;
      if (!readUint32Param(params["port"], rawPort) || rawPort == 0 || rawPort > 223) {
        return respondError("invalid_port", "port must be between 1 and 223");
      }
      port = static_cast<uint8_t>(rawPort);
    }

    const bool confirmed =
        params["confirmed"].is<bool>() ? params["confirmed"].as<bool>() : state_.snapshot().loRaWan.confirmedUplink;
    const bool commitNonce = params["commitNonce"] | false;
    uint32_t nonceToCommit = state_.snapshot().nextNonce;
    if (commitNonce &&
        !params["nonceToCommit"].isNull() &&
        !readUint32Param(params["nonceToCommit"], nonceToCommit)) {
      return respondError("invalid_nonce", "nonceToCommit must be a positive integer");
    }

    String error;
    if (!lorawan_.queueUplink(
            String(params["payloadHex"].as<const char *>()),
            port,
            confirmed,
            commitNonce,
            nonceToCommit,
            error)) {
      return respondError("lorawan_send_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      result["queued"] = true;
      result["port"] = port;
      result["confirmed"] = confirmed;
      result["commitNonce"] = commitNonce;
      result["nonceToCommit"] = commitNonce ? nonceToCommit : 0;
      result["nextNonce"] = state_.snapshot().nextNonce;
      writeLoRaWanStatus(result.createNestedObject("runtime"), lorawan_.status());
    });
  }

  if (strcmp(command, "prepare_deploy") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        return respondError("invalid_tick", error);
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t maxSupply = 0;
    uint64_t limitPerMint = 0;
    if (!readUint64Param(params["maxSupply"], maxSupply) ||
        !readUint64Param(params["limitPerMint"], limitPerMint)) {
      return respondError("invalid_supply", "prepare_deploy requires maxSupply and limitPerMint");
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildDeployPayload(state_.snapshot(), tick, maxSupply, limitPerMint, nonce, prepared, error)) {
      return respondError("prepare_deploy_failed", error);
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      return respondError("commit_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
  }

  if (strcmp(command, "prepare_mint") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        return respondError("invalid_tick", error);
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t amount = current.defaultMintAmount;
    if (!params["amount"].isNull() && !readUint64Param(params["amount"], amount)) {
      return respondError("invalid_amount", "amount must be a positive integer");
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildMintPayload(state_.snapshot(), tick, amount, nonce, prepared, error)) {
      return respondError("prepare_mint_failed", error);
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      return respondError("commit_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
  }

  if (strcmp(command, "prepare_transfer") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        return respondError("invalid_tick", error);
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t amount = 0;
    if (!readUint64Param(params["amount"], amount)) {
      return respondError("invalid_amount", "prepare_transfer requires amount");
    }

    if (!params["toDeviceId"].is<const char *>()) {
      return respondError("missing_recipient", "prepare_transfer requires toDeviceId");
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildTransferPayload(state_.snapshot(),
                              tick,
                              amount,
                              String(params["toDeviceId"].as<const char *>()),
                              nonce,
                              prepared,
                              error)) {
      return respondError("prepare_transfer_failed", error);
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      return respondError("commit_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
  }

  if (strcmp(command, "prepare_message") == 0) {
    if (!params["toDeviceId"].is<const char *>()) {
      return respondError("missing_recipient", "prepare_message requires toDeviceId");
    }

    uint32_t rawMessageLength = 0;
    if (!readUint32Param(params["messageLength"], rawMessageLength) ||
        rawMessageLength == 0 ||
        rawMessageLength > 32) {
      return respondError("invalid_message_length", "messageLength must be between 1 and 32");
    }

    std::vector<uint8_t> packedMessage;
    String error;
    if (!readHexVectorParam(params["messageHex"], 24, packedMessage, error)) {
      return respondError("invalid_message_hex", error);
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildMessagePayload(state_.snapshot(),
                             String(params["toDeviceId"].as<const char *>()),
                             static_cast<uint8_t>(rawMessageLength),
                             packedMessage,
                             nonce,
                             prepared,
                             error)) {
      return respondError("prepare_message_failed", error);
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      return respondError("commit_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["messageLength"] = rawMessageLength;
      result["packedSize"] = packedMessage.size();
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
  }

  if (strcmp(command, "prepare_config") == 0) {
    DeviceConfig next = state_.snapshot().config;
    String error;

    if (params["autoMintEnabled"].is<bool>()) {
      next.autoMintEnabled = params["autoMintEnabled"].as<bool>();
    }

    if (!params["autoMintIntervalSeconds"].isNull()) {
      if (!readUint32Param(params["autoMintIntervalSeconds"], next.autoMintIntervalSeconds)) {
        return respondError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be a positive integer");
      }
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildConfigPayload(state_.snapshot(), next, nonce, prepared, error)) {
      return respondError("prepare_config_failed", error);
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.applyCommittedConfig(next, nonce, error)) {
      return respondError("commit_failed", error);
    }

    return respondSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      writeConfig(result.createNestedObject("deviceConfig"), state_.snapshot().config);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
  }

  return respondError("unknown_command", String("Unknown command: ") + command);
}

void SerialRpcServer::sendBootEvent() {
  DynamicJsonDocument boot(2304);
  boot["type"] = "boot";
  boot["firmware"] = kFirmwareName;
  boot["version"] = LORA20_FW_VERSION;
  boot["hasKey"] = state_.snapshot().hasKey;
  boot["deviceId"] = state_.snapshot().hasKey ? toHex(state_.snapshot().deviceId) : "";
  boot["nextNonce"] = state_.snapshot().nextNonce;
  boot["lorawanConfigured"] = lorawan_.status().configured;
  writeLoRaWanStatus(boot.createNestedObject("lorawanRuntime"), lorawan_.status());
  writeConnectivityRuntime(boot.createNestedObject("connectivity"), connectivity_.status());
  writeBootStatus(boot.createNestedObject("bootControl"), boot_.status());
  sendDocument(serial_, boot);
}

}  // namespace lora20

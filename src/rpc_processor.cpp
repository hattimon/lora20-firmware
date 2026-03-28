#include "rpc_processor.hpp"

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {

constexpr size_t kLineLimit = 3072;

String u64ToString(uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
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

const char *resolveCommand(JsonDocument &request) {
  if (request["command"].is<const char *>()) {
    return request["command"].as<const char *>();
  }
  if (request["method"].is<const char *>()) {
    return request["method"].as<const char *>();
  }
  return "";
}

void writeResponse(String &output, const JsonDocument &document) {
  output = "";
  serializeJson(document, output);
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
  writeLoRaWanConfig(target.createNestedObject("lorawan"), snapshot.loRaWan);
  target["heltecLicensePresent"] = snapshot.heltecLicense.hasLicense;
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

}  // namespace

namespace lora20 {

RpcProcessor::RpcProcessor(DeviceStateStore &state, LoRaWanClient &lorawan)
    : state_(state), lorawan_(lorawan) {}

void RpcProcessor::buildBootEvent(String &response) {
  DynamicJsonDocument boot(1024);
  boot["type"] = "boot";
  boot["firmware"] = kFirmwareName;
  boot["version"] = LORA20_FW_VERSION;
  boot["hasKey"] = state_.snapshot().hasKey;
  boot["deviceId"] = state_.snapshot().hasKey ? toHex(state_.snapshot().deviceId) : "";
  boot["nextNonce"] = state_.snapshot().nextNonce;
  boot["lorawanConfigured"] = lorawan_.status().configured;
  JsonObject runtime = boot.createNestedObject("lorawanRuntime");
  writeLoRaWanStatus(runtime, lorawan_.status());
  writeResponse(response, boot);
}

bool RpcProcessor::handleLine(const String &line, String &response) {
  response = "";
  if (line.isEmpty()) {
    return false;
  }

  if (line.length() >= kLineLimit) {
    DynamicJsonDocument responseDoc(256);
    responseDoc["ok"] = false;
    JsonObject error = responseDoc.createNestedObject("error");
    error["code"] = "line_too_long";
    error["message"] = "Incoming line exceeded the size limit";
    writeResponse(response, responseDoc);
    return true;
  }

  DynamicJsonDocument request(3072);
  const auto parseError = deserializeJson(request, line);
  if (parseError) {
    DynamicJsonDocument responseDoc(256);
    responseDoc["ok"] = false;
    JsonObject error = responseDoc.createNestedObject("error");
    error["code"] = "invalid_json";
    error["message"] = parseError.c_str();
    writeResponse(response, responseDoc);
    return true;
  }

  const char *command = resolveCommand(request);
  const char *requestId = request["id"].is<const char *>() ? request["id"].as<const char *>() : "";
  JsonVariantConst params = request["params"];

  DynamicJsonDocument responseDoc(4096);
  if (requestId[0] != '\0') {
    responseDoc["id"] = requestId;
  }

  auto sendError = [&](const char *code, const String &message) {
    responseDoc.clear();
    if (requestId[0] != '\0') {
      responseDoc["id"] = requestId;
    }
    responseDoc["ok"] = false;
    JsonObject error = responseDoc.createNestedObject("error");
    error["code"] = code;
    error["message"] = message;
    writeResponse(response, responseDoc);
  };

  auto sendSuccess = [&](const std::function<void(JsonObject)> &fillResult) {
    responseDoc.clear();
    if (requestId[0] != '\0') {
      responseDoc["id"] = requestId;
    }
    responseDoc["ok"] = true;
    JsonObject result = responseDoc.createNestedObject("result");
    fillResult(result);
    writeResponse(response, responseDoc);
  };

  if (strlen(command) == 0) {
    sendError("missing_command", "Request must contain command or method");
    return true;
  }

  if (strcmp(command, "ping") == 0) {
    sendSuccess([&](JsonObject result) {
      result["firmware"] = kFirmwareName;
      result["version"] = LORA20_FW_VERSION;
      result["uptimeMs"] = millis();
    });
    return true;
  }

  if (strcmp(command, "get_info") == 0) {
    sendSuccess([&](JsonObject result) {
      result["firmware"] = kFirmwareName;
      result["version"] = LORA20_FW_VERSION;
      result["chipModel"] = ESP.getChipModel();
      result["chipRevision"] = ESP.getChipRevision();
      result["flashSize"] = ESP.getFlashChipSize();
      result["freeHeap"] = ESP.getFreeHeap();
      result["uptimeMs"] = millis();
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
      writeLoRaWanStatus(result.createNestedObject("lorawanRuntime"), lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "get_lorawan") == 0) {
    sendSuccess([&](JsonObject result) {
      writeLoRaWanConfig(result.createNestedObject("config"), state_.snapshot().loRaWan);
      writeLoRaWanStatus(result.createNestedObject("runtime"), lorawan_.status());
      writeHeltecLicense(result.createNestedObject("heltec"), state_.snapshot().heltecLicense, lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "get_heltec_license") == 0) {
    sendSuccess([&](JsonObject result) {
      writeHeltecLicense(result, state_.snapshot().heltecLicense, lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "get_config") == 0) {
    sendSuccess([&](JsonObject result) {
      writeConfig(result, state_.snapshot().config);
    });
    return true;
  }

  if (strcmp(command, "set_config") == 0) {
    DeviceConfig next = state_.snapshot().config;
    String error;

    if (params["autoMintEnabled"].is<bool>()) {
      next.autoMintEnabled = params["autoMintEnabled"].as<bool>();
    }

    if (!params["autoMintIntervalSeconds"].isNull()) {
      if (!readUint32Param(params["autoMintIntervalSeconds"], next.autoMintIntervalSeconds)) {
        sendError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be a positive integer");
        return true;
      }
    }

    if (params["defaultTick"].is<const char *>()) {
      if (!normalizeTick(String(params["defaultTick"].as<const char *>()), next.defaultTick, error)) {
        sendError("invalid_tick", error);
        return true;
      }
    }

    if (!params["defaultMintAmount"].isNull()) {
      if (!readUint64Param(params["defaultMintAmount"], next.defaultMintAmount) || next.defaultMintAmount == 0) {
        sendError("invalid_default_mint_amount", "defaultMintAmount must be a positive integer");
        return true;
      }
    }

    JsonVariantConst profilesParam = params["profiles"];
    if (profilesParam.isNull()) {
      profilesParam = params["mintProfiles"];
    }
    if (!readMintProfilesParam(profilesParam, next.mintProfiles, error)) {
      sendError("invalid_profiles", error);
      return true;
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
      sendError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be > 0 when autoMintEnabled=true");
      return true;
    }

    if (!state_.updateConfig(next, error)) {
      sendError("config_persist_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writeConfig(result, state_.snapshot().config);
    });
    return true;
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
        sendError("invalid_app_port", "appPort must be between 1 and 223");
        return true;
      }
      next.appPort = static_cast<uint8_t>(rawValue);
    }

    if (!params["defaultDataRate"].isNull()) {
      if (!readUint32Param(params["defaultDataRate"], rawValue) || rawValue > 15) {
        sendError("invalid_default_data_rate", "defaultDataRate must be between 0 and 15");
        return true;
      }
      next.defaultDataRate = static_cast<uint8_t>(rawValue);
    }

    if (!params["devEuiHex"].isNull() &&
        !readHexArrayParam(params["devEuiHex"], next.devEui, next.hasDevEui)) {
      sendError("invalid_dev_eui", "devEuiHex must be 16 hex characters");
      return true;
    }

    if (!params["joinEuiHex"].isNull() &&
        !readHexArrayParam(params["joinEuiHex"], next.joinEui, next.hasJoinEui)) {
      sendError("invalid_join_eui", "joinEuiHex must be 16 hex characters");
      return true;
    }

    if (!params["appKeyHex"].isNull() &&
        !readHexArrayParam(params["appKeyHex"], next.appKey, next.hasAppKey)) {
      sendError("invalid_app_key", "appKeyHex must be 32 hex characters");
      return true;
    }

    if (!state_.updateLoRaWanConfig(next, error)) {
      sendError("lorawan_config_persist_failed", error);
      return true;
    }

    lorawan_.reset();

    sendSuccess([&](JsonObject result) {
      writeLoRaWanConfig(result.createNestedObject("config"), state_.snapshot().loRaWan);
      writeLoRaWanStatus(result.createNestedObject("runtime"), lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "set_heltec_license") == 0) {
    if (!params["licenseHex"].is<const char *>()) {
      sendError("missing_license", "set_heltec_license requires params.licenseHex");
      return true;
    }

    HeltecLicenseConfig next = state_.snapshot().heltecLicense;
    String licenseHex;
    if (!normalizeHeltecLicenseInput(String(params["licenseHex"].as<const char *>()), licenseHex)) {
      sendError("invalid_license", "licenseHex must be 32 hex characters or Heltec license= / 0x...,0x...,0x...,0x... format");
      return true;
    }

    if (licenseHex.length() == 0) {
      next.hasLicense = false;
      next.value.fill(0);
    } else {
      if (!hexToBytes(licenseHex, next.value.data(), next.value.size())) {
        sendError("invalid_license", "licenseHex must be 32 hex characters or Heltec license= / 0x...,0x...,0x...,0x... format");
        return true;
      }
      next.hasLicense = true;
    }

    String error;
    if (!state_.updateHeltecLicense(next, error)) {
      sendError("heltec_license_persist_failed", error);
      return true;
    }

    lorawan_.reset();

    sendSuccess([&](JsonObject result) {
      writeHeltecLicense(result, state_.snapshot().heltecLicense, lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "generate_key") == 0) {
    const bool force = params["force"] | false;
    String error;

    if (!state_.generateKey(force, error)) {
      sendError("generate_key_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
    });
    return true;
  }

  if (strcmp(command, "get_public_key") == 0) {
    if (!state_.snapshot().hasKey) {
      sendError("missing_key", "Device key has not been generated yet");
      return true;
    }

    sendSuccess([&](JsonObject result) {
      result["deviceId"] = toHex(state_.snapshot().deviceId);
      result["publicKeyHex"] = toHex(state_.snapshot().publicKey);
    });
    return true;
  }

  if (strcmp(command, "export_backup") == 0) {
    if (!params["passphrase"].is<const char *>()) {
      sendError("missing_passphrase", "export_backup requires params.passphrase");
      return true;
    }

    BackupBlob backup;
    String error;
    if (!state_.exportBackup(String(params["passphrase"].as<const char *>()), backup, error)) {
      sendError("backup_export_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      result["version"] = backup.version;
      result["algorithm"] = backup.algorithm;
      result["saltHex"] = backup.saltHex;
      result["ivHex"] = backup.ivHex;
      result["ciphertextHex"] = backup.ciphertextHex;
      result["tagHex"] = backup.tagHex;
      result["deviceId"] = backup.deviceId;
    });
    return true;
  }

  if (strcmp(command, "import_backup") == 0) {
    if (!params["passphrase"].is<const char *>()) {
      sendError("missing_passphrase", "import_backup requires params.passphrase");
      return true;
    }

    JsonObjectConst backupObject = params["backup"].as<JsonObjectConst>();
    if (backupObject.isNull()) {
      sendError("missing_backup", "import_backup requires params.backup");
      return true;
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
    if (!state_.importBackup(backup, String(params["passphrase"].as<const char *>()), params["overwrite"] | false, error)) {
      sendError("backup_import_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
    });
    return true;
  }

  if (strcmp(command, "join_lorawan") == 0) {
    String error;
    if (!lorawan_.requestJoin(error)) {
      sendError("lorawan_join_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writeLoRaWanStatus(result, lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "lorawan_send") == 0) {
    if (!params["payloadHex"].is<const char *>()) {
      sendError("missing_payload", "lorawan_send requires params.payloadHex");
      return true;
    }

    uint8_t port = state_.snapshot().loRaWan.appPort;
    if (!params["port"].isNull()) {
      uint32_t rawPort = 0;
      if (!readUint32Param(params["port"], rawPort) || rawPort == 0 || rawPort > 223) {
        sendError("invalid_port", "port must be between 1 and 223");
        return true;
      }
      port = static_cast<uint8_t>(rawPort);
    }

    const bool confirmed =
        params["confirmed"].is<bool>() ? params["confirmed"].as<bool>() : state_.snapshot().loRaWan.confirmedUplink;
    const bool commitNonce = params["commitNonce"] | false;
    uint32_t nonceToCommit = state_.snapshot().nextNonce;
    if (commitNonce && !params["nonceToCommit"].isNull() && !readUint32Param(params["nonceToCommit"], nonceToCommit)) {
      sendError("invalid_nonce", "nonceToCommit must be a positive integer");
      return true;
    }

    String error;
    if (!lorawan_.queueUplink(
            String(params["payloadHex"].as<const char *>()),
            port,
            confirmed,
            commitNonce,
            nonceToCommit,
            error)) {
      sendError("lorawan_send_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      result["queued"] = true;
      result["port"] = port;
      result["confirmed"] = confirmed;
      result["commitNonce"] = commitNonce;
      result["nonceToCommit"] = commitNonce ? nonceToCommit : 0;
      result["nextNonce"] = state_.snapshot().nextNonce;
      writeLoRaWanStatus(result.createNestedObject("runtime"), lorawan_.status());
    });
    return true;
  }

  if (strcmp(command, "prepare_deploy") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        sendError("invalid_tick", error);
        return true;
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t maxSupply = 0;
    uint64_t limitPerMint = 0;
    if (!readUint64Param(params["maxSupply"], maxSupply) || !readUint64Param(params["limitPerMint"], limitPerMint)) {
      sendError("invalid_supply", "prepare_deploy requires maxSupply and limitPerMint");
      return true;
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildDeployPayload(state_.snapshot(), tick, maxSupply, limitPerMint, nonce, prepared, error)) {
      sendError("prepare_deploy_failed", error);
      return true;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      sendError("commit_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return true;
  }

  if (strcmp(command, "prepare_mint") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        sendError("invalid_tick", error);
        return true;
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t amount = current.defaultMintAmount;
    if (!params["amount"].isNull() && !readUint64Param(params["amount"], amount)) {
      sendError("invalid_amount", "amount must be a positive integer");
      return true;
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildMintPayload(state_.snapshot(), tick, amount, nonce, prepared, error)) {
      sendError("prepare_mint_failed", error);
      return true;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      sendError("commit_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return true;
  }

  if (strcmp(command, "prepare_transfer") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        sendError("invalid_tick", error);
        return true;
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t amount = 0;
    if (!readUint64Param(params["amount"], amount)) {
      sendError("invalid_amount", "prepare_transfer requires amount");
      return true;
    }

    if (!params["toDeviceId"].is<const char *>()) {
      sendError("missing_recipient", "prepare_transfer requires toDeviceId");
      return true;
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildTransferPayload(
            state_.snapshot(),
            tick,
            amount,
            String(params["toDeviceId"].as<const char *>()),
            nonce,
            prepared,
            error)) {
      sendError("prepare_transfer_failed", error);
      return true;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      sendError("commit_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return true;
  }

  if (strcmp(command, "prepare_config") == 0) {
    DeviceConfig next = state_.snapshot().config;
    String error;

    if (params["autoMintEnabled"].is<bool>()) {
      next.autoMintEnabled = params["autoMintEnabled"].as<bool>();
    }

    if (!params["autoMintIntervalSeconds"].isNull()) {
      if (!readUint32Param(params["autoMintIntervalSeconds"], next.autoMintIntervalSeconds)) {
        sendError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be a positive integer");
        return true;
      }
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildConfigPayload(state_.snapshot(), next, nonce, prepared, error)) {
      sendError("prepare_config_failed", error);
      return true;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.applyCommittedConfig(next, nonce, error)) {
      sendError("commit_failed", error);
      return true;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      writeConfig(result.createNestedObject("deviceConfig"), state_.snapshot().config);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return true;
  }

  sendError("unknown_command", String("Unknown command: ") + command);
  return true;
}

}  // namespace lora20

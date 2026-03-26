#include "serial_rpc.hpp"

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

void writeConfig(JsonObject target, const lora20::DeviceConfig &config) {
  target["autoMintEnabled"] = config.autoMintEnabled;
  target["autoMintIntervalSeconds"] = config.autoMintIntervalSeconds;
  target["defaultTick"] = lora20::tickToString(config.defaultTick);
  target["defaultMintAmount"] = u64ToString(config.defaultMintAmount);
}

void writeSnapshot(JsonObject target, const lora20::DeviceSnapshot &snapshot) {
  target["hasKey"] = snapshot.hasKey;
  target["deviceId"] = snapshot.hasKey ? lora20::toHex(snapshot.deviceId) : "";
  target["publicKeyHex"] = snapshot.hasKey ? lora20::toHex(snapshot.publicKey) : "";
  target["nextNonce"] = snapshot.nextNonce;
  writeConfig(target.createNestedObject("config"), snapshot.config);
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

}  // namespace

namespace lora20 {

SerialRpcServer::SerialRpcServer(Stream &serial, DeviceStateStore &state)
    : serial_(serial), state_(state) {}

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

  DynamicJsonDocument request(3072);
  const auto parseError = deserializeJson(request, line);
  if (parseError) {
    DynamicJsonDocument response(256);
    response["ok"] = false;
    JsonObject error = response.createNestedObject("error");
    error["code"] = "invalid_json";
    error["message"] = parseError.c_str();
    sendDocument(serial_, response);
    return;
  }

  const char *command = resolveCommand(request);
  const char *requestId = request["id"].is<const char *>() ? request["id"].as<const char *>() : "";
  JsonVariantConst params = request["params"];

  DynamicJsonDocument response(4096);
  if (requestId[0] != '\0') {
    response["id"] = requestId;
  }

  auto sendError = [&](const char *code, const String &message) {
    response.clear();
    if (requestId[0] != '\0') {
      response["id"] = requestId;
    }
    response["ok"] = false;
    JsonObject error = response.createNestedObject("error");
    error["code"] = code;
    error["message"] = message;
    sendDocument(serial_, response);
  };

  auto sendSuccess = [&](const std::function<void(JsonObject)> &fillResult) {
    response.clear();
    if (requestId[0] != '\0') {
      response["id"] = requestId;
    }
    response["ok"] = true;
    JsonObject result = response.createNestedObject("result");
    fillResult(result);
    sendDocument(serial_, response);
  };

  if (strlen(command) == 0) {
    sendError("missing_command", "Request must contain command or method");
    return;
  }

  if (strcmp(command, "ping") == 0) {
    sendSuccess([&](JsonObject result) {
      result["firmware"] = kFirmwareName;
      result["version"] = LORA20_FW_VERSION;
      result["uptimeMs"] = millis();
    });
    return;
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
    });
    return;
  }

  if (strcmp(command, "get_config") == 0) {
    sendSuccess([&](JsonObject result) {
      writeConfig(result, state_.snapshot().config);
    });
    return;
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
        return;
      }
    }

    if (params["defaultTick"].is<const char *>()) {
      if (!normalizeTick(String(params["defaultTick"].as<const char *>()), next.defaultTick, error)) {
        sendError("invalid_tick", error);
        return;
      }
    }

    if (!params["defaultMintAmount"].isNull()) {
      if (!readUint64Param(params["defaultMintAmount"], next.defaultMintAmount) || next.defaultMintAmount == 0) {
        sendError("invalid_default_mint_amount", "defaultMintAmount must be a positive integer");
        return;
      }
    }

    if (next.autoMintEnabled && next.autoMintIntervalSeconds == 0) {
      sendError("invalid_auto_mint_interval", "autoMintIntervalSeconds must be > 0 when autoMintEnabled=true");
      return;
    }

    if (!state_.updateConfig(next, error)) {
      sendError("config_persist_failed", error);
      return;
    }

    sendSuccess([&](JsonObject result) {
      writeConfig(result, state_.snapshot().config);
    });
    return;
  }

  if (strcmp(command, "generate_key") == 0) {
    const bool force = params["force"] | false;
    String error;

    if (!state_.generateKey(force, error)) {
      sendError("generate_key_failed", error);
      return;
    }

    sendSuccess([&](JsonObject result) {
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
    });
    return;
  }

  if (strcmp(command, "get_public_key") == 0) {
    if (!state_.snapshot().hasKey) {
      sendError("missing_key", "Device key has not been generated yet");
      return;
    }

    sendSuccess([&](JsonObject result) {
      result["deviceId"] = toHex(state_.snapshot().deviceId);
      result["publicKeyHex"] = toHex(state_.snapshot().publicKey);
    });
    return;
  }

  if (strcmp(command, "export_backup") == 0) {
    if (!params["passphrase"].is<const char *>()) {
      sendError("missing_passphrase", "export_backup requires params.passphrase");
      return;
    }

    BackupBlob backup;
    String error;
    if (!state_.exportBackup(String(params["passphrase"].as<const char *>()), backup, error)) {
      sendError("backup_export_failed", error);
      return;
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
    return;
  }

  if (strcmp(command, "import_backup") == 0) {
    if (!params["passphrase"].is<const char *>()) {
      sendError("missing_passphrase", "import_backup requires params.passphrase");
      return;
    }

    JsonObjectConst backupObject = params["backup"].as<JsonObjectConst>();
    if (backupObject.isNull()) {
      sendError("missing_backup", "import_backup requires params.backup");
      return;
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
      return;
    }

    sendSuccess([&](JsonObject result) {
      writeSnapshot(result.createNestedObject("device"), state_.snapshot());
    });
    return;
  }

  if (strcmp(command, "prepare_deploy") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        sendError("invalid_tick", error);
        return;
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t maxSupply = 0;
    uint64_t limitPerMint = 0;
    if (!readUint64Param(params["maxSupply"], maxSupply) || !readUint64Param(params["limitPerMint"], limitPerMint)) {
      sendError("invalid_supply", "prepare_deploy requires maxSupply and limitPerMint");
      return;
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildDeployPayload(state_.snapshot(), tick, maxSupply, limitPerMint, nonce, prepared, error)) {
      sendError("prepare_deploy_failed", error);
      return;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      sendError("commit_failed", error);
      return;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return;
  }

  if (strcmp(command, "prepare_mint") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        sendError("invalid_tick", error);
        return;
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t amount = current.defaultMintAmount;
    if (!params["amount"].isNull() && !readUint64Param(params["amount"], amount)) {
      sendError("invalid_amount", "amount must be a positive integer");
      return;
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildMintPayload(state_.snapshot(), tick, amount, nonce, prepared, error)) {
      sendError("prepare_mint_failed", error);
      return;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      sendError("commit_failed", error);
      return;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return;
  }

  if (strcmp(command, "prepare_transfer") == 0) {
    DeviceConfig current = state_.snapshot().config;
    char tick[5];
    String error;
    if (params["tick"].is<const char *>()) {
      if (!normalizeTick(String(params["tick"].as<const char *>()), tick, error)) {
        sendError("invalid_tick", error);
        return;
      }
    } else {
      memcpy(tick, current.defaultTick, 5);
    }

    uint64_t amount = 0;
    if (!readUint64Param(params["amount"], amount)) {
      sendError("invalid_amount", "prepare_transfer requires amount");
      return;
    }

    if (!params["toDeviceId"].is<const char *>()) {
      sendError("missing_recipient", "prepare_transfer requires toDeviceId");
      return;
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
      return;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.commitNonce(nonce, error)) {
      sendError("commit_failed", error);
      return;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return;
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
        return;
      }
    }

    PreparedPayload prepared;
    const uint32_t nonce = state_.snapshot().nextNonce;
    if (!buildConfigPayload(state_.snapshot(), next, nonce, prepared, error)) {
      sendError("prepare_config_failed", error);
      return;
    }

    prepared.committed = params["commit"] | false;
    if (prepared.committed && !state_.applyCommittedConfig(next, nonce, error)) {
      sendError("commit_failed", error);
      return;
    }

    sendSuccess([&](JsonObject result) {
      writePreparedPayload(result, prepared);
      writeConfig(result.createNestedObject("deviceConfig"), state_.snapshot().config);
      result["nextNonce"] = state_.snapshot().nextNonce;
    });
    return;
  }

  sendError("unknown_command", String("Unknown command: ") + command);
}

void SerialRpcServer::sendBootEvent() {
  DynamicJsonDocument boot(512);
  boot["type"] = "boot";
  boot["firmware"] = kFirmwareName;
  boot["version"] = LORA20_FW_VERSION;
  boot["hasKey"] = state_.snapshot().hasKey;
  boot["deviceId"] = state_.snapshot().hasKey ? toHex(state_.snapshot().deviceId) : "";
  boot["nextNonce"] = state_.snapshot().nextNonce;
  sendDocument(serial_, boot);
}

}  // namespace lora20

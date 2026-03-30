#pragma once

#include <Arduino.h>
#include <LoRaWan_APP.h>

#include <vector>

#include "lora20_device.hpp"

namespace lora20 {

struct LoRaWanRuntimeStatus {
  bool hardwareReady = false;
  bool initialized = false;
  bool configured = false;
  bool joining = false;
  bool joined = false;
  bool queuePending = false;
  bool lastSendAccepted = false;
  uint8_t activePort = 1;
  size_t queuedPayloadSize = 0;
  size_t lastAcceptedPayloadSize = 0;
  uint32_t lastJoinAttemptMs = 0;
  uint32_t lastAcceptedSendMs = 0;
  int16_t lastDownlinkRssi = 0;
  int8_t lastDownlinkSnr = 0;
  uint8_t lastDownlinkPort = 0;
  String region = "EU868";
  String chipIdHex;
  String lastEvent;
  String lastError;
  String lastDownlinkHex;
};

class LoRaWanClient {
 public:
  explicit LoRaWanClient(DeviceStateStore &state);

  void poll();
  void reset();

  bool requestJoin(String &error);
  bool queueUplink(const String &payloadHex,
                   uint8_t port,
                   bool confirmed,
                   bool commitNonce,
                   uint32_t nonceToCommit,
                   String &error);

  const LoRaWanRuntimeStatus &status() const;

  void handleAck();
  void handleDownlink(const McpsIndication_t &indication);
  void handleDeviceTimeUpdated();

 private:
  bool ensureInitialized(String &error);
  bool ensureHardwareReady(String &error);
  bool applyCurrentConfig(String &error);
  bool isConfigured(const DeviceSnapshot &snapshot) const;
  bool trySendQueued(String &error);
  void refreshJoinState();
  String chipIdHex() const;

  DeviceStateStore &state_;
  LoRaWanRuntimeStatus status_;
  bool hardwareReady_ = false;
  bool initialized_ = false;
  bool joinRequested_ = false;
  bool joinStarted_ = false;
  bool queuedConfirmed_ = false;
  bool queuedCommitNonce_ = false;
  uint32_t nonceToCommit_ = 0;
  uint32_t nextSendAttemptMs_ = 0;
  uint8_t queuedPort_ = 1;
  std::vector<uint8_t> queuedPayload_;
};

}  // namespace lora20

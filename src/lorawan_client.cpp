#include "lorawan_client.hpp"

#include <LoRaWan_APP.h>
#include <radio/radio.h>

#include <cstring>

extern uint32_t storedlicense[4];
extern int calRTC(unsigned long *license);
extern void writelicense(unsigned long *license, unsigned char board_type);
extern void getLicenseAddress(unsigned char board_type);

namespace {

constexpr uint16_t kDefaultChannelsMask[6] = {0x00FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};

lora20::LoRaWanClient *g_activeClient = nullptr;

String chipIdToHex(uint64_t chipId) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%04llx%08llx",
           static_cast<unsigned long long>((chipId >> 32) & 0xFFFFULL),
           static_cast<unsigned long long>(chipId & 0xFFFFFFFFULL));
  return String(buffer);
}

void populateAutoDevEui(uint8_t target[8]) {
  const uint64_t chipId = ESP.getEfuseMac();
  const uint32_t high = static_cast<uint32_t>(chipId >> 32);
  const uint32_t low = static_cast<uint32_t>(chipId & 0xFFFFFFFFULL);

  for (int index = 0; index < 8; ++index) {
    if (index < 4) {
      target[index] = static_cast<uint8_t>((low >> (8 * (3 - index))) & 0xFF);
    } else {
      target[index] = static_cast<uint8_t>((high >> (8 * (7 - index))) & 0xFF);
    }
  }
}

void resolveEffectiveDevEui(const lora20::DeviceSnapshot &snapshot, uint8_t target[8]) {
  if (snapshot.loRaWan.hasDevEui) {
    std::memcpy(target, snapshot.loRaWan.devEui.data(), snapshot.loRaWan.devEui.size());
    return;
  }

  if (snapshot.loRaWan.autoDevEui) {
    populateAutoDevEui(target);
    return;
  }

  std::memset(target, 0, 8);
}

bool isValidPort(uint8_t port) {
  return port > 0 && port < 224;
}

void applyStoredLicense(const lora20::HeltecLicenseConfig &config) {
  for (size_t wordIndex = 0; wordIndex < 4; ++wordIndex) {
    uint32_t word = 0;
    if (config.hasLicense) {
      word = (static_cast<uint32_t>(config.value[wordIndex * 4]) << 24) |
             (static_cast<uint32_t>(config.value[wordIndex * 4 + 1]) << 16) |
             (static_cast<uint32_t>(config.value[wordIndex * 4 + 2]) << 8) |
             static_cast<uint32_t>(config.value[wordIndex * 4 + 3]);
    }
    storedlicense[wordIndex] = word;
  }
}

}  // namespace

uint8_t devEui[8] = {0};
uint8_t appEui[8] = {0};
uint8_t appKey[16] = {0};
uint8_t nwkSKey[16] = {0};
uint8_t appSKey[16] = {0};
uint32_t devAddr = 0;
bool overTheAirActivation = true;
bool loraWanAdr = true;
bool isTxConfirmed = false;
uint8_t appPort = 1;
uint32_t appTxDutyCycle = 15000;
DeviceClass_t loraWanClass = CLASS_A;
uint8_t confirmedNbTrials = 1;
bool keepNet = false;
uint16_t userChannelsMask[6] = {0x00FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;

extern "C" void downLinkAckHandle() {
  if (g_activeClient != nullptr) {
    g_activeClient->handleAck();
  }
}

extern "C" void downLinkDataHandle(McpsIndication_t *mcpsIndication) {
  if (g_activeClient != nullptr && mcpsIndication != nullptr) {
    g_activeClient->handleDownlink(*mcpsIndication);
  }
}

extern "C" void dev_time_updated() {
  if (g_activeClient != nullptr) {
    g_activeClient->handleDeviceTimeUpdated();
  }
}

namespace lora20 {

String effectiveDevEuiHex(const DeviceSnapshot &snapshot) {
  if (!snapshot.loRaWan.autoDevEui && !snapshot.loRaWan.hasDevEui) {
    return "";
  }

  uint8_t effectiveDevEui[8] = {0};
  resolveEffectiveDevEui(snapshot, effectiveDevEui);
  return toHex(effectiveDevEui, sizeof(effectiveDevEui));
}

String devEuiSourceLabel(const DeviceSnapshot &snapshot) {
  if (snapshot.loRaWan.hasDevEui) {
    return "stored";
  }
  if (snapshot.loRaWan.autoDevEui) {
    return "chip";
  }
  return "none";
}

uint8_t effectiveJoinDataRate(const DeviceSnapshot &snapshot) {
  // EU868 join accepts are currently the weakest part of this deployment path.
  // Force the most robust join DR, then let ADR optimize uplinks after join.
  if (ACTIVE_REGION == LORAMAC_REGION_EU868) {
    return 0;
  }
  return snapshot.loRaWan.defaultDataRate;
}

LoRaWanClient::LoRaWanClient(DeviceStateStore &state) : state_(state) {
  status_.chipIdHex = chipIdHex();
  status_.effectiveJoinDataRate = effectiveJoinDataRate(state_.snapshot());
  status_.effectiveDevEuiHex = effectiveDevEuiHex(state_.snapshot());
  status_.devEuiSource = devEuiSourceLabel(state_.snapshot());
}

void LoRaWanClient::poll() {
  status_.configured = isConfigured(state_.snapshot());
  status_.queuePending = !queuedPayload_.empty();

  if (!hardwareReady_ && (joinRequested_ || !queuedPayload_.empty())) {
    String error;
    ensureHardwareReady(error);
    if (error.length() > 0) {
      status_.lastError = error;
      return;
    }
  }

  if (!initialized_ && (joinRequested_ || !queuedPayload_.empty())) {
    String error;
    ensureInitialized(error);
    if (error.length() > 0) {
      status_.lastError = error;
      return;
    }
  }

  if (!initialized_) {
    refreshJoinState();
    return;
  }

  if (joinRequested_ && !joinStarted_) {
    status_.lastEvent = "join_starting";
    status_.lastError = "";
    Serial.println("[lorawan] starting join");
    LoRaWAN.join();
    joinStarted_ = true;
    deviceState = DEVICE_STATE_SLEEP;
    status_.joining = true;
    status_.lastJoinAttemptMs = millis();
    status_.lastEvent = "joining";
  }

  Mcu.timerhandler();
  Radio.IrqProcess();
  refreshJoinState();

  if (status_.joined && deviceState == DEVICE_STATE_SEND && queuedPayload_.empty()) {
    deviceState = DEVICE_STATE_SLEEP;
  }

  if (!queuedPayload_.empty()) {
    String error;
    trySendQueued(error);
    if (error.length() > 0) {
      status_.lastError = error;
    }
  }
}

void LoRaWanClient::reset() {
  initialized_ = false;
  joinRequested_ = false;
  joinStarted_ = false;
  queuedConfirmed_ = false;
  queuedCommitNonce_ = false;
  nonceToCommit_ = 0;
  nextSendAttemptMs_ = 0;
  queuedPort_ = 1;
  queuedPayload_.clear();
  status_.initialized = false;
  status_.joining = false;
  status_.joined = false;
  status_.queuePending = false;
  status_.lastSendAccepted = false;
  status_.lastAcceptedPayloadSize = 0;
  status_.queuedPayloadSize = 0;
  status_.lastEvent = "lorawan_reset";
  status_.lastError = "";
  deviceState = DEVICE_STATE_INIT;
}

bool LoRaWanClient::requestJoin(String &error, bool forceRestart) {
  status_.configured = isConfigured(state_.snapshot());
  if (!status_.configured) {
    error = "LoRaWAN OTAA configuration is incomplete";
    return false;
  }

  refreshJoinState();
  if (status_.joined) {
    status_.lastEvent = "already_joined";
    status_.lastError = "";
    return true;
  }

  if (forceRestart && (joinRequested_ || joinStarted_ || initialized_ || hardwareReady_)) {
    reset();
    status_.configured = isConfigured(state_.snapshot());
    status_.lastEvent = "join_restart_requested";
    status_.lastError = "";
  } else if (joinRequested_ || joinStarted_) {
    status_.lastEvent = "join_already_in_progress";
    status_.lastError = "";
    return true;
  }

  joinRequested_ = true;
  status_.joining = true;
  status_.lastError = "";
  if (joinStarted_) {
    status_.lastEvent = "joining";
  } else if (!hardwareReady_) {
    status_.lastEvent = "join_queued_hardware";
  } else if (!initialized_) {
    status_.lastEvent = "join_queued_init";
  } else {
    status_.lastEvent = "join_queued_start";
  }

  return true;
}

bool LoRaWanClient::queueUplink(const String &payloadHex,
                                uint8_t port,
                                bool confirmed,
                                bool commitNonce,
                                uint32_t nonceToCommit,
                                String &error) {
  if (!isValidPort(port)) {
    error = "LoRaWAN port must be between 1 and 223";
    return false;
  }

  if (!isConfigured(state_.snapshot())) {
    error = "LoRaWAN OTAA configuration is incomplete";
    return false;
  }

  if (!queuedPayload_.empty()) {
    error = "A LoRaWAN uplink is already queued";
    return false;
  }

  if (payloadHex.length() == 0 || (payloadHex.length() % 2) != 0) {
    error = "payloadHex must be non-empty even-length hex";
    return false;
  }

  const size_t payloadLength = payloadHex.length() / 2;
  if (payloadLength > LORAWAN_APP_DATA_MAX_SIZE) {
    error = "payloadHex exceeds the LoRaWAN application payload buffer";
    return false;
  }

  queuedPayload_.assign(payloadLength, 0);
  if (!hexToBytes(payloadHex, queuedPayload_.data(), queuedPayload_.size())) {
    queuedPayload_.clear();
    error = "payloadHex contains invalid hex";
    return false;
  }

  if (commitNonce) {
    if (nonceToCommit == 0) {
      queuedPayload_.clear();
      error = "nonceToCommit is required when commitNonce=true";
      return false;
    }

    if (nonceToCommit != state_.snapshot().nextNonce) {
      queuedPayload_.clear();
      error = "nonceToCommit must match the device nextNonce";
      return false;
    }
  }

  queuedPort_ = port;
  queuedConfirmed_ = confirmed;
  queuedCommitNonce_ = commitNonce;
  nonceToCommit_ = nonceToCommit;
  nextSendAttemptMs_ = 0;
  status_.queuePending = true;
  status_.queuedPayloadSize = queuedPayload_.size();
  status_.activePort = queuedPort_;
  status_.lastSendAccepted = false;
  status_.lastError = "";

  if (!status_.joined) {
    if (!requestJoin(error)) {
      queuedPayload_.clear();
      status_.queuePending = false;
      status_.queuedPayloadSize = 0;
      return false;
    }

    status_.lastEvent = "uplink_queued_waiting_for_join";
    return true;
  }

  trySendQueued(error);
  return error.length() == 0;
}

const LoRaWanRuntimeStatus &LoRaWanClient::status() const {
  return status_;
}

bool LoRaWanClient::takeAcceptedPayload(std::vector<uint8_t> &out) {
  if (!hasAcceptedPayload_) {
    return false;
  }
  out = lastAcceptedPayload_;
  lastAcceptedPayload_.clear();
  hasAcceptedPayload_ = false;
  return true;
}

void LoRaWanClient::handleAck() {
  status_.lastEvent = "downlink_ack";
}

void LoRaWanClient::handleDownlink(const McpsIndication_t &indication) {
  status_.lastDownlinkRssi = indication.Rssi;
  status_.lastDownlinkSnr = indication.Snr;
  status_.lastDownlinkPort = indication.Port;
  status_.lastDownlinkHex =
      indication.BufferSize > 0 ? toHex(indication.Buffer, indication.BufferSize) : "";
  status_.lastEvent = "downlink_data";
}

void LoRaWanClient::handleDeviceTimeUpdated() {
  status_.lastEvent = "device_time_updated";
}

bool LoRaWanClient::ensureInitialized(String &error) {
  if (initialized_) {
    return true;
  }

  if (!ensureHardwareReady(error)) {
    return false;
  }

  status_.lastEvent = "lorawan_config_apply";
  if (!applyCurrentConfig(error)) {
    status_.lastEvent = "lorawan_config_failed";
    return false;
  }

  status_.lastEvent = "lorawan_mac_init";
  Serial.println("[lorawan] LoRaWAN.init enter");
  LoRaWAN.init(loraWanClass, loraWanRegion);
  const uint8_t joinDataRate = effectiveJoinDataRate(state_.snapshot());
  LoRaWAN.setDefaultDR(joinDataRate);
  status_.effectiveJoinDataRate = joinDataRate;
  Serial.printf("[lorawan] using JoinDR=%u requestedDR=%u\r\n",
                static_cast<unsigned>(joinDataRate),
                static_cast<unsigned>(state_.snapshot().loRaWan.defaultDataRate));
  Serial.println("[lorawan] LoRaWAN.init ok");

  initialized_ = true;
  status_.initialized = true;
  status_.lastEvent = "lorawan_initialized";
  status_.lastError = "";
  return true;
}

bool LoRaWanClient::ensureHardwareReady(String &error) {
  if (hardwareReady_) {
    return true;
  }

  if (!state_.snapshot().heltecLicense.hasLicense) {
    error = "Heltec vendor license is missing; set it with set_heltec_license before join_lorawan";
    status_.lastEvent = "hw_license_missing";
    status_.lastError = error;
    return false;
  }

  status_.lastEvent = "hw_license_apply";
  Serial.println("[lorawan] applying Heltec license");
  applyStoredLicense(state_.snapshot().heltecLicense);
  unsigned long *vendorLicense = reinterpret_cast<unsigned long *>(storedlicense);
  status_.lastEvent = "hw_license_check";
  Serial.println("[lorawan] validating Heltec license");
  if (calRTC(vendorLicense) != 1) {
    error = "Heltec vendor license is invalid for this board chip ID";
    status_.lastEvent = "hw_license_invalid";
    status_.lastError = error;
    return false;
  }

  status_.lastEvent = "hw_license_write";
  Serial.println("[lorawan] writing Heltec license");
  getLicenseAddress(HELTEC_BOARD);
  writelicense(vendorLicense, HELTEC_BOARD);
  status_.lastEvent = "hw_mcu_begin";
  Serial.println("[lorawan] Mcu.begin enter");
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
  Serial.println("[lorawan] Mcu.begin ok");
  hardwareReady_ = true;
  status_.hardwareReady = true;
  g_activeClient = this;
  status_.lastEvent = "hw_ready";
  status_.lastError = "";
  return true;
}

bool LoRaWanClient::applyCurrentConfig(String &error) {
  const DeviceSnapshot &snapshot = state_.snapshot();
  if (!isConfigured(snapshot)) {
    error = "LoRaWAN OTAA configuration is incomplete";
    return false;
  }

  std::memcpy(userChannelsMask, kDefaultChannelsMask, sizeof(userChannelsMask));
  std::memset(nwkSKey, 0, sizeof(nwkSKey));
  std::memset(appSKey, 0, sizeof(appSKey));
  devAddr = 0;
  keepNet = false;
  overTheAirActivation = true;
  loraWanClass = CLASS_A;
  loraWanAdr = snapshot.loRaWan.adr;
  isTxConfirmed = snapshot.loRaWan.confirmedUplink;
  appPort = snapshot.loRaWan.appPort;
  confirmedNbTrials = snapshot.loRaWan.confirmedUplink ? 2 : 1;
  appTxDutyCycle = 15000;
  loraWanRegion = ACTIVE_REGION;

  resolveEffectiveDevEui(snapshot, devEui);

  std::memcpy(appEui, snapshot.loRaWan.joinEui.data(), snapshot.loRaWan.joinEui.size());
  std::memcpy(appKey, snapshot.loRaWan.appKey.data(), snapshot.loRaWan.appKey.size());
  status_.effectiveDevEuiHex = toHex(devEui, sizeof(devEui));
  status_.devEuiSource = devEuiSourceLabel(snapshot);
  Serial.printf("[lorawan] using DevEUI=%s source=%s\r\n",
                status_.effectiveDevEuiHex.c_str(),
                status_.devEuiSource.c_str());
  Serial.printf("[lorawan] using JoinEUI=%s appPort=%u dr=%u adr=%u confirmed=%u\r\n",
                toHex(appEui, sizeof(appEui)).c_str(),
                static_cast<unsigned>(appPort),
                static_cast<unsigned>(snapshot.loRaWan.defaultDataRate),
                snapshot.loRaWan.adr ? 1U : 0U,
                snapshot.loRaWan.confirmedUplink ? 1U : 0U);
  status_.region = "EU868";
  status_.effectiveJoinDataRate = effectiveJoinDataRate(snapshot);
  status_.configured = true;
  return true;
}

bool LoRaWanClient::isConfigured(const DeviceSnapshot &snapshot) const {
  return (snapshot.loRaWan.autoDevEui || snapshot.loRaWan.hasDevEui) &&
         snapshot.loRaWan.hasJoinEui &&
         snapshot.loRaWan.hasAppKey &&
         isValidPort(snapshot.loRaWan.appPort);
}

bool LoRaWanClient::trySendQueued(String &error) {
  if (queuedPayload_.empty() || !initialized_) {
    return false;
  }

  refreshJoinState();
  if (!status_.joined) {
    return false;
  }

  if (millis() < nextSendAttemptMs_) {
    return false;
  }

  appPort = queuedPort_;
  isTxConfirmed = queuedConfirmed_;
  appDataSize = static_cast<uint8_t>(queuedPayload_.size());
  std::memcpy(appData, queuedPayload_.data(), queuedPayload_.size());

  if (SendFrame()) {
    nextSendAttemptMs_ = millis() + 1000;
    status_.lastEvent = "uplink_retry_scheduled";
    status_.lastSendAccepted = false;
    return false;
  }

  if (queuedCommitNonce_) {
    if (!state_.commitNonce(nonceToCommit_, error)) {
      status_.lastEvent = "uplink_nonce_commit_failed";
      status_.lastSendAccepted = true;
      return false;
    }
  }

  status_.lastEvent = "uplink_enqueued";
  status_.lastError = "";
  status_.lastSendAccepted = true;
  status_.lastAcceptedSendMs = millis();
  status_.lastAcceptedPayloadSize = queuedPayload_.size();
  lastAcceptedPayload_ = queuedPayload_;
  hasAcceptedPayload_ = true;
  status_.queuedPayloadSize = 0;
  status_.queuePending = false;
  queuedPayload_.clear();
  queuedConfirmed_ = false;
  queuedCommitNonce_ = false;
  nonceToCommit_ = 0;
  nextSendAttemptMs_ = 0;
  return true;
}

void LoRaWanClient::refreshJoinState() {
  status_.chipIdHex = chipIdHex();
  status_.effectiveJoinDataRate = effectiveJoinDataRate(state_.snapshot());
  status_.effectiveDevEuiHex = effectiveDevEuiHex(state_.snapshot());
  status_.devEuiSource = devEuiSourceLabel(state_.snapshot());
  status_.hardwareReady = hardwareReady_;
  status_.initialized = initialized_;
  status_.configured = isConfigured(state_.snapshot());
  status_.queuePending = !queuedPayload_.empty();

  if (!initialized_) {
    status_.joined = false;
    status_.joining = joinRequested_ || joinStarted_;
    return;
  }

  MibRequestConfirm_t mibRequest{};
  mibRequest.Type = MIB_NETWORK_JOINED;
  if (LoRaMacMibGetRequestConfirm(&mibRequest) == LORAMAC_STATUS_OK) {
    status_.joined = mibRequest.Param.IsNetworkJoined;
  } else {
    status_.joined = false;
  }

  status_.joining = joinRequested_ && !status_.joined;

  if (status_.joined) {
    joinRequested_ = false;
    joinStarted_ = false;
  }
}

String LoRaWanClient::chipIdHex() const {
  return chipIdToHex(ESP.getEfuseMac());
}

}  // namespace lora20

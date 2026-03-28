#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>

#include "lora20_device.hpp"
#include "lorawan_client.hpp"
#include "rpc_processor.hpp"
#include "serial_rpc.hpp"
#include "wifi_bridge.hpp"
#include "ble_bridge.hpp"

lora20::DeviceStateStore g_state;
lora20::LoRaWanClient g_lorawan(g_state);
lora20::RpcProcessor g_rpcProcessor(g_state, g_lorawan);
lora20::SerialRpcServer g_rpc(Serial, g_rpcProcessor);
lora20::WifiBridge g_wifiBridge(g_rpcProcessor);
lora20::BleBridge g_bleBridge(g_rpcProcessor);
bool g_ready = false;
bool g_autoMintArmed = false;
unsigned long g_nextAutoMintAtMs = 0;
unsigned long g_lastAutoMintAttemptMs = 0;
uint32_t g_lastAutoMintIntervalSeconds = 0;
size_t g_autoMintCursor = 0;
std::vector<lora20::MintProfile> g_lastAutoMintProfiles;
String g_lastAutoMintStage;
String g_lastAutoMintError;
unsigned long g_lastAutoMintEventAtMs = 0;

String mintAmountToString(uint64_t amount) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(amount));
  return String(buffer);
}

void emitAutoMintEvent(const char *stage,
                       const lora20::MintProfile *profile,
                       uint32_t nonce,
                       const String &error,
                       size_t activeProfileCount,
                       unsigned long nowMs,
                       unsigned long nextAtMs,
                       bool force) {
  const String stageValue = stage != nullptr ? String(stage) : String("unknown");
  const String errorValue = error;
  if (!force &&
      stageValue == g_lastAutoMintStage &&
      errorValue == g_lastAutoMintError &&
      (nowMs - g_lastAutoMintEventAtMs) < 30000UL) {
    return;
  }

  DynamicJsonDocument event(640);
  event["type"] = "auto_mint";
  event["stage"] = stageValue;
  event["enabled"] = g_state.snapshot().config.autoMintEnabled;
  event["intervalSeconds"] = g_state.snapshot().config.autoMintIntervalSeconds;
  event["activeProfiles"] = activeProfileCount;
  event["cursor"] = g_autoMintCursor;
  event["nowMs"] = nowMs;
  event["nextAtMs"] = nextAtMs;
  event["nextInMs"] = (nextAtMs > nowMs) ? (nextAtMs - nowMs) : 0;
  event["nextNonce"] = g_state.snapshot().nextNonce;
  if (profile != nullptr) {
    event["tick"] = lora20::tickToString(profile->tick);
    event["amount"] = mintAmountToString(profile->amount);
  }
  if (nonce != 0) {
    event["nonce"] = nonce;
  }
  if (errorValue.length() > 0) {
    event["error"] = errorValue;
  }

  serializeJson(event, Serial);
  Serial.println();

  g_lastAutoMintStage = stageValue;
  g_lastAutoMintError = errorValue;
  g_lastAutoMintEventAtMs = nowMs;
}

bool sameMintProfile(const lora20::MintProfile &left, const lora20::MintProfile &right) {
  return left.amount == right.amount && left.enabled == right.enabled && std::memcmp(left.tick, right.tick, 5) == 0;
}

void collectActiveAutoMintProfiles(const lora20::DeviceSnapshot &snapshot, std::vector<lora20::MintProfile> &profiles) {
  profiles.clear();

  if (snapshot.config.mintProfiles.empty()) {
    lora20::MintProfile fallback;
    std::memcpy(fallback.tick, snapshot.config.defaultTick, sizeof(fallback.tick));
    fallback.amount = snapshot.config.defaultMintAmount;
    fallback.enabled = true;
    profiles.push_back(fallback);
    return;
  }

  for (const auto &profile : snapshot.config.mintProfiles) {
    if (profile.enabled) {
      profiles.push_back(profile);
    }
  }
}

bool autoMintProfileChanged(const lora20::DeviceSnapshot &snapshot, const std::vector<lora20::MintProfile> &activeProfiles) {
  if (!g_autoMintArmed || g_lastAutoMintIntervalSeconds != snapshot.config.autoMintIntervalSeconds) {
    return true;
  }

  if (g_lastAutoMintProfiles.size() != activeProfiles.size()) {
    return true;
  }

  for (size_t index = 0; index < activeProfiles.size(); ++index) {
    if (!sameMintProfile(g_lastAutoMintProfiles[index], activeProfiles[index])) {
      return true;
    }
  }

  return false;
}

void armAutoMintSchedule(const lora20::DeviceSnapshot &snapshot,
                         const std::vector<lora20::MintProfile> &activeProfiles,
                         unsigned long nowMs,
                         bool immediateFirstTick) {
  const unsigned long intervalMs = static_cast<unsigned long>(snapshot.config.autoMintIntervalSeconds) * 1000UL;
  g_autoMintArmed = true;
  g_nextAutoMintAtMs = nowMs + (immediateFirstTick ? 1500UL : intervalMs);
  g_lastAutoMintIntervalSeconds = snapshot.config.autoMintIntervalSeconds;
  g_lastAutoMintProfiles = activeProfiles;
  if (g_autoMintCursor >= g_lastAutoMintProfiles.size()) {
    g_autoMintCursor = 0;
  }
}

void resetAutoMintSchedule() {
  g_autoMintArmed = false;
  g_nextAutoMintAtMs = 0;
  g_lastAutoMintAttemptMs = 0;
  g_lastAutoMintIntervalSeconds = 0;
  g_autoMintCursor = 0;
  g_lastAutoMintProfiles.clear();
}

void serviceAutoMint() {
  const lora20::DeviceSnapshot &snapshot = g_state.snapshot();
  const unsigned long nowMs = millis();
  std::vector<lora20::MintProfile> activeProfiles;

  if (!snapshot.hasKey || !snapshot.config.autoMintEnabled || snapshot.config.autoMintIntervalSeconds == 0) {
    if (g_autoMintArmed) {
      emitAutoMintEvent(
          "disabled",
          nullptr,
          0,
          "auto-mint disabled or incomplete config",
          0,
          nowMs,
          0,
          true);
    }
    resetAutoMintSchedule();
    return;
  }

  collectActiveAutoMintProfiles(snapshot, activeProfiles);
  if (activeProfiles.empty()) {
    if (g_autoMintArmed) {
      emitAutoMintEvent(
          "no_active_profiles",
          nullptr,
          0,
          "auto-mint has no enabled profiles",
          0,
          nowMs,
          0,
          true);
    }
    resetAutoMintSchedule();
    return;
  }

  if (autoMintProfileChanged(snapshot, activeProfiles)) {
    const bool immediateFirstTick = !g_autoMintArmed;
    g_autoMintCursor = 0;
    armAutoMintSchedule(snapshot, activeProfiles, nowMs, immediateFirstTick);
    emitAutoMintEvent(
        "armed",
        activeProfiles.empty() ? nullptr : &activeProfiles.front(),
        0,
        "",
        activeProfiles.size(),
        nowMs,
        g_nextAutoMintAtMs,
        true);
    return;
  }

  if (nowMs < g_nextAutoMintAtMs) {
    return;
  }

  if (g_lastAutoMintAttemptMs != 0 && (nowMs - g_lastAutoMintAttemptMs) < 1000UL) {
    return;
  }

  if (g_autoMintCursor >= activeProfiles.size()) {
    g_autoMintCursor = 0;
  }

  const lora20::MintProfile &profile = activeProfiles[g_autoMintCursor];
  lora20::PreparedPayload prepared;
  String error;
  const uint32_t nonce = snapshot.nextNonce;
  if (!lora20::buildMintPayload(snapshot, profile.tick, profile.amount, nonce, prepared, error)) {
    g_lastAutoMintAttemptMs = nowMs;
    g_nextAutoMintAtMs = nowMs + 5000UL;
    emitAutoMintEvent(
        "prepare_failed",
        &profile,
        nonce,
        error,
        activeProfiles.size(),
        nowMs,
        g_nextAutoMintAtMs,
        false);
    return;
  }

  if (!g_lorawan.queueUplink(lora20::toHex(prepared.payload),
                             snapshot.loRaWan.appPort,
                             snapshot.loRaWan.confirmedUplink,
                             true,
                             nonce,
                             error)) {
    g_lastAutoMintAttemptMs = nowMs;
    g_nextAutoMintAtMs = nowMs + 5000UL;
    emitAutoMintEvent(
        "queue_failed",
        &profile,
        nonce,
        error,
        activeProfiles.size(),
        nowMs,
        g_nextAutoMintAtMs,
        false);
    return;
  }

  g_lastAutoMintAttemptMs = nowMs;
  g_autoMintCursor = (g_autoMintCursor + 1) % activeProfiles.size();
  armAutoMintSchedule(snapshot, activeProfiles, nowMs, false);
  emitAutoMintEvent(
      "queued",
      &profile,
      nonce,
      "",
      activeProfiles.size(),
      nowMs,
      g_nextAutoMintAtMs,
      true);
}

void setup() {
  Serial.begin(115200);

  const unsigned long waitStarted = millis();
  while (!Serial && (millis() - waitStarted) < 4000) {
    delay(10);
  }

  String error;
  if (!g_state.begin(error)) {
    DynamicJsonDocument fatalDoc(256);
    fatalDoc["type"] = "fatal";
    fatalDoc["message"] = error;
    serializeJson(fatalDoc, Serial);
    Serial.println();
    g_ready = false;
    return;
  }

  g_rpc.begin();
  g_wifiBridge.begin();
  g_bleBridge.begin();
  g_ready = true;
}

void loop() {
  if (!g_ready) {
    delay(250);
    return;
  }

  g_lorawan.poll();
  g_rpc.poll();
  g_wifiBridge.poll();
  g_bleBridge.poll();
  serviceAutoMint();
  delay(2);
}

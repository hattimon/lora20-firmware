#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>

#include "lora20_device.hpp"
#include "lorawan_client.hpp"
#include "serial_rpc.hpp"

lora20::DeviceStateStore g_state;
lora20::LoRaWanClient g_lorawan(g_state);
lora20::SerialRpcServer g_rpc(Serial, g_state, g_lorawan);
bool g_ready = false;
bool g_autoMintArmed = false;
unsigned long g_nextAutoMintAtMs = 0;
unsigned long g_lastAutoMintAttemptMs = 0;
uint32_t g_lastAutoMintIntervalSeconds = 0;
size_t g_autoMintCursor = 0;
std::vector<lora20::MintProfile> g_lastAutoMintProfiles;

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
                         unsigned long nowMs) {
  const unsigned long intervalMs = static_cast<unsigned long>(snapshot.config.autoMintIntervalSeconds) * 1000UL;
  g_autoMintArmed = true;
  g_nextAutoMintAtMs = nowMs + intervalMs;
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
    resetAutoMintSchedule();
    return;
  }

  collectActiveAutoMintProfiles(snapshot, activeProfiles);
  if (activeProfiles.empty()) {
    resetAutoMintSchedule();
    return;
  }

  if (autoMintProfileChanged(snapshot, activeProfiles)) {
    g_autoMintCursor = 0;
    armAutoMintSchedule(snapshot, activeProfiles, nowMs);
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
    return;
  }

  g_lastAutoMintAttemptMs = nowMs;
  g_autoMintCursor = (g_autoMintCursor + 1) % activeProfiles.size();
  armAutoMintSchedule(snapshot, activeProfiles, nowMs);
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
  g_ready = true;
}

void loop() {
  if (!g_ready) {
    delay(250);
    return;
  }

  g_lorawan.poll();
  g_rpc.poll();
  serviceAutoMint();
  delay(2);
}

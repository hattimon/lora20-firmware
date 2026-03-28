#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>
#include <ctime>

#include "heltec.h"
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

struct DeviceEvent {
  String label;
  uint32_t nonce = 0;
  size_t payloadSize = 0;
  time_t timestamp = 0;
};

constexpr size_t kMaxDeviceEvents = 12;
std::vector<DeviceEvent> g_deviceEvents;

constexpr uint8_t kMaxDisplayScreens = 3;
constexpr unsigned long kDisplayRefreshMs = 500;
constexpr unsigned long kQueueScrollIntervalMs = 2000;
constexpr unsigned long kLongPressMs = 3000;
#ifndef LORA20_PRG_PIN
#define LORA20_PRG_PIN 0
#endif
#ifndef LORA20_BATTERY_PIN
#if defined(WIFI_LORA_32_V4)
#define LORA20_BATTERY_PIN 1
#else
#define LORA20_BATTERY_PIN -1
#endif
#endif
#ifndef LORA20_BATTERY_ENABLE_PIN
#if defined(WIFI_LORA_32_V4)
#define LORA20_BATTERY_ENABLE_PIN 37
#else
#define LORA20_BATTERY_ENABLE_PIN -1
#endif
#endif
#ifndef LORA20_BATTERY_DIVIDER
#define LORA20_BATTERY_DIVIDER 4.9f
#endif
#ifndef LORA20_BATTERY_MIN_V
#define LORA20_BATTERY_MIN_V 3.2f
#endif
#ifndef LORA20_BATTERY_MAX_V
#define LORA20_BATTERY_MAX_V 4.2f
#endif

bool g_displayReady = false;
bool g_displaySleeping = false;
uint8_t g_displayScreen = 0;
unsigned long g_lastDisplayUpdateMs = 0;
unsigned long g_lastQueueScrollMs = 0;
size_t g_queueScrollIndex = 0;
bool g_buttonDown = false;
unsigned long g_buttonDownAtMs = 0;
bool g_timeSynced = false;

void collectActiveAutoMintProfiles(const lora20::DeviceSnapshot &snapshot, std::vector<lora20::MintProfile> &profiles);
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

String truncateText(const String &text, size_t maxLen) {
  if (text.length() <= maxLen) return text;
  if (maxLen <= 3) return text.substring(0, maxLen);
  return text.substring(0, maxLen - 3) + "...";
}

uint32_t readUint32BE(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

uint64_t readUint64BE(const uint8_t *data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[i]);
  }
  return value;
}

bool parsePayloadSummary(const std::vector<uint8_t> &payload, DeviceEvent &event) {
  if (payload.empty()) return false;
  const uint8_t op = payload[0];
  event.payloadSize = payload.size();

  auto readTick = [&]() -> String {
    if (payload.size() < 5) return "";
    String tick;
    tick.reserve(4);
    for (size_t i = 1; i < 5; ++i) {
      tick += static_cast<char>(payload[i]);
    }
    return tick;
  };

  if (op == lora20::kOpMint) {
    if (payload.size() < 1 + 4 + 8 + 4) return false;
    const String tick = readTick();
    const uint64_t amount = readUint64BE(&payload[5]);
    const uint32_t nonce = readUint32BE(&payload[13]);
    event.label = "MINT " + tick + " " + mintAmountToString(amount);
    event.nonce = nonce;
    return true;
  }

  if (op == lora20::kOpDeploy) {
    if (payload.size() < 1 + 4 + 8 + 8 + 4) return false;
    const String tick = readTick();
    const uint64_t maxSupply = readUint64BE(&payload[5]);
    const uint64_t limit = readUint64BE(&payload[13]);
    const uint32_t nonce = readUint32BE(&payload[21]);
    event.label = "DEPLOY " + tick + " max " + mintAmountToString(maxSupply) + " lim " + mintAmountToString(limit);
    event.nonce = nonce;
    return true;
  }

  if (op == lora20::kOpTransfer) {
    if (payload.size() < 1 + 4 + 8 + 4 + 8) return false;
    const String tick = readTick();
    const uint64_t amount = readUint64BE(&payload[5]);
    const uint32_t nonce = readUint32BE(&payload[13]);
    event.label = "TRANSFER " + tick + " " + mintAmountToString(amount);
    event.nonce = nonce;
    return true;
  }

  if (op == lora20::kOpConfig) {
    if (payload.size() < 1 + 1 + 4 + 4) return false;
    const bool enabled = (payload[1] & 0x01) != 0;
    const uint32_t interval = readUint32BE(&payload[2]);
    const uint32_t nonce = readUint32BE(&payload[6]);
    event.label = String("CONFIG auto ") + (enabled ? "ON " : "OFF ") + String(interval) + "s";
    event.nonce = nonce;
    return true;
  }

  event.label = "UPLINK";
  event.nonce = 0;
  return true;
}

void pushDeviceEvent(const DeviceEvent &event) {
  if (event.label.length() == 0) return;
  g_deviceEvents.insert(g_deviceEvents.begin(), event);
  if (g_deviceEvents.size() > kMaxDeviceEvents) {
    g_deviceEvents.pop_back();
  }
}

int readBatteryPercent() {
  if (LORA20_BATTERY_PIN < 0) return -1;
  if (LORA20_BATTERY_ENABLE_PIN >= 0) {
    pinMode(LORA20_BATTERY_ENABLE_PIN, OUTPUT);
    digitalWrite(LORA20_BATTERY_ENABLE_PIN, HIGH);
    delay(2);
  }
  analogReadResolution(12);
  analogSetPinAttenuation(LORA20_BATTERY_PIN, ADC_11db);
  const uint16_t raw = analogRead(LORA20_BATTERY_PIN);
  if (LORA20_BATTERY_ENABLE_PIN >= 0) {
    digitalWrite(LORA20_BATTERY_ENABLE_PIN, LOW);
  }
  if (raw == 0) return -1;
  const float vAdc = (static_cast<float>(raw) / 4095.0f) * 3.3f;
  const float voltage = vAdc * LORA20_BATTERY_DIVIDER;
  const float clamped = std::min(LORA20_BATTERY_MAX_V, std::max(LORA20_BATTERY_MIN_V, voltage));
  const int percent = static_cast<int>(((clamped - LORA20_BATTERY_MIN_V) / (LORA20_BATTERY_MAX_V - LORA20_BATTERY_MIN_V)) * 100.0f + 0.5f);
  return std::min(100, std::max(0, percent));
}

String formatClock() {
  const time_t now = time(nullptr);
  if (now < 1000000000) return "--:--";
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buffer);
}

String formatEventTime(time_t timestamp) {
  if (timestamp < 1000000000) return "--:--";
  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buffer);
}

String formatConnectionLabel() {
  switch (g_state.snapshot().connection.mode) {
    case lora20::ConnectionMode::kBle:
      return "BLE";
    case lora20::ConnectionMode::kWifi:
      return "WIFI";
    case lora20::ConnectionMode::kUsb:
    default:
      return "USB";
  }
}

void updateDisplay() {
  if (!g_displayReady || g_displaySleeping) return;
  const unsigned long nowMs = millis();
  if ((nowMs - g_lastDisplayUpdateMs) < kDisplayRefreshMs) return;
  g_lastDisplayUpdateMs = nowMs;

  const auto &snapshot = g_state.snapshot();
  const auto &runtime = g_lorawan.status();
  const int battery = readBatteryPercent();
  const String clock = formatClock();
  String wifiLine;
  if (snapshot.connection.mode == lora20::ConnectionMode::kWifi) {
    const String ip = g_wifiBridge.ipAddress();
    if (ip.length() > 0) {
      wifiLine = "WiFi: " + ip;
    } else {
      const String modeLabel = g_wifiBridge.modeLabel();
      wifiLine = modeLabel == "sta_connecting" ? "WiFi: connecting" : "WiFi: offline";
    }
  }

  Heltec.display->clear();
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->setFont(ArialMT_Plain_10);

  String topLeft = formatConnectionLabel();
  if (battery >= 0) {
    topLeft += " ";
    topLeft += String(battery);
    topLeft += "%";
  }
  Heltec.display->drawString(0, 0, topLeft);
  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(128, 0, clock);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);

  if (g_displayScreen == 0) {
    int y = 14;
    if (wifiLine.length() > 0) {
      Heltec.display->drawString(0, y, wifiLine);
      y += 12;
    }
    Heltec.display->drawString(0, y, runtime.joined ? "LoRa: joined" : "LoRa: offline");
    y += 12;
    Heltec.display->drawString(0, y, "Last:");
    y += 12;
    if (!g_deviceEvents.empty()) {
      Heltec.display->drawString(0, y, truncateText(g_deviceEvents.front().label, 20));
      y += 12;
      Heltec.display->drawString(0, y, "nonce " + String(g_deviceEvents.front().nonce));
    } else {
      Heltec.display->drawString(0, y, "no events yet");
    }
  } else if (g_displayScreen == 1) {
    const String autoMint = snapshot.config.autoMintEnabled ? "AutoMint ON" : "AutoMint OFF";
    Heltec.display->drawString(0, 14, autoMint + " " + String(snapshot.config.autoMintIntervalSeconds) + "s");
    std::vector<lora20::MintProfile> profiles;
    collectActiveAutoMintProfiles(snapshot, profiles);
    if (profiles.empty()) {
      Heltec.display->drawString(0, 30, "no profiles");
    } else {
      if ((nowMs - g_lastQueueScrollMs) > kQueueScrollIntervalMs) {
        g_queueScrollIndex = (g_queueScrollIndex + 1) % profiles.size();
        g_lastQueueScrollMs = nowMs;
      }
      const size_t visible = 3;
      for (size_t line = 0; line < visible; ++line) {
        const size_t index = (g_queueScrollIndex + line) % profiles.size();
        const auto &profile = profiles[index];
        String row = lora20::tickToString(profile.tick) + " " + mintAmountToString(profile.amount);
        Heltec.display->drawString(0, 28 + static_cast<int>(line) * 12, truncateText(row, 20));
      }
    }
  } else {
    if (!g_deviceEvents.empty()) {
      const auto &evt = g_deviceEvents.front();
      Heltec.display->drawString(0, 14, truncateText(evt.label, 20));
      Heltec.display->drawString(0, 28, "nonce " + String(evt.nonce));
      Heltec.display->drawString(0, 40, "payload " + String(evt.payloadSize) + "B");
      Heltec.display->drawString(0, 52, formatEventTime(evt.timestamp));
    } else {
      Heltec.display->drawString(0, 18, "no recent events");
    }
  }

  Heltec.display->display();
}

void handleButton() {
  const unsigned long nowMs = millis();
  const bool pressed = digitalRead(LORA20_PRG_PIN) == LOW;

  if (pressed && !g_buttonDown) {
    g_buttonDown = true;
    g_buttonDownAtMs = nowMs;
  }

  if (!pressed && g_buttonDown) {
    const unsigned long held = nowMs - g_buttonDownAtMs;
    g_buttonDown = false;
    if (held >= kLongPressMs) {
      g_displaySleeping = !g_displaySleeping;
      if (g_displaySleeping) {
        Heltec.display->displayOff();
      } else {
        Heltec.display->displayOn();
      }
    } else {
      if (g_displaySleeping) {
        g_displaySleeping = false;
        Heltec.display->displayOn();
      } else {
        g_displayScreen = (g_displayScreen + 1) % kMaxDisplayScreens;
      }
    }
  }
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

  Heltec.begin(true, false, false);
  if (Heltec.display) {
    Heltec.display->clear();
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->display();
    g_displayReady = true;
  }
  pinMode(LORA20_PRG_PIN, INPUT_PULLUP);

  g_rpc.begin();
  g_wifiBridge.begin();
  g_rpcProcessor.setWifiBridge(&g_wifiBridge);
  g_ready = true;
}

void loop() {
  if (!g_ready) {
    delay(250);
    return;
  }

  g_lorawan.poll();
  g_bleBridge.setEnabled(g_state.snapshot().connection.mode == lora20::ConnectionMode::kBle);
  g_wifiBridge.applyConfig(g_state.snapshot().connection);
  g_rpc.poll();
  g_wifiBridge.poll();
  g_bleBridge.poll();

  if (!g_timeSynced && WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    g_timeSynced = true;
  }

  std::vector<uint8_t> acceptedPayload;
  if (g_lorawan.takeAcceptedPayload(acceptedPayload)) {
    DeviceEvent event;
    if (parsePayloadSummary(acceptedPayload, event)) {
      event.timestamp = time(nullptr);
      pushDeviceEvent(event);
    }
  }

  serviceAutoMint();
  handleButton();
  updateDisplay();
  delay(2);
}

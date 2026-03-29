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

constexpr uint8_t kMaxDisplayScreens = 4;
constexpr unsigned long kDisplayRefreshMs = 500;
constexpr unsigned long kQueueScrollIntervalMs = 2000;
constexpr unsigned long kLongPressMs = 2200;
constexpr unsigned long kBridgeWindowDefaultMs = 300000UL;
constexpr unsigned long kLinkIdleGraceMs = 30000UL;
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
#ifndef LORA20_OLED_FREQ
#define LORA20_OLED_FREQ 400000
#endif

enum class DisplayScreen : uint8_t {
  kMintStream = 0,
  kAutomation = 1,
  kLastEvent = 2,
  kConnectivity = 3
};

enum class UiMode : uint8_t {
  kView = 0,
  kMenu = 1
};

bool g_displayReady = false;
bool g_displaySleeping = false;
DisplayScreen g_displayScreen = DisplayScreen::kMintStream;
UiMode g_uiMode = UiMode::kView;
uint8_t g_menuIndex = 0;
size_t g_profileMenuIndex = 0;
unsigned long g_lastDisplayUpdateMs = 0;
unsigned long g_lastQueueScrollMs = 0;
size_t g_queueScrollIndex = 0;
bool g_buttonDown = false;
unsigned long g_buttonDownAtMs = 0;
unsigned long g_lastUserInteractionMs = 0;
unsigned long g_bridgeWindowUntilMs = 0;
unsigned long g_lastBridgeActivityMs = 0;
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

String connectionModeLabel(lora20::ConnectionMode mode) {
  switch (mode) {
    case lora20::ConnectionMode::kBle:
      return "BLE";
    case lora20::ConnectionMode::kWifi:
      return "WIFI";
    case lora20::ConnectionMode::kUsb:
    default:
      return "USB";
  }
}

bool tryInitDisplay(uint32_t i2cFreq, int8_t rstPin, DISPLAY_GEOMETRY geometry) {
  SSD1306Wire *candidate = new SSD1306Wire(0x3c, i2cFreq, SDA_OLED, SCL_OLED, geometry, rstPin);
  if (candidate == nullptr) {
    Serial.println("[display] allocation failed");
    return false;
  }
  if (!candidate->init()) {
    Serial.printf("[display] init failed freq=%lu rst=%d geom=%d\n",
                  static_cast<unsigned long>(i2cFreq),
                  static_cast<int>(rstPin),
                  static_cast<int>(geometry));
    delete candidate;
    return false;
  }

  Serial.printf("[display] init ok freq=%lu rst=%d geom=%d\n",
                static_cast<unsigned long>(i2cFreq),
                static_cast<int>(rstPin),
                static_cast<int>(geometry));
  Heltec.display = candidate;
  Heltec.display->clear();
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->displayOn();
  Heltec.display->drawString(0, 0, "LORA20 boot");
  Heltec.display->drawString(0, 12, String("OLED ") + String(i2cFreq) + "Hz");
  Heltec.display->display();
  return true;
}

void pushSystemEvent(const String &label) {
  DeviceEvent event;
  event.label = label;
  event.nonce = g_state.snapshot().nextNonce;
  event.payloadSize = 0;
  event.timestamp = time(nullptr);
  pushDeviceEvent(event);
}

void reopenBridgeWindow() {
  const unsigned long nowMs = millis();
  const uint16_t windowSeconds = std::max<uint16_t>(30, g_state.snapshot().connection.bridgeWindowSeconds);
  g_lastBridgeActivityMs = nowMs;
  g_bridgeWindowUntilMs = nowMs + static_cast<unsigned long>(windowSeconds) * 1000UL;
}

void markUserInteraction() {
  g_lastUserInteractionMs = millis();
}

void wakeDisplay() {
  markUserInteraction();
  if (!g_displayReady || Heltec.display == nullptr) return;
  if (g_displaySleeping) {
    Heltec.display->displayOn();
  }
  g_displaySleeping = false;
}

uint8_t menuItemCount(DisplayScreen screen) {
  switch (screen) {
    case DisplayScreen::kMintStream:
      return 3;  // Wake links, clear stream, back
    case DisplayScreen::kAutomation:
      return 4;  // Toggle auto-mint, select profile, toggle profile, back
    case DisplayScreen::kLastEvent:
      return 2;  // Refresh clock, back
    case DisplayScreen::kConnectivity:
    default:
      return 5;  // Mode, AP fallback, sleep, power save, back
  }
}

String menuItemLabel(DisplayScreen screen) {
  const auto &snapshot = g_state.snapshot();
  switch (screen) {
    case DisplayScreen::kMintStream:
      if (g_menuIndex == 0) return "Enable links 5m";
      if (g_menuIndex == 1) return "Clear mint stream";
      return "Back";
    case DisplayScreen::kAutomation:
      if (g_menuIndex == 0) {
        return snapshot.config.autoMintEnabled ? "AutoMint: ON" : "AutoMint: OFF";
      }
      if (g_menuIndex == 1) {
        if (snapshot.config.mintProfiles.empty()) return "Select profile (none)";
        const size_t selected = g_profileMenuIndex % snapshot.config.mintProfiles.size();
        const auto &profile = snapshot.config.mintProfiles[selected];
        String line = "Profile: ";
        line += lora20::tickToString(profile.tick);
        line += profile.enabled ? " ON" : " OFF";
        return line;
      }
      if (g_menuIndex == 2) {
        return "Toggle profile";
      }
      return "Back";
    case DisplayScreen::kLastEvent:
      return g_menuIndex == 0 ? "Sync clock (NTP)" : "Back";
    case DisplayScreen::kConnectivity:
    default:
      if (g_menuIndex == 0) {
        return "Mode: " + connectionModeLabel(snapshot.connection.mode);
      }
      if (g_menuIndex == 1) {
        return snapshot.connection.wifiApFallback ? "AP fallback: ON" : "AP fallback: OFF";
      }
      if (g_menuIndex == 2) {
        if (snapshot.connection.displaySleepSeconds == 0) return "Sleep: OFF";
        return "Sleep: " + String(snapshot.connection.displaySleepSeconds) + "s";
      }
      if (g_menuIndex == 3) {
        return "Power save: L" + String(snapshot.connection.powerSaveLevel);
      }
      return "Back";
  }
}

void executeMenuAction() {
  auto snapshot = g_state.snapshot();
  String error;

  if (g_displayScreen == DisplayScreen::kMintStream) {
    if (g_menuIndex == 0) {
      reopenBridgeWindow();
      pushSystemEvent("LINKS window extended");
    } else if (g_menuIndex == 1) {
      g_deviceEvents.clear();
      pushSystemEvent("Mint stream cleared");
    } else {
      g_uiMode = UiMode::kView;
    }
    return;
  }

  if (g_displayScreen == DisplayScreen::kAutomation) {
    auto next = snapshot.config;
    if (g_menuIndex == 0) {
      next.autoMintEnabled = !next.autoMintEnabled;
      if (g_state.updateConfig(next, error)) {
        pushSystemEvent(next.autoMintEnabled ? "AutoMint enabled" : "AutoMint disabled");
      } else {
        pushSystemEvent("ERR config " + error);
      }
      return;
    }
    if (g_menuIndex == 1) {
      if (!next.mintProfiles.empty()) {
        g_profileMenuIndex = (g_profileMenuIndex + 1) % next.mintProfiles.size();
      }
      return;
    }
    if (g_menuIndex == 2) {
      if (!next.mintProfiles.empty()) {
        const size_t selected = g_profileMenuIndex % next.mintProfiles.size();
        next.mintProfiles[selected].enabled = !next.mintProfiles[selected].enabled;
        if (g_state.updateConfig(next, error)) {
          pushSystemEvent(String("Profile ") + lora20::tickToString(next.mintProfiles[selected].tick) +
                          (next.mintProfiles[selected].enabled ? " enabled" : " disabled"));
        } else {
          pushSystemEvent("ERR profile " + error);
        }
      }
      return;
    }
    g_uiMode = UiMode::kView;
    return;
  }

  if (g_displayScreen == DisplayScreen::kLastEvent) {
    if (g_menuIndex == 0) {
      g_timeSynced = false;
      pushSystemEvent("Clock sync requested");
    } else {
      g_uiMode = UiMode::kView;
    }
    return;
  }

  auto nextConnection = snapshot.connection;
  if (g_menuIndex == 0) {
    if (nextConnection.mode == lora20::ConnectionMode::kUsb) {
      nextConnection.mode = lora20::ConnectionMode::kBle;
    } else if (nextConnection.mode == lora20::ConnectionMode::kBle) {
      nextConnection.mode = lora20::ConnectionMode::kWifi;
    } else {
      nextConnection.mode = lora20::ConnectionMode::kUsb;
    }
    if (g_state.updateConnectionConfig(nextConnection, error)) {
      reopenBridgeWindow();
      pushSystemEvent("Connection mode " + connectionModeLabel(nextConnection.mode));
    } else {
      pushSystemEvent("ERR connectivity " + error);
    }
    return;
  }
  if (g_menuIndex == 1) {
    nextConnection.wifiApFallback = !nextConnection.wifiApFallback;
    if (g_state.updateConnectionConfig(nextConnection, error)) {
      pushSystemEvent(nextConnection.wifiApFallback ? "AP fallback enabled" : "AP fallback disabled");
    } else {
      pushSystemEvent("ERR fallback " + error);
    }
    return;
  }
  if (g_menuIndex == 2) {
    const uint16_t current = nextConnection.displaySleepSeconds;
    if (current == 0) {
      nextConnection.displaySleepSeconds = 60;
    } else if (current <= 60) {
      nextConnection.displaySleepSeconds = 300;
    } else {
      nextConnection.displaySleepSeconds = 0;
    }
    if (g_state.updateConnectionConfig(nextConnection, error)) {
      markUserInteraction();
      pushSystemEvent(String("Sleep ") + (nextConnection.displaySleepSeconds == 0 ? "OFF" : String(nextConnection.displaySleepSeconds) + "s"));
    } else {
      pushSystemEvent("ERR sleep " + error);
    }
    return;
  }
  if (g_menuIndex == 3) {
    nextConnection.powerSaveLevel = static_cast<uint8_t>((nextConnection.powerSaveLevel + 1) % 3);
    if (g_state.updateConnectionConfig(nextConnection, error)) {
      pushSystemEvent("Power save L" + String(nextConnection.powerSaveLevel));
    } else {
      pushSystemEvent("ERR power " + error);
    }
    return;
  }
  g_uiMode = UiMode::kView;
}

void updateDisplay() {
  if (!g_displayReady || Heltec.display == nullptr) return;

  const unsigned long nowMs = millis();
  const uint16_t sleepSeconds = g_state.snapshot().connection.displaySleepSeconds;
  if (!g_displaySleeping && sleepSeconds > 0 &&
      nowMs > g_lastUserInteractionMs &&
      (nowMs - g_lastUserInteractionMs) >= static_cast<unsigned long>(sleepSeconds) * 1000UL) {
    g_displaySleeping = true;
    Heltec.display->displayOff();
    return;
  }

  if (g_displaySleeping) return;
  if ((nowMs - g_lastDisplayUpdateMs) < kDisplayRefreshMs) return;
  g_lastDisplayUpdateMs = nowMs;

  const auto &snapshot = g_state.snapshot();
  const auto &runtime = g_lorawan.status();
  const int battery = readBatteryPercent();
  const String clock = formatClock();
  const String wifiMode = g_wifiBridge.modeLabel();
  const String wifiIp = g_wifiBridge.ipAddress();

  Heltec.display->clear();
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->setFont(ArialMT_Plain_10);

  String topLeft = connectionModeLabel(snapshot.connection.mode);
  if (battery >= 0) {
    topLeft += " ";
    topLeft += String(battery);
    topLeft += "%";
  }
  Heltec.display->drawString(0, 0, topLeft);
  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(128, 0, clock);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);

  if (g_displayScreen == DisplayScreen::kMintStream) {
    Heltec.display->drawString(0, 14, "Mint stream");
    if (g_deviceEvents.empty()) {
      Heltec.display->drawString(0, 28, "No mint events");
    } else {
      const size_t visible = std::min<size_t>(3, g_deviceEvents.size());
      for (size_t line = 0; line < visible; ++line) {
        const auto &evt = g_deviceEvents[line];
        String row = truncateText(evt.label, 18);
        if (line == 0 && evt.nonce != 0) {
          row += " n";
          row += String(evt.nonce);
        }
        Heltec.display->drawString(0, 28 + static_cast<int>(line) * 12, row);
      }
    }
  } else if (g_displayScreen == DisplayScreen::kAutomation) {
    Heltec.display->drawString(0, 14, snapshot.config.autoMintEnabled ? "Automation: ON" : "Automation: OFF");
    Heltec.display->drawString(0, 26, "Every " + String(snapshot.config.autoMintIntervalSeconds) + "s");
    std::vector<lora20::MintProfile> profiles;
    collectActiveAutoMintProfiles(snapshot, profiles);
    if (profiles.empty()) {
      Heltec.display->drawString(0, 40, "No active profiles");
    } else {
      if ((nowMs - g_lastQueueScrollMs) > kQueueScrollIntervalMs) {
        g_queueScrollIndex = (g_queueScrollIndex + 1) % profiles.size();
        g_lastQueueScrollMs = nowMs;
      }
      const size_t index = g_queueScrollIndex % profiles.size();
      const auto &profile = profiles[index];
      Heltec.display->drawString(0, 40, truncateText("Queue: " + lora20::tickToString(profile.tick) + " " + mintAmountToString(profile.amount), 20));
      Heltec.display->drawString(0, 52, "idx " + String(index + 1) + "/" + String(profiles.size()));
    }
  } else if (g_displayScreen == DisplayScreen::kLastEvent) {
    if (!g_deviceEvents.empty()) {
      const auto &evt = g_deviceEvents.front();
      Heltec.display->drawString(0, 14, truncateText(evt.label, 20));
      Heltec.display->drawString(0, 28, "nonce " + String(evt.nonce));
      Heltec.display->drawString(0, 40, "payload " + String(evt.payloadSize) + "B");
      Heltec.display->drawString(0, 52, formatEventTime(evt.timestamp));
    } else {
      Heltec.display->drawString(0, 24, "No recent events");
      Heltec.display->drawString(0, 38, runtime.joined ? "LoRa joined" : "LoRa offline");
    }
  } else {
    Heltec.display->drawString(0, 14, "Conn: " + connectionModeLabel(snapshot.connection.mode));
    if (wifiIp.length() > 0) {
      Heltec.display->drawString(0, 26, truncateText("IP " + wifiIp, 20));
    } else {
      Heltec.display->drawString(0, 26, "WiFi " + wifiMode);
    }
    const unsigned long remainingMs = nowMs < g_bridgeWindowUntilMs ? (g_bridgeWindowUntilMs - nowMs) : 0;
    Heltec.display->drawString(0, 38, "Links " + String(remainingMs / 1000) + "s");
    Heltec.display->drawString(0, 50, "PWR L" + String(snapshot.connection.powerSaveLevel));
  }

  if (g_uiMode == UiMode::kMenu) {
    const uint8_t count = menuItemCount(g_displayScreen);
    if (count > 0) {
      g_menuIndex %= count;
      Heltec.display->fillRect(0, 52, 128, 12);
      Heltec.display->setColor(BLACK);
      Heltec.display->drawString(2, 53, truncateText("> " + menuItemLabel(g_displayScreen), 20));
      Heltec.display->setColor(WHITE);
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
    markUserInteraction();

    if (held >= kLongPressMs) {
      if (g_displaySleeping) {
        wakeDisplay();
        return;
      }
      if (g_uiMode == UiMode::kView) {
        g_uiMode = UiMode::kMenu;
        g_menuIndex = 0;
      } else {
        executeMenuAction();
      }
      return;
    }

    if (g_displaySleeping) {
      wakeDisplay();
      return;
    }

    if (g_uiMode == UiMode::kMenu) {
      const uint8_t count = menuItemCount(g_displayScreen);
      if (count > 0) {
        g_menuIndex = (g_menuIndex + 1) % count;
      }
    } else {
      const uint8_t next = (static_cast<uint8_t>(g_displayScreen) + 1) % kMaxDisplayScreens;
      g_displayScreen = static_cast<DisplayScreen>(next);
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

void applyConnectivityPolicy() {
  const auto &connection = g_state.snapshot().connection;
  const unsigned long nowMs = millis();
  const unsigned long windowMs = static_cast<unsigned long>(std::max<uint16_t>(30, connection.bridgeWindowSeconds)) * 1000UL;
  const bool wifiConfigured = std::strlen(connection.wifiSsid) > 0 || connection.wifiApFallback;

  const unsigned long latestActivity = std::max(
      std::max(g_rpc.lastActivityMs(), g_wifiBridge.lastActivityMs()),
      g_bleBridge.lastActivityMs());
  if (latestActivity > g_lastBridgeActivityMs) {
    g_lastBridgeActivityMs = latestActivity;
    g_bridgeWindowUntilMs = latestActivity + windowMs;
    markUserInteraction();
    if (g_displaySleeping) {
      wakeDisplay();
    }
  }
  if (g_bridgeWindowUntilMs == 0) {
    g_bridgeWindowUntilMs = nowMs + windowMs;
  }

  bool windowOpen = nowMs < g_bridgeWindowUntilMs;
  if (connection.powerSaveLevel == 0) {
    windowOpen = true;
  }

  bool enableBle = false;
  bool enableWifi = false;
  const bool bleClientConnected = g_bleBridge.connected();
  switch (connection.mode) {
    case lora20::ConnectionMode::kUsb:
      enableBle = true;
      // Prefer BLE link stability in USB mode: when a BLE client is connected,
      // temporarily suspend Wi-Fi to avoid ESP32-S3 coexistence edge cases.
      enableWifi = wifiConfigured && !bleClientConnected;
      break;
    case lora20::ConnectionMode::kBle:
      enableBle = true;
      enableWifi = false;
      break;
    case lora20::ConnectionMode::kWifi:
      enableBle = true;
      enableWifi = wifiConfigured;
      break;
    default:
      break;
  }

  g_bleBridge.setEnabled(enableBle);
  g_wifiBridge.applyConfig(connection, enableWifi);
  if (enableWifi) {
    // ESP32-S3 requires modem sleep when WiFi and BLE are active at the same time.
    const bool forceModemSleepForCoex = enableBle;
    WiFi.setSleep(forceModemSleepForCoex || connection.powerSaveLevel >= 2);
  }
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

  Heltec.begin(false, false, false);
#ifdef Heltec_Vext
  // Heltec V4 OLED power rail is controlled by Vext. Force it ON before custom display init.
  Heltec.VextON();
  delay(50);
#endif
#if defined(SDA_OLED) && defined(SCL_OLED) && defined(RST_OLED)
  // Force explicit OLED init with fallbacks for Heltec V4 variants.
  Heltec.display = nullptr;
  const uint32_t freqs[] = {500000UL, 400000UL, 100000UL};
  const int8_t resetPins[] = {RST_OLED, -1};
  const DISPLAY_GEOMETRY geometries[] = {GEOMETRY_128_64, GEOMETRY_64_32};
  for (const auto freq : freqs) {
    for (const auto resetPin : resetPins) {
      for (const auto geometry : geometries) {
        if (tryInitDisplay(freq, resetPin, geometry)) {
          g_displayReady = true;
          break;
        }
      }
      if (g_displayReady) break;
    }
    if (g_displayReady) break;
  }
  if (!g_displayReady) {
    Serial.println("[display] all init attempts failed");
  }
#else
  if (Heltec.display != nullptr) {
    Heltec.display->init();
    Heltec.display->clear();
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->displayOn();
    Heltec.display->display();
    g_displayReady = true;
  }
#endif
  pinMode(LORA20_PRG_PIN, INPUT_PULLUP);

  g_rpc.begin();
  g_wifiBridge.begin();
  g_rpcProcessor.setWifiBridge(&g_wifiBridge);
  const unsigned long startedAtMs = millis();
  g_lastUserInteractionMs = startedAtMs;
  g_lastBridgeActivityMs = startedAtMs;
  g_bridgeWindowUntilMs = startedAtMs + kBridgeWindowDefaultMs;
  g_ready = true;
}

void loop() {
  if (!g_ready) {
    delay(250);
    return;
  }

  g_rpc.poll();
  applyConnectivityPolicy();
  g_wifiBridge.poll();
  g_bleBridge.poll();
  g_lorawan.poll();

  if (!g_timeSynced && g_wifiBridge.modeLabel() == "sta_connected") {
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
  const uint8_t powerSaveLevel = g_state.snapshot().connection.powerSaveLevel;
  delay(powerSaveLevel >= 2 ? 10 : 2);
}

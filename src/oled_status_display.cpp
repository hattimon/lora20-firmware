#include "oled_status_display.hpp"

#include <Adafruit_GFX.h>
#define SSD1306_NO_SPLASH
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>

#include <algorithm>

namespace {

constexpr uint8_t kDisplayAddress = LORA20_OLED_I2C_ADDR;
constexpr uint16_t kDisplayWidth = 128;
constexpr uint16_t kDisplayHeight = 64;
constexpr int kPrgButtonPin = 0;
constexpr unsigned long kMenuActionHoldMs = 2000UL;
constexpr unsigned long kBootSwitchHoldMs = 3000UL;
constexpr unsigned long kDebounceMs = 40UL;
constexpr std::array<uint32_t, 5> kSleepPresets = {60UL, 120UL, 240UL, 480UL, 0UL};
constexpr std::array<uint32_t, 9> kIntervalPresets = {
    60UL, 300UL, 900UL, 1800UL, 3600UL, 14400UL, 28800UL, 57600UL, 86400UL};

Adafruit_SSD1306 g_display(kDisplayWidth, kDisplayHeight, &Wire, RST_OLED);

void writeDisplayPower(bool enabled) {
#ifdef Vext
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, enabled ? LORA20_OLED_VEXT_ON_LEVEL : LORA20_OLED_VEXT_OFF_LEVEL);
#else
  (void)enabled;
#endif
}

void powerCycleDisplayRail() {
  writeDisplayPower(false);
  delay(20);
  writeDisplayPower(true);
  delay(60);
}

void resetDisplayPanel() {
#ifdef RST_OLED
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, HIGH);
  delay(2);
  digitalWrite(RST_OLED, LOW);
  delay(10);
  digitalWrite(RST_OLED, HIGH);
  delay(10);
#endif
}

bool probeAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

String twoDigitNumber(int value) {
  if (value < 10) {
    return String("0") + String(value);
  }
  return String(value);
}

String formatSixDigitPin(uint32_t pin) {
  char buffer[7];
  snprintf(buffer, sizeof(buffer), "%06lu", static_cast<unsigned long>(pin % 1000000UL));
  return String(buffer);
}

String u64ToString(uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
}

String shortenMiddle(const String &text, size_t maxLength) {
  if (text.length() <= maxLength) {
    return text;
  }
  const size_t left = (maxLength / 2) - 1;
  const size_t right = maxLength - left - 3;
  return text.substring(0, left) + "..." + text.substring(text.length() - right);
}

String formatProtocolLabel(const String &protocol) {
  String normalized = protocol;
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "meshcore") return "MeshCore";
  if (normalized == "meshtastic") return "Meshtastic";
  return "LoRa20";
}

void drawTextLine(int16_t x, int16_t y, const String &text, uint8_t size = 1) {
  g_display.setTextSize(size);
  g_display.setTextColor(SSD1306_WHITE);
  g_display.setCursor(x, y);
  g_display.print(text);
}

void drawCenteredLine(int16_t y, const String &text, uint8_t size = 1) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  g_display.setTextSize(size);
  g_display.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  const int16_t x = static_cast<int16_t>((kDisplayWidth - width) / 2);
  g_display.setCursor(x < 0 ? 0 : x, y);
  g_display.print(text);
}

}  // namespace

namespace lora20 {

OledStatusDisplay::OledStatusDisplay(Stream &serial,
                                     DeviceStateStore &state,
                                     ConnectivityManager &connectivity)
    : serial_(serial), state_(state), connectivity_(connectivity) {}

bool OledStatusDisplay::begin(const BootControlStatus &bootStatus,
                              const LoRaWanRuntimeStatus &lorawanStatus,
                              const ConnectivityRuntimeStatus &connectivityStatus,
                              const AutoMintRuntimeStatus &autoMintStatus,
                              String &error) {
  error = "";

  powerCycleDisplayRail();
  resetDisplayPanel();

  Wire.begin(SDA_OLED, SCL_OLED);
  Wire.setClock(LORA20_OLED_I2C_FREQ);

  if (!g_display.begin(SSD1306_SWITCHCAPVCC, kDisplayAddress, true, false)) {
    error = "OLED begin failed";
    return false;
  }

  if (!probeAddress(kDisplayAddress)) {
    error = String("OLED not detected at 0x") + String(kDisplayAddress, HEX);
    return false;
  }

  pinMode(kPrgButtonPin, INPUT_PULLUP);
  applyBrightnessProfile();
  available_ = true;
  lastActivityMs_ = millis();
  lastSeenConnectivityActivity_ = connectivityStatus.activityCounter;
  lastSeenJoined_ = lorawanStatus.joined;
  lastSeenEvent_ = lorawanStatus.lastEvent;
  lastSeenAcceptedSendMs_ = lorawanStatus.lastAcceptedSendMs;

#if defined(LORA20_OLED_SELF_TEST)
  selfTest();
#endif

  renderScreen(bootStatus, lorawanStatus, connectivityStatus, autoMintStatus, millis());
  return true;
}

void OledStatusDisplay::poll(const BootControlStatus &bootStatus,
                             const LoRaWanRuntimeStatus &lorawanStatus,
                             const ConnectivityRuntimeStatus &connectivityStatus,
                             const AutoMintRuntimeStatus &autoMintStatus) {
  if (!available_) {
    return;
  }

  const unsigned long nowMs = millis();
  handleMenuButton(nowMs, autoMintStatus);
  trackLoRaActivity(lorawanStatus, nowMs);
  trackConnectivityActivity(connectivityStatus, nowMs);

  const uint32_t sleepSeconds = connectivityStatus.displaySleepSeconds;
  if (sleepSeconds != 0 && !sleeping_ && (nowMs - lastActivityMs_) >= (sleepSeconds * 1000UL)) {
    sleepDisplay();
    return;
  }

  if (sleeping_) {
    return;
  }

  renderScreen(bootStatus, lorawanStatus, connectivityStatus, autoMintStatus, nowMs);
}

void OledStatusDisplay::applyBrightnessProfile() {
  g_display.dim(false);
  g_display.ssd1306_command(SSD1306_DISPLAYALLON_RESUME);
  g_display.ssd1306_command(SSD1306_NORMALDISPLAY);
  g_display.ssd1306_command(SSD1306_SETCONTRAST);
  g_display.ssd1306_command(0xFF);
  g_display.ssd1306_command(SSD1306_SETPRECHARGE);
  g_display.ssd1306_command(0xF1);
  g_display.ssd1306_command(SSD1306_SETVCOMDETECT);
  g_display.ssd1306_command(0x40);
}

void OledStatusDisplay::sleepDisplay() {
  if (sleeping_ || !available_) {
    return;
  }
  g_display.ssd1306_command(SSD1306_DISPLAYOFF);
  sleeping_ = true;
  lastRenderedSignature_ = "";
}

void OledStatusDisplay::wakeDisplay() {
  if (!available_) {
    return;
  }
  g_display.ssd1306_command(SSD1306_DISPLAYON);
  applyBrightnessProfile();
  sleeping_ = false;
  lastRenderedSignature_ = "";
}

void OledStatusDisplay::recordActivity(unsigned long nowMs) {
  lastActivityMs_ = nowMs;
  if (sleeping_) {
    wakeDisplay();
  }
}

void OledStatusDisplay::handleMenuButton(unsigned long nowMs, const AutoMintRuntimeStatus &autoMintStatus) {
  const bool pressed = digitalRead(kPrgButtonPin) == LOW;

  if (pressed && !buttonPressed_) {
    buttonPressed_ = true;
    buttonDownSinceMs_ = nowMs;
    ignoreReleaseAfterWake_ = false;
    if (sleeping_) {
      recordActivity(nowMs);
      ignoreReleaseAfterWake_ = true;
    }
    return;
  }

  if (!pressed && buttonPressed_) {
    const unsigned long heldMs = nowMs - buttonDownSinceMs_;
    buttonPressed_ = false;
    buttonDownSinceMs_ = 0;

    if (ignoreReleaseAfterWake_) {
      ignoreReleaseAfterWake_ = false;
      return;
    }

    if (heldMs < kDebounceMs) {
      return;
    }

    if (heldMs >= kMenuActionHoldMs && heldMs < kBootSwitchHoldMs) {
      handleLongPress(state_.snapshot(), autoMintStatus);
      recordActivity(nowMs);
      return;
    }

    if (heldMs < kMenuActionHoldMs) {
      handleShortPress(state_.snapshot());
      recordActivity(nowMs);
    }
  }
}

void OledStatusDisplay::trackLoRaActivity(const LoRaWanRuntimeStatus &lorawanStatus, unsigned long nowMs) {
  bool sawActivity = false;

  if (lorawanStatus.joined != lastSeenJoined_) {
    lastSeenJoined_ = lorawanStatus.joined;
    sawActivity = true;
  }

  if (lorawanStatus.lastEvent != lastSeenEvent_) {
    lastSeenEvent_ = lorawanStatus.lastEvent;
    if (lorawanStatus.lastEvent.length() > 0) {
      sawActivity = true;
    }
  }

  if (lorawanStatus.lastAcceptedSendMs != lastSeenAcceptedSendMs_) {
    lastSeenAcceptedSendMs_ = lorawanStatus.lastAcceptedSendMs;
    if (lorawanStatus.lastAcceptedSendMs != 0) {
      sawActivity = true;
    }
  }

  if (lorawanStatus.joining || lorawanStatus.queuePending) {
    sawActivity = true;
  }

  if (sawActivity) {
    recordActivity(nowMs);
  }
}

void OledStatusDisplay::trackConnectivityActivity(const ConnectivityRuntimeStatus &connectivityStatus,
                                                  unsigned long nowMs) {
  if (connectivityStatus.activityCounter != lastSeenConnectivityActivity_) {
    lastSeenConnectivityActivity_ = connectivityStatus.activityCounter;
    recordActivity(nowMs);
  }
}

size_t OledStatusDisplay::buildMenuItems(const DeviceSnapshot &snapshot,
                                         std::array<MenuItem, kMaxMenuItems> &items) const {
  size_t count = 0;
  auto append = [&](MenuItemType type, int profileIndex = -1) {
    if (count >= items.size()) {
      return;
    }
    items[count].type = type;
    items[count].profileIndex = profileIndex;
    count += 1;
  };

  append(MenuItemType::MainStatus);
  append(MenuItemType::LinksStatus);
  append(MenuItemType::ToggleBluetooth);
  append(MenuItemType::ToggleWifi);
  append(MenuItemType::CycleSleep);
  append(MenuItemType::AutomationStatus);
  append(MenuItemType::ToggleAutomation);
  append(MenuItemType::CycleInterval);

  for (size_t index = 0; index < snapshot.config.mintProfiles.size() && count < items.size() - 2; ++index) {
    append(MenuItemType::ToggleProfile, static_cast<int>(index));
  }

  append(MenuItemType::RecentAutoMints);
  append(MenuItemType::ExitMenu);
  return count;
}

void OledStatusDisplay::handleShortPress(const DeviceSnapshot &snapshot) {
  std::array<MenuItem, kMaxMenuItems> items{};
  const size_t count = buildMenuItems(snapshot, items);
  if (count == 0) {
    selectedMenuIndex_ = 0;
    return;
  }
  selectedMenuIndex_ = (selectedMenuIndex_ + 1) % count;
  lastRenderedSignature_ = "";
}

void OledStatusDisplay::handleLongPress(const DeviceSnapshot &snapshot, const AutoMintRuntimeStatus &) {
  std::array<MenuItem, kMaxMenuItems> items{};
  const size_t count = buildMenuItems(snapshot, items);
  if (count == 0) {
    return;
  }
  if (selectedMenuIndex_ >= count) {
    selectedMenuIndex_ = 0;
  }

  const MenuItem &item = items[selectedMenuIndex_];
  switch (item.type) {
    case MenuItemType::ToggleBluetooth: {
      ConnectivityConfig next = snapshot.connectivity;
      next.bluetoothEnabled = !next.bluetoothEnabled;
      applyConnectivityChange(next, next.bluetoothEnabled ? "Bluetooth enabled" : "Bluetooth disabled");
      break;
    }
    case MenuItemType::ToggleWifi: {
      ConnectivityConfig next = snapshot.connectivity;
      next.wifiEnabled = !next.wifiEnabled;
      applyConnectivityChange(next, next.wifiEnabled ? "Wi-Fi enabled" : "Wi-Fi disabled");
      break;
    }
    case MenuItemType::CycleSleep: {
      ConnectivityConfig next = snapshot.connectivity;
      next.displaySleepSeconds = nextSleepPreset(next.displaySleepSeconds);
      applyConnectivityChange(next, String("Display sleep set to ") + intervalLabel(next.displaySleepSeconds));
      break;
    }
    case MenuItemType::ToggleAutomation: {
      DeviceConfig next = snapshot.config;
      next.autoMintEnabled = !next.autoMintEnabled;
      applyDeviceConfigChange(next, next.autoMintEnabled ? "Automation enabled" : "Automation disabled");
      break;
    }
    case MenuItemType::CycleInterval: {
      DeviceConfig next = snapshot.config;
      next.autoMintIntervalSeconds = nextIntervalPreset(next.autoMintIntervalSeconds);
      applyDeviceConfigChange(next, String("Automation interval set to ") + intervalLabel(next.autoMintIntervalSeconds));
      break;
    }
    case MenuItemType::ToggleProfile: {
      if (item.profileIndex >= 0 && static_cast<size_t>(item.profileIndex) < snapshot.config.mintProfiles.size()) {
        DeviceConfig next = snapshot.config;
        auto &profile = next.mintProfiles[static_cast<size_t>(item.profileIndex)];
        profile.enabled = !profile.enabled;
        applyDeviceConfigChange(
            next,
            String("Profile ") + tickToString(profile.tick) + (profile.enabled ? " enabled" : " disabled"));
      }
      break;
    }
    case MenuItemType::ExitMenu:
      selectedMenuIndex_ = 0;
      logUiEvent("exit", "OLED menu returned to status");
      break;
    default:
      break;
  }

  lastRenderedSignature_ = "";
}

bool OledStatusDisplay::applyConnectivityChange(ConnectivityConfig &config, const String &successLabel) {
  String error;
  if (!connectivity_.updateConfig(config, error)) {
    logUiEvent("connectivity_error", error);
    return false;
  }
  logUiEvent("connectivity", successLabel);
  return true;
}

bool OledStatusDisplay::applyDeviceConfigChange(DeviceConfig &config, const String &successLabel) {
  String error;
  if (!state_.updateConfig(config, error)) {
    logUiEvent("config_error", error);
    return false;
  }
  logUiEvent("config", successLabel);
  return true;
}

void OledStatusDisplay::logUiEvent(const char *stage, const String &message) {
  DynamicJsonDocument event(256);
  event["type"] = "oled_menu";
  event["stage"] = stage != nullptr ? stage : "unknown";
  event["message"] = message;
  serializeJson(event, serial_);
  serial_.println();
}

uint32_t OledStatusDisplay::nextSleepPreset(uint32_t current) const {
  for (size_t index = 0; index < kSleepPresets.size(); ++index) {
    if (kSleepPresets[index] == current) {
      return kSleepPresets[(index + 1) % kSleepPresets.size()];
    }
  }
  return kSleepPresets.front();
}

uint32_t OledStatusDisplay::nextIntervalPreset(uint32_t current) const {
  for (size_t index = 0; index < kIntervalPresets.size(); ++index) {
    if (kIntervalPresets[index] == current) {
      return kIntervalPresets[(index + 1) % kIntervalPresets.size()];
    }
  }
  return kIntervalPresets.front();
}

String OledStatusDisplay::intervalLabel(uint32_t seconds) const {
  if (seconds == 0) {
    return "always_on";
  }
  if (seconds < 3600UL) {
    return String(seconds / 60UL) + "m";
  }
  if (seconds % 3600UL == 0) {
    return String(seconds / 3600UL) + "h";
  }
  return String(seconds) + "s";
}

void OledStatusDisplay::drawTopBar(const ConnectivityRuntimeStatus &connectivityStatus, unsigned long nowMs) {
  drawTextLine(0, 0, batteryLabel(connectivityStatus), 1);
  drawConnectionIcons(connectivityStatus);

  const String clock = clockLabel(nowMs, connectivityStatus);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  g_display.setTextSize(1);
  g_display.getTextBounds(clock, 0, 0, &x1, &y1, &width, &height);
  drawTextLine(kDisplayWidth - width, 0, clock, 1);
  g_display.drawFastHLine(0, 10, kDisplayWidth, SSD1306_WHITE);
}

void OledStatusDisplay::drawConnectionIcons(const ConnectivityRuntimeStatus &connectivityStatus) {
  constexpr int16_t startX = 46;
  constexpr int16_t topY = 0;
  drawConnectionGlyph(startX, topY, 'U', connectivityStatus.usbConnected);
  drawConnectionGlyph(startX + 13, topY, 'B', connectivityStatus.bluetooth.connected);
  drawConnectionGlyph(startX + 26, topY, 'W', connectivityStatus.wifi.connected);
}

void OledStatusDisplay::drawConnectionGlyph(int16_t x, int16_t y, char label, bool active) {
  constexpr int16_t width = 10;
  constexpr int16_t height = 8;

  if (active) {
    g_display.fillRect(x, y, width, height, SSD1306_WHITE);
    g_display.drawRect(x, y, width, height, SSD1306_WHITE);
    g_display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    g_display.drawRect(x, y, width, height, SSD1306_WHITE);
    g_display.drawLine(x, y + height - 1, x + width - 1, y, SSD1306_WHITE);
    g_display.setTextColor(SSD1306_WHITE);
  }

  g_display.setTextSize(1);
  g_display.setCursor(x + 2, y);
  g_display.print(String(label));
  g_display.setTextColor(SSD1306_WHITE);
}

void OledStatusDisplay::renderScreen(const BootControlStatus &bootStatus,
                                     const LoRaWanRuntimeStatus &lorawanStatus,
                                     const ConnectivityRuntimeStatus &connectivityStatus,
                                     const AutoMintRuntimeStatus &autoMintStatus,
                                     unsigned long nowMs) {
  const DeviceSnapshot &snapshot = state_.snapshot();
  std::array<MenuItem, kMaxMenuItems> items{};
  const size_t count = buildMenuItems(snapshot, items);
  if (count == 0) {
    return;
  }
  if (selectedMenuIndex_ >= count) {
    selectedMenuIndex_ = 0;
  }

  const MenuItem &item = items[selectedMenuIndex_];
  const String title = menuItemTitle(item, snapshot, autoMintStatus);
  const String line1 = menuItemLine1(item, bootStatus, lorawanStatus, connectivityStatus, snapshot, autoMintStatus);
  const String line2 = menuItemLine2(item, lorawanStatus, connectivityStatus, snapshot, autoMintStatus);
  const String line3 = menuItemLine3(item, connectivityStatus, snapshot, autoMintStatus);
  const String footer = menuItemFooter(item);
  const String signature = String(static_cast<int>(item.type)) + "|" + String(item.profileIndex) + "|" +
                           title + "|" + line1 + "|" + line2 + "|" + line3 + "|" + footer + "|" +
                           batteryLabel(connectivityStatus) + "|" + clockLabel(nowMs, connectivityStatus) + "|" +
                           String(connectivityStatus.usbConnected) + "|" + String(connectivityStatus.bluetooth.connected) +
                           "|" + String(connectivityStatus.wifi.connected);
  if (signature == lastRenderedSignature_) {
    return;
  }

  g_display.clearDisplay();
  applyBrightnessProfile();
  drawTopBar(connectivityStatus, nowMs);

  if (item.type == MenuItemType::MainStatus) {
    drawCenteredLine(16, title, 2);
    drawCenteredLine(38, line1, 1);
    drawWrappedLine(0, 50, kDisplayWidth, line2);
  } else if (item.type == MenuItemType::RecentAutoMints) {
    drawCenteredLine(14, title, 1);
    drawWrappedLine(0, 26, kDisplayWidth, line1);
    drawWrappedLine(0, 36, kDisplayWidth, line2);
    drawWrappedLine(0, 46, kDisplayWidth, line3);
    drawCenteredLine(56, footer, 1);
  } else {
    drawCenteredLine(14, title, 1);
    drawWrappedLine(0, 26, kDisplayWidth, line1);
    drawWrappedLine(0, 36, kDisplayWidth, line2);
    drawWrappedLine(0, 46, kDisplayWidth, line3);
    drawCenteredLine(56, footer, 1);
  }

  g_display.display();
  lastRenderedSignature_ = signature;
}

void OledStatusDisplay::drawWrappedLine(int16_t x, int16_t y, int16_t maxWidth, const String &text) {
  String remaining = text;
  remaining.trim();
  if (remaining.length() == 0) {
    return;
  }

  String currentLine;
  while (remaining.length() > 0) {
    const int splitIndex = remaining.indexOf(' ');
    const String token = splitIndex >= 0 ? remaining.substring(0, splitIndex) : remaining;
    const String rest = splitIndex >= 0 ? remaining.substring(splitIndex + 1) : "";
    const String candidate = currentLine.length() > 0 ? currentLine + " " + token : token;

    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    g_display.setTextSize(1);
    g_display.getTextBounds(candidate, x, y, &x1, &y1, &width, &height);
    if (width > maxWidth && currentLine.length() > 0) {
      drawCenteredLine(y, currentLine, 1);
      y += 10;
      currentLine = token;
      remaining = rest;
    } else {
      currentLine = candidate;
      remaining = rest;
    }

    if (splitIndex < 0) {
      break;
    }
  }

  if (currentLine.length() > 0 && y <= (kDisplayHeight - 8)) {
    drawCenteredLine(y, currentLine, 1);
  }
}

String OledStatusDisplay::protocolLabel(const String &protocol) const {
  return formatProtocolLabel(protocol);
}

String OledStatusDisplay::joinStateLabel(const LoRaWanRuntimeStatus &lorawanStatus) const {
  return lorawanStatus.joined ? "JOINED" : "NOT JOINED";
}

String OledStatusDisplay::regionLabel(const LoRaWanRuntimeStatus &lorawanStatus) const {
  String region = lorawanStatus.region;
  region.trim();
  return region.length() > 0 ? region : "EU868";
}

String OledStatusDisplay::batteryLabel(const ConnectivityRuntimeStatus &connectivityStatus) const {
  if (!connectivityStatus.battery.available) {
    return "BAT --";
  }
  return String(connectivityStatus.battery.percent) + "%";
}

String OledStatusDisplay::clockLabel(unsigned long nowMs, const ConnectivityRuntimeStatus &connectivityStatus) const {
  time_t current = time(nullptr);
  if (current > 1700000000) {
    struct tm timeInfo {};
    current += static_cast<time_t>(connectivityStatus.utcOffsetMinutes) * 60;
    gmtime_r(&current, &timeInfo);
    return twoDigitNumber(timeInfo.tm_hour) + ":" + twoDigitNumber(timeInfo.tm_min);
  }

  const long offsetMinutes = static_cast<long>(connectivityStatus.utcOffsetMinutes);
  const long rawMinutes = static_cast<long>(nowMs / 60000UL) + offsetMinutes;
  const long dayMinutes = 24L * 60L;
  const long totalMinutes = ((rawMinutes % dayMinutes) + dayMinutes) % dayMinutes;
  const int hours = static_cast<int>(totalMinutes / 60UL);
  const int minutes = static_cast<int>(totalMinutes % 60UL);
  return twoDigitNumber(hours) + ":" + twoDigitNumber(minutes);
}

String OledStatusDisplay::menuItemTitle(const MenuItem &item,
                                        const DeviceSnapshot &snapshot,
                                        const AutoMintRuntimeStatus &autoMintStatus) const {
  switch (item.type) {
    case MenuItemType::MainStatus:
      return protocolLabel("lora20");
    case MenuItemType::LinksStatus:
      return "LINK STATUS";
    case MenuItemType::ToggleBluetooth:
      return "BLE CONTROL";
    case MenuItemType::ToggleWifi:
      return "WIFI CONTROL";
    case MenuItemType::CycleSleep:
      return "DISPLAY SLEEP";
    case MenuItemType::AutomationStatus:
      return "AUTOMATION";
    case MenuItemType::ToggleAutomation:
      return autoMintStatus.queueMode || !snapshot.config.mintProfiles.empty() ? "QUEUE POWER" : "SINGLE POWER";
    case MenuItemType::CycleInterval:
      return autoMintStatus.queueMode || !snapshot.config.mintProfiles.empty() ? "QUEUE INT" : "SINGLE INT";
    case MenuItemType::ToggleProfile:
      return String("PROFILE ") + String(item.profileIndex + 1);
    case MenuItemType::RecentAutoMints:
      return "LAST AUTO MINTS";
    case MenuItemType::ExitMenu:
      return "EXIT MENU";
  }
  return "STATUS";
}

String OledStatusDisplay::menuItemLine1(const MenuItem &item,
                                        const BootControlStatus &bootStatus,
                                        const LoRaWanRuntimeStatus &lorawanStatus,
                                        const ConnectivityRuntimeStatus &connectivityStatus,
                                        const DeviceSnapshot &snapshot,
                                        const AutoMintRuntimeStatus &autoMintStatus) const {
  switch (item.type) {
    case MenuItemType::MainStatus:
      if (lorawanStatus.lastError.length() > 0) {
        return shortenMiddle(lorawanStatus.lastError, 24);
      }
      if (connectivityStatus.bluetooth.pairing) {
        return String("PIN ") + formatSixDigitPin(connectivityStatus.bluetooth.pin);
      }
      if (connectivityStatus.wifi.connected && connectivityStatus.wifi.ipAddress.length() > 0) {
        return connectivityStatus.wifi.ipAddress;
      }
      return String("FW ") + LORA20_FW_VERSION;
    case MenuItemType::LinksStatus:
      return String("USB: ") + (connectivityStatus.usbConnected ? "ON" : "OFF");
    case MenuItemType::ToggleBluetooth:
      return String("State: ") + connectivityStatus.bluetooth.state;
    case MenuItemType::ToggleWifi:
      return String("State: ") + connectivityStatus.wifi.state;
    case MenuItemType::CycleSleep:
      return String("Now: ") + intervalLabel(snapshot.connectivity.displaySleepSeconds);
    case MenuItemType::AutomationStatus:
      if (autoMintStatus.queueMode || !snapshot.config.mintProfiles.empty()) {
        return String("Queue: ") + (snapshot.config.autoMintEnabled ? "ON" : "OFF");
      }
      return String("Single: ") + (snapshot.config.autoMintEnabled ? "ON" : "OFF");
    case MenuItemType::ToggleAutomation:
      return String("State: ") + (snapshot.config.autoMintEnabled ? "ON" : "OFF");
    case MenuItemType::CycleInterval:
      return String("Now: ") + intervalLabel(snapshot.config.autoMintIntervalSeconds);
    case MenuItemType::ToggleProfile:
      if (item.profileIndex >= 0 && static_cast<size_t>(item.profileIndex) < snapshot.config.mintProfiles.size()) {
        const auto &profile = snapshot.config.mintProfiles[static_cast<size_t>(item.profileIndex)];
        return tickToString(profile.tick) + " " + u64ToString(profile.amount);
      }
      return "Profile missing";
    case MenuItemType::RecentAutoMints:
      return autoMintStatus.recentPackets[0].length() > 0 ? shortenMiddle(autoMintStatus.recentPackets[0], 24)
                                                          : "No auto-mints yet";
    case MenuItemType::ExitMenu:
      return "Hold 2s to return";
  }
  (void)bootStatus;
  return "";
}

String OledStatusDisplay::menuItemLine2(const MenuItem &item,
                                        const LoRaWanRuntimeStatus &lorawanStatus,
                                        const ConnectivityRuntimeStatus &connectivityStatus,
                                        const DeviceSnapshot &snapshot,
                                        const AutoMintRuntimeStatus &autoMintStatus) const {
  switch (item.type) {
    case MenuItemType::MainStatus:
      if (connectivityStatus.bluetooth.pairing) {
        return "Enter on phone or PC";
      }
      return joinStateLabel(lorawanStatus) + " | " + regionLabel(lorawanStatus);
    case MenuItemType::LinksStatus:
      return String("BLE: ") + connectivityStatus.bluetooth.state;
    case MenuItemType::ToggleBluetooth:
      return snapshot.connectivity.bluetoothEnabled ? "Hold 2s: disable BLE" : "Hold 2s: enable BLE";
    case MenuItemType::ToggleWifi:
      return snapshot.connectivity.wifiEnabled ? "Hold 2s: disable Wi-Fi" : "Hold 2s: enable Wi-Fi";
    case MenuItemType::CycleSleep:
      return "Hold 2s: cycle preset";
    case MenuItemType::AutomationStatus:
      return String("Every ") + intervalLabel(autoMintStatus.intervalSeconds);
    case MenuItemType::ToggleAutomation:
      if (autoMintStatus.queueMode || !snapshot.config.mintProfiles.empty()) {
        return String("Profiles: ") + String(autoMintStatus.enabledProfiles) + "/" + String(autoMintStatus.totalProfiles);
      }
      return snapshot.config.defaultTick[0] != '\0'
                 ? tickToString(snapshot.config.defaultTick) + " " + u64ToString(snapshot.config.defaultMintAmount)
                 : "No single mint config";
    case MenuItemType::CycleInterval:
      return "Hold 2s: next interval";
    case MenuItemType::ToggleProfile:
      if (item.profileIndex >= 0 && static_cast<size_t>(item.profileIndex) < snapshot.config.mintProfiles.size()) {
        const auto &profile = snapshot.config.mintProfiles[static_cast<size_t>(item.profileIndex)];
        return String("State: ") + (profile.enabled ? "ENABLED" : "DISABLED");
      }
      return "";
    case MenuItemType::RecentAutoMints:
      return autoMintStatus.recentPackets[1].length() > 0 ? shortenMiddle(autoMintStatus.recentPackets[1], 24) : "";
    case MenuItemType::ExitMenu:
      return "Return to main status";
  }
  return "";
}

String OledStatusDisplay::menuItemLine3(const MenuItem &item,
                                        const ConnectivityRuntimeStatus &connectivityStatus,
                                        const DeviceSnapshot &snapshot,
                                        const AutoMintRuntimeStatus &autoMintStatus) const {
  switch (item.type) {
    case MenuItemType::LinksStatus:
      if (connectivityStatus.wifi.connected && connectivityStatus.wifi.ipAddress.length() > 0) {
        return String("Wi-Fi: ") + connectivityStatus.wifi.ipAddress;
      }
      return String("Wi-Fi: ") + connectivityStatus.wifi.state;
    case MenuItemType::ToggleBluetooth:
      return snapshot.connectivity.bluetoothName[0] != '\0' ? shortenMiddle(String(snapshot.connectivity.bluetoothName), 24)
                                                             : "No BLE name";
    case MenuItemType::ToggleWifi:
      return snapshot.connectivity.wifiSsid[0] != '\0' ? shortenMiddle(String(snapshot.connectivity.wifiSsid), 24)
                                                        : "SSID not set";
    case MenuItemType::CycleSleep:
      return "0 means always on";
    case MenuItemType::AutomationStatus:
      if (autoMintStatus.lastError.length() > 0) {
        return shortenMiddle(autoMintStatus.lastError, 24);
      }
      if (autoMintStatus.queueMode || !snapshot.config.mintProfiles.empty()) {
        return String("Active: ") + String(autoMintStatus.enabledProfiles) + "/" + String(autoMintStatus.totalProfiles);
      }
      return "No queue profiles";
    case MenuItemType::ToggleAutomation:
      return snapshot.config.autoMintEnabled ? "Hold 2s: disable loop" : "Hold 2s: enable loop";
    case MenuItemType::CycleInterval:
      if (autoMintStatus.queueMode || !snapshot.config.mintProfiles.empty()) {
        return String("Queue profiles: ") + String(autoMintStatus.totalProfiles);
      }
      return String("Mint: ") + snapshot.config.defaultTick + " " + u64ToString(snapshot.config.defaultMintAmount);
    case MenuItemType::ToggleProfile:
      return "Hold 2s: toggle";
    case MenuItemType::RecentAutoMints:
      return autoMintStatus.recentPackets[2].length() > 0 ? shortenMiddle(autoMintStatus.recentPackets[2], 24) : "";
    default:
      return "";
  }
}

String OledStatusDisplay::menuItemFooter(const MenuItem &item) const {
  switch (item.type) {
    case MenuItemType::ToggleBluetooth:
    case MenuItemType::ToggleWifi:
    case MenuItemType::CycleSleep:
    case MenuItemType::ToggleAutomation:
    case MenuItemType::CycleInterval:
    case MenuItemType::ToggleProfile:
      return "Short next | Hold 2s";
    case MenuItemType::ExitMenu:
      return "Short next | Hold exit";
    default:
      return "Short press: next";
  }
}

void OledStatusDisplay::selfTest() {
  if (!available_) {
    return;
  }

  applyBrightnessProfile();
  g_display.clearDisplay();
  g_display.fillRect(0, 0, kDisplayWidth, kDisplayHeight, SSD1306_WHITE);
  g_display.display();
  delay(700);

  g_display.clearDisplay();
  g_display.setTextColor(SSD1306_WHITE);
  g_display.drawRect(0, 0, kDisplayWidth, kDisplayHeight, SSD1306_WHITE);
  g_display.drawRect(2, 2, kDisplayWidth - 4, kDisplayHeight - 4, SSD1306_WHITE);
  drawCenteredLine(8, "OLED SELF TEST", 1);
  drawCenteredLine(24, "FULL WHITE", 1);
  drawCenteredLine(40, String("ADDR 0x") + String(kDisplayAddress, HEX), 1);
  g_display.display();
  delay(1200);
}

}  // namespace lora20

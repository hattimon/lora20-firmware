#include "oled_status_display.hpp"

#include <Adafruit_GFX.h>
#define SSD1306_NO_SPLASH
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>
#include <time.h>

namespace {

constexpr uint8_t kDisplayAddress = LORA20_OLED_I2C_ADDR;
constexpr uint16_t kDisplayWidth = 128;
constexpr uint16_t kDisplayHeight = 64;
constexpr int kPrgButtonPin = 0;

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

String formatProtocolLabel(const String &protocol) {
  String normalized = protocol;
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "meshcore") return "MeshCore";
  if (normalized == "meshtastic") return "Meshtastic";
  return "LoRa20";
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

void drawCenteredLine(int16_t y, const String &text, uint8_t size) {
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

String shortenMiddle(const String &text, size_t maxLength) {
  if (text.length() <= maxLength) {
    return text;
  }
  const size_t left = (maxLength / 2) - 1;
  const size_t right = maxLength - left - 3;
  return text.substring(0, left) + "..." + text.substring(text.length() - right);
}

}  // namespace

namespace lora20 {

OledStatusDisplay::OledStatusDisplay(Stream &serial) : serial_(serial) {}

bool OledStatusDisplay::begin(const BootControlStatus &bootStatus,
                              const LoRaWanRuntimeStatus &lorawanStatus,
                              const ConnectivityRuntimeStatus &connectivityStatus,
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

#if defined(LORA20_OLED_SELF_TEST)
  selfTest();
#endif

  renderScreen(bootStatus, lorawanStatus, connectivityStatus, millis());
  return true;
}

void OledStatusDisplay::poll(const BootControlStatus &bootStatus,
                             const LoRaWanRuntimeStatus &lorawanStatus,
                             const ConnectivityRuntimeStatus &connectivityStatus) {
  if (!available_) {
    return;
  }

  const unsigned long nowMs = millis();
  trackWakeButton(nowMs);
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

  renderScreen(bootStatus, lorawanStatus, connectivityStatus, nowMs);
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

void OledStatusDisplay::trackWakeButton(unsigned long nowMs) {
  const bool pressed = digitalRead(kPrgButtonPin) == LOW;
  if (pressed && !lastPrgPressed_) {
    recordActivity(nowMs);
  }
  lastPrgPressed_ = pressed;
}

void OledStatusDisplay::trackLoRaActivity(const LoRaWanRuntimeStatus &lorawanStatus, unsigned long nowMs) {
  bool sawActivity = false;

  if (lorawanStatus.joining || lorawanStatus.queuePending) {
    sawActivity = true;
  }

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

  if (sawActivity) {
    lastLoRaActivityMs_ = nowMs;
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

void OledStatusDisplay::renderScreen(const BootControlStatus &bootStatus,
                                     const LoRaWanRuntimeStatus &lorawanStatus,
                                     const ConnectivityRuntimeStatus &connectivityStatus,
                                     unsigned long nowMs) {
  const String topLeft = batteryLabel(connectivityStatus);
  const String topCenter = connectionLabel(connectivityStatus);
  const String topRight = clockLabel(nowMs, connectivityStatus);

  String title;
  String line1;
  String line2;

  if (lorawanStatus.lastError.length() > 0) {
    title = "RADIO ERROR";
    line1 = shortenMiddle(lorawanStatus.lastError, 28);
    line2 = regionLabel(lorawanStatus);
  } else if (connectivityStatus.lastError.length() > 0) {
    title = "LINK ERROR";
    line1 = shortenMiddle(connectivityStatus.lastError, 28);
    line2 = connectionLabel(connectivityStatus);
  } else if (connectivityStatus.bluetooth.enabled &&
             (connectivityStatus.bluetooth.pairing ||
              (connectivityStatus.activeInterface == "bluetooth" && !connectivityStatus.bluetooth.connected))) {
    if (connectivityStatus.bluetooth.pairing) {
      title = "BLE PAIR";
      line1 = String("PIN ") + formatSixDigitPin(connectivityStatus.bluetooth.pin);
      line2 = "Enter on phone/PC";
    } else {
      title = connectivityStatus.bluetooth.connected ? "BLE LINK" : "BLE READY";
      line1 = shortenMiddle(connectivityStatus.bluetooth.deviceName, 20);
      line2 = connectivityStatus.bluetooth.connected ? "Connected" : "Pair on phone/PC";
    }
  } else if (connectivityStatus.wifi.connected && connectivityStatus.wifi.ipAddress.length() > 0) {
    title = "WIFI IP";
    line1 = connectivityStatus.wifi.ipAddress;
    line2 = shortenMiddle(connectivityStatus.wifi.hostname, 24);
  } else {
    title = protocolLabel(bootStatus.currentProtocol);
    line1 = String("FW ") + LORA20_FW_VERSION;
    line2 = joinStateLabel(lorawanStatus) + " · " + regionLabel(lorawanStatus);
  }

  const String signature = topLeft + "|" + topCenter + "|" + topRight + "|" + title + "|" + line1 + "|" + line2;
  if (signature == lastRenderedSignature_) {
    return;
  }

  applyBrightnessProfile();
  g_display.clearDisplay();
  g_display.setTextColor(SSD1306_WHITE);

  g_display.setTextSize(1);
  g_display.setCursor(0, 0);
  g_display.print(topLeft);
  drawCenteredLine(0, topCenter, 1);
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  g_display.getTextBounds(topRight, 0, 0, &x1, &y1, &width, &height);
  g_display.setCursor(kDisplayWidth - width, 0);
  g_display.print(topRight);
  g_display.drawFastHLine(0, 10, kDisplayWidth, SSD1306_WHITE);

  drawCenteredLine(16, title, 2);
  drawCenteredLine(38, line1, 1);
  drawWrappedLine(0, 50, kDisplayWidth, line2);

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
    int splitIndex = remaining.indexOf(' ');
    String token = splitIndex >= 0 ? remaining.substring(0, splitIndex) : remaining;
    String rest = splitIndex >= 0 ? remaining.substring(splitIndex + 1) : "";
    String candidate = currentLine.length() > 0 ? currentLine + " " + token : token;

    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t width = 0;
    uint16_t height = 0;
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

String OledStatusDisplay::connectionLabel(const ConnectivityRuntimeStatus &connectivityStatus) const {
  const String mode = connectivityStatus.activeInterface;
  if (mode == "wifi") return "WIFI";
  if (mode == "bluetooth") return "BLE";
  if (mode == "usb") return "USB";
  return "NONE";
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

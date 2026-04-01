#include "oled_status_display.hpp"

#include <Adafruit_GFX.h>
#define SSD1306_NO_SPLASH
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t kDisplayAddress = LORA20_OLED_I2C_ADDR;
constexpr uint8_t kDisplayAddresses[] = {0x3C, 0x3D};
constexpr uint16_t kDisplayWidth = 128;
constexpr uint16_t kDisplayHeight = 64;
constexpr unsigned long kRestoreIdleScreenAfterMs = 2500UL;

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

String scanDisplayCandidates() {
  String found;
  for (const uint8_t address : kDisplayAddresses) {
    if (probeAddress(address)) {
      if (found.length() > 0) {
        found += ",";
      }
      found += String("0x") + String(address, HEX);
    }
  }
  return found.length() > 0 ? found : "none";
}

void applyContrastProfile() {
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

String formatProtocolLabel(const String &protocol) {
  String normalized = protocol;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "meshcore") {
    return "MeshCore";
  }
  if (normalized == "meshtastic") {
    return "Meshtastic";
  }
  return "LoRa20";
}

void drawCenteredLine(int16_t y, const String &text, uint8_t size) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;

  g_display.setTextSize(size);
  g_display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  const int16_t x = static_cast<int16_t>((kDisplayWidth - w) / 2);
  g_display.setCursor(x < 0 ? 0 : x, y);
  g_display.print(text);
}

}  // namespace

namespace lora20 {

OledStatusDisplay::OledStatusDisplay(Stream &serial) : serial_(serial) {}

bool OledStatusDisplay::begin(const BootControlStatus &bootStatus,
                              const LoRaWanRuntimeStatus &lorawanStatus,
                              String &error) {
  error = "";

  powerCycleDisplayRail();
  resetDisplayPanel();

  Wire.begin(SDA_OLED, SCL_OLED);
  Wire.setClock(LORA20_OLED_I2C_FREQ);

  const String scanResult = scanDisplayCandidates();
  serial_.printf("[display] i2c scan=%s configured=0x%02x freq=%lu\r\n",
                 scanResult.c_str(),
                 kDisplayAddress,
                 static_cast<unsigned long>(LORA20_OLED_I2C_FREQ));

  if (!g_display.begin(SSD1306_SWITCHCAPVCC, kDisplayAddress, true, false)) {
    error = "OLED begin failed";
    return false;
  }

  if (!probeAddress(kDisplayAddress)) {
    error = String("OLED not detected at 0x") + String(kDisplayAddress, HEX) +
            " scan=" + scanResult;
    return false;
  }

  applyBrightnessProfile();
  available_ = true;

#if defined(LORA20_OLED_SELF_TEST)
  selfTest();
#endif

  renderIdleScreen(bootStatus, lorawanStatus);
  serial_.printf("[display] init ok addr=0x%02x\r\n", kDisplayAddress);
  return true;
}

void OledStatusDisplay::poll(const BootControlStatus &bootStatus,
                             const LoRaWanRuntimeStatus &lorawanStatus) {
  if (!available_) {
    return;
  }

  const unsigned long nowMs = millis();
  trackLoRaActivity(lorawanStatus, nowMs);
  const String nextProtocol = protocolLabel(bootStatus.currentProtocol);
  const String nextRegion = regionLabel(lorawanStatus);
  const String nextJoinState = joinStateLabel(lorawanStatus);

  const bool screenContentChanged =
      idleScreenVisible_ &&
      (nextProtocol != lastRenderedProtocol_ || nextRegion != lastRenderedRegion_ ||
       nextJoinState != lastRenderedJoinState_);

  if (screenContentChanged) {
    renderIdleScreen(bootStatus, lorawanStatus);
    return;
  }

  if (idleScreenVisible_) {
    return;
  }

  if ((nowMs - lastLoRaActivityMs_) < kRestoreIdleScreenAfterMs) {
    return;
  }

  renderIdleScreen(bootStatus, lorawanStatus);
}

void OledStatusDisplay::applyBrightnessProfile() {
  applyContrastProfile();
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
  delay(1400);
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

  if (!sawActivity) {
    return;
  }

  lastLoRaActivityMs_ = nowMs;
  idleScreenVisible_ = false;
}

void OledStatusDisplay::renderIdleScreen(const BootControlStatus &bootStatus,
                                         const LoRaWanRuntimeStatus &lorawanStatus) {
  if (!probeAddress(kDisplayAddress)) {
    available_ = false;
    return;
  }

  const String currentProtocol = protocolLabel(bootStatus.currentProtocol);
  const String firmwareVersion = String("FW ") + LORA20_FW_VERSION;
  const String region = regionLabel(lorawanStatus);
  const String joinState = joinStateLabel(lorawanStatus);

  applyBrightnessProfile();
  g_display.clearDisplay();
  g_display.setTextColor(SSD1306_WHITE);

  drawCenteredLine(6, currentProtocol, 2);
  drawCenteredLine(30, firmwareVersion, 1);
  drawCenteredLine(42, region, 1);
  drawCenteredLine(54, joinState, 1);

  g_display.display();

  idleScreenVisible_ = true;
  lastRenderedProtocol_ = currentProtocol;
  lastRenderedRegion_ = region;
  lastRenderedJoinState_ = joinState;
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
  if (region.length() == 0) {
    return "EU868";
  }
  return region;
}

}  // namespace lora20

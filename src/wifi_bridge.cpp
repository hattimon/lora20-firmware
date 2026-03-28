#include "wifi_bridge.hpp"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <cstdio>

namespace {

constexpr const char *kApPassword = "lora20-setup";
constexpr unsigned long kStaTimeoutMs = 12000;

String buildSsid() {
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08llx", static_cast<unsigned long long>(chipId & 0xFFFFFFFFULL));
  String ssid = String("LORA20-") + String(suffix).substring(4);
  return ssid;
}

String buildHostname() {
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08llx", static_cast<unsigned long long>(chipId & 0xFFFFFFFFULL));
  String name = String("lora20-") + String(suffix).substring(4);
  name.toLowerCase();
  return name;
}

}  // namespace

namespace lora20 {

WifiBridge::WifiBridge(RpcProcessor &processor)
    : processor_(processor),
      server_(80) {}

void WifiBridge::begin() {
  apSsid_ = buildSsid();
  hostname_ = buildHostname();
  server_.on("/", HTTP_GET, [this]() { handleHealth(); });
  server_.on("/health", HTTP_GET, [this]() { handleHealth(); });
  server_.on("/rpc", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.on("/rpc", HTTP_POST, [this]() { handleRpc(); });
}

void WifiBridge::poll() {
  updateConnectionState();
  if (serverStarted_) {
    server_.handleClient();
  }
}

void WifiBridge::applyConfig(const ConnectionConfig &config, bool enabled) {
  const bool shouldEnable = enabled;
  const String ssid = String(config.wifiSsid);
  const String pass = String(config.wifiPassword);
  const bool wantsSta = ssid.length() > 0;
  allowApFallback_ = config.wifiApFallback;

  if (!shouldEnable) {
    if (enabled_) {
      stopWifi();
    }
    return;
  }

  enabled_ = true;
  if (!serverStarted_) {
    ensureServer();
  }

  if (wantsSta != wantsSta_ || ssid != staSsid_ || pass != staPass_ || mode_ == Mode::kOff) {
    staSsid_ = ssid;
    staPass_ = pass;
    wantsSta_ = wantsSta;
    if (wantsSta_) {
      startSta();
    } else if (allowApFallback_) {
      startAp();
    } else {
      stopWifi();
    }
  }
}

String WifiBridge::ipAddress() const {
  if (mode_ == Mode::kStaConnected || mode_ == Mode::kStaConnecting) {
    return WiFi.localIP().toString();
  }
  if (mode_ == Mode::kAp) {
    return WiFi.softAPIP().toString();
  }
  return "";
}

String WifiBridge::modeLabel() const {
  switch (mode_) {
    case Mode::kAp:
      return "ap";
    case Mode::kStaConnecting:
      return "sta_connecting";
    case Mode::kStaConnected:
      return "sta_connected";
    case Mode::kOff:
    default:
      return "off";
  }
}

String WifiBridge::hostname() const {
  return hostname_;
}

bool WifiBridge::isEnabled() const {
  return enabled_;
}

unsigned long WifiBridge::lastActivityMs() const {
  return lastActivityMs_;
}

bool WifiBridge::isClientActive(unsigned long nowMs, unsigned long timeoutMs) const {
  if (lastActivityMs_ == 0) return false;
  return nowMs >= lastActivityMs_ && (nowMs - lastActivityMs_) <= timeoutMs;
}

void WifiBridge::startAp() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid_.c_str(), kApPassword);
  mode_ = Mode::kAp;
  connectStartedMs_ = 0;
  mdnsStarted_ = false;
}

void WifiBridge::startSta() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(staSsid_.c_str(), staPass_.c_str());
  mode_ = Mode::kStaConnecting;
  connectStartedMs_ = millis();
  mdnsStarted_ = false;
}

void WifiBridge::stopWifi() {
  if (serverStarted_) {
    server_.stop();
    serverStarted_ = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  enabled_ = false;
  wantsSta_ = false;
  mode_ = Mode::kOff;
  mdnsStarted_ = false;
}

void WifiBridge::ensureServer() {
  if (!serverStarted_) {
    server_.begin();
    serverStarted_ = true;
  }
}

void WifiBridge::updateConnectionState() {
  if (!enabled_) return;
  if (mode_ == Mode::kStaConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      mode_ = Mode::kStaConnected;
      startMdnsIfNeeded();
      return;
    }
    if (connectStartedMs_ > 0 && (millis() - connectStartedMs_) > kStaTimeoutMs) {
      if (allowApFallback_) {
        startAp();
      } else {
        startSta();
      }
    }
  } else if (mode_ == Mode::kStaConnected && WiFi.status() != WL_CONNECTED) {
    if (wantsSta_) {
      startSta();
    } else if (allowApFallback_) {
      startAp();
    } else {
      stopWifi();
    }
  } else if (mode_ == Mode::kStaConnected) {
    startMdnsIfNeeded();
  }
}

void WifiBridge::startMdnsIfNeeded() {
  if (mdnsStarted_ || mode_ != Mode::kStaConnected) return;
  if (hostname_.length() == 0) return;
  if (MDNS.begin(hostname_.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("lora20", "tcp", 80);
    mdnsStarted_ = true;
  }
}

void WifiBridge::sendCorsHeaders() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void WifiBridge::sendJson(int code, const String &payload) {
  sendCorsHeaders();
  server_.send(code, "application/json", payload);
}

void WifiBridge::handleOptions() {
  lastActivityMs_ = millis();
  sendCorsHeaders();
  server_.send(204);
}

void WifiBridge::handleHealth() {
  lastActivityMs_ = millis();
  DynamicJsonDocument response(384);
  response["status"] = "ok";
  response["service"] = "lora20-device";
  response["mode"] = modeLabel();
  response["ssid"] = (mode_ == Mode::kAp) ? apSsid_ : staSsid_;
  response["ip"] = ipAddress();
  response["hostname"] = hostname_;
  response["apFallback"] = allowApFallback_;
  response["apPassword"] = (mode_ == Mode::kAp) ? kApPassword : "";
  String payload;
  serializeJson(response, payload);
  sendJson(200, payload);
}

void WifiBridge::handleRpc() {
  lastActivityMs_ = millis();
  const String body = server_.arg("plain");
  String response;
  if (!processor_.handleLine(body, response, true)) {
    DynamicJsonDocument fallback(256);
    fallback["ok"] = false;
    JsonObject error = fallback.createNestedObject("error");
    error["code"] = "empty_request";
    error["message"] = "RPC body is empty";
    serializeJson(fallback, response);
  }
  sendJson(200, response);
}

}  // namespace lora20

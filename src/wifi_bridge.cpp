#include "wifi_bridge.hpp"

#include <ArduinoJson.h>
#include <cstdio>

namespace {

constexpr const char *kApPassword = "lora20-setup";

String buildSsid() {
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%08llx", static_cast<unsigned long long>(chipId & 0xFFFFFFFFULL));
  String ssid = String("LORA20-") + String(suffix).substring(4);
  return ssid;
}

}  // namespace

namespace lora20 {

WifiBridge::WifiBridge(RpcProcessor &processor)
    : processor_(processor),
      server_(80) {}

void WifiBridge::begin() {
  ssid_ = buildSsid();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid_.c_str(), kApPassword);

  server_.on("/", HTTP_GET, [this]() { handleHealth(); });
  server_.on("/health", HTTP_GET, [this]() { handleHealth(); });
  server_.on("/rpc", HTTP_OPTIONS, [this]() { handleOptions(); });
  server_.on("/rpc", HTTP_POST, [this]() { handleRpc(); });
  server_.begin();
}

void WifiBridge::poll() {
  server_.handleClient();
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
  sendCorsHeaders();
  server_.send(204);
}

void WifiBridge::handleHealth() {
  DynamicJsonDocument response(256);
  response["status"] = "ok";
  response["service"] = "lora20-device";
  response["ssid"] = ssid_;
  response["ip"] = WiFi.softAPIP().toString();
  String payload;
  serializeJson(response, payload);
  sendJson(200, payload);
}

void WifiBridge::handleRpc() {
  const String body = server_.arg("plain");
  String response;
  if (!processor_.handleLine(body, response)) {
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

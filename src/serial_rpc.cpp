#include "serial_rpc.hpp"

#include <ArduinoJson.h>

namespace {

constexpr size_t kLineLimit = 3072;

}  // namespace

namespace lora20 {

SerialRpcServer::SerialRpcServer(Stream &serial, RpcProcessor &processor)
    : serial_(serial), processor_(processor) {}

void SerialRpcServer::begin() {
  buffer_.reserve(kLineLimit);
  sendBootEvent();
}

void SerialRpcServer::poll() {
  while (serial_.available() > 0) {
    const char ch = static_cast<char>(serial_.read());

    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      const String line = buffer_;
      buffer_.clear();
      handleLine(line);
      continue;
    }

    if (buffer_.length() >= kLineLimit) {
      buffer_.clear();
      DynamicJsonDocument response(256);
      response["ok"] = false;
      JsonObject error = response.createNestedObject("error");
      error["code"] = "line_too_long";
      error["message"] = "Incoming serial line exceeded the size limit";
      serializeJson(response, serial_);
      serial_.println();
      continue;
    }

    buffer_ += ch;
  }
}

void SerialRpcServer::handleLine(const String &line) {
  String response;
  if (!processor_.handleLine(line, response, false)) {
    return;
  }
  if (response.length() > 0) {
    serial_.println(response);
  }
}

void SerialRpcServer::sendBootEvent() {
  String response;
  processor_.buildBootEvent(response);
  if (response.length() > 0) {
    serial_.println(response);
  }
}

}  // namespace lora20

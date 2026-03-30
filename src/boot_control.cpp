#include "boot_control.hpp"

#include <ArduinoJson.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace {

constexpr int kPrgButtonPin = 0;
constexpr unsigned long kMeshCoreHoldMs = 3000UL;
constexpr unsigned long kMeshtasticHoldMs = 6000UL;

#ifdef LORA20_TRIBOOT
constexpr bool kTriBootBuildEnabled = true;
#else
constexpr bool kTriBootBuildEnabled = false;
#endif

struct SlotDefinition {
  const char *protocol;
  const char *label;
  esp_partition_subtype_t subtype;
};

struct HoldTargetDefinition {
  const char *shortHoldProtocol;
  const char *longHoldProtocol;
};

constexpr SlotDefinition kSlots[] = {
    {"lora20", "lora20", ESP_PARTITION_SUBTYPE_APP_OTA_0},
    {"meshcore", "meshcore", ESP_PARTITION_SUBTYPE_APP_OTA_1},
    {"meshtastic", "meshtastic", ESP_PARTITION_SUBTYPE_APP_OTA_2},
};

String partitionSubtypeName(esp_partition_subtype_t subtype) {
  switch (subtype) {
    case ESP_PARTITION_SUBTYPE_APP_FACTORY:
      return "factory";
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:
      return "ota_0";
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:
      return "ota_1";
    case ESP_PARTITION_SUBTYPE_APP_OTA_2:
      return "ota_2";
    default:
      return "other";
  }
}

String normalizeProtocolName(const String &value) {
  String normalized = value;
  normalized.trim();
  normalized.toLowerCase();
  return normalized;
}

const SlotDefinition *findSlotDefinition(const String &protocol) {
  const String normalized = normalizeProtocolName(protocol);
  for (const auto &slot : kSlots) {
    if (normalized == slot.protocol) {
      return &slot;
    }
  }
  return nullptr;
}

HoldTargetDefinition holdTargetsForProtocol(const String &protocol) {
  const String normalized = normalizeProtocolName(protocol);
  if (normalized == "meshcore") {
    return {"lora20", "meshtastic"};
  }
  if (normalized == "meshtastic") {
    return {"lora20", "meshcore"};
  }
  return {"meshcore", "meshtastic"};
}

String buildButtonHint(const String &currentProtocol) {
  const HoldTargetDefinition targets = holdTargetsForProtocol(currentProtocol);
  return String("Hold PRG for 3-5.9s, release to switch to ") + targets.shortHoldProtocol +
         ". Hold for 6s+, release to switch to " + targets.longHoldProtocol;
}

String protocolFromPartition(const esp_partition_t *partition) {
  if (partition == nullptr) {
    return "";
  }

  for (const auto &slot : kSlots) {
    if (partition->subtype == slot.subtype || String(partition->label) == slot.label) {
      return String(slot.protocol);
    }
  }

  return String(partition->label);
}

void writeDocument(Stream &serial, const JsonDocument &document) {
  serializeJson(document, serial);
  serial.println();
}

String appDescriptionField(const char *field) {
  return (field != nullptr && field[0] != '\0') ? String(field) : String("");
}

}  // namespace

namespace lora20 {

BootControl::BootControl(Stream &serial) : serial_(serial) {}

bool BootControl::begin(String &error) {
  error = "";
  pinMode(kPrgButtonPin, INPUT_PULLUP);
  status_.supported = kTriBootBuildEnabled;
  status_.buttonHint = kTriBootBuildEnabled ? buildButtonHint("lora20")
                                            : "Tri-boot is disabled in this build";
  statusDirty_ = true;
  refreshStatus();
  return true;
}

void BootControl::poll() {
  if (!status().supported) {
    return;
  }

  const bool pressed = digitalRead(kPrgButtonPin) == LOW;
  const unsigned long nowMs = millis();

  if (pressed && !buttonPressed_) {
    buttonPressed_ = true;
    buttonDownSinceMs_ = nowMs;
    buttonHintStage_ = 0;
    return;
  }

  if (!pressed && buttonPressed_) {
    const unsigned long heldMs = nowMs - buttonDownSinceMs_;
    buttonPressed_ = false;
    buttonDownSinceMs_ = 0;
    buttonHintStage_ = 0;

    const HoldTargetDefinition targets = holdTargetsForProtocol(status().currentProtocol);
    String error;
    if (heldMs >= kMeshtasticHoldMs) {
      if (!switchToProtocol(targets.longHoldProtocol, true, error)) {
        emitSwitchEvent("button_switch_failed", targets.longHoldProtocol, error, false);
      }
    } else if (heldMs >= kMeshCoreHoldMs) {
      if (!switchToProtocol(targets.shortHoldProtocol, true, error)) {
        emitSwitchEvent("button_switch_failed", targets.shortHoldProtocol, error, false);
      }
    }
    return;
  }

  if (!pressed || !buttonPressed_) {
    return;
  }

  const unsigned long heldMs = nowMs - buttonDownSinceMs_;
  const HoldTargetDefinition targets = holdTargetsForProtocol(status().currentProtocol);
  if (buttonHintStage_ == 0 && heldMs >= kMeshCoreHoldMs) {
    emitButtonHint(targets.shortHoldProtocol, heldMs);
    buttonHintStage_ = 1;
    return;
  }

  if (buttonHintStage_ == 1 && heldMs >= kMeshtasticHoldMs) {
    emitButtonHint(targets.longHoldProtocol, heldMs);
    buttonHintStage_ = 2;
  }
}

const BootControlStatus &BootControl::status() {
  if (statusDirty_) {
    refreshStatus();
  }
  return status_;
}

bool BootControl::switchToProtocol(const String &protocol, bool rebootNow, String &error) {
  if (!status().supported) {
    error = "Tri-boot is disabled in this firmware build";
    return false;
  }

  const SlotDefinition *slot = findSlotDefinition(protocol);
  if (slot == nullptr) {
    error = "Unknown protocol target";
    return false;
  }

  const esp_partition_t *partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, slot->subtype, slot->label);
  if (partition == nullptr) {
    error = "Target partition is missing from the current flash layout";
    return false;
  }

  esp_app_desc_t description{};
  if (esp_ota_get_partition_description(partition, &description) != ESP_OK) {
    error = "Target partition does not contain a valid firmware image yet";
    return false;
  }

  const esp_err_t rc = esp_ota_set_boot_partition(partition);
  if (rc != ESP_OK) {
    error = String("Failed to set boot partition: ") + esp_err_to_name(rc);
    return false;
  }

  statusDirty_ = true;
  refreshStatus();
  emitSwitchEvent("switch_requested",
                  slot->protocol,
                  String("Boot target set to ") + slot->label + " (" + appDescriptionField(description.version) + ")",
                  rebootNow);

  if (rebootNow) {
    delay(50);
    serial_.flush();
    ESP.restart();
  }

  return true;
}

void BootControl::refreshStatus() {
  status_.supported = kTriBootBuildEnabled;
  status_.currentProtocol = "";
  status_.bootProtocol = "";
  status_.runningPartitionLabel = "";
  status_.bootPartitionLabel = "";
  status_.buttonHint = kTriBootBuildEnabled ? buildButtonHint("lora20")
                                            : "Tri-boot is disabled in this build";

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();

  if (running != nullptr) {
    status_.currentProtocol = protocolFromPartition(running);
    status_.runningPartitionLabel = String(running->label);
    status_.buttonHint = buildButtonHint(status_.currentProtocol);
  }
  if (boot != nullptr) {
    status_.bootProtocol = protocolFromPartition(boot);
    status_.bootPartitionLabel = String(boot->label);
  }

  for (size_t index = 0; index < kBootSlotCount; ++index) {
    BootSlotInfo &slotInfo = status_.slots[index];
    slotInfo = BootSlotInfo();
    slotInfo.protocol = kSlots[index].protocol;
    slotInfo.partitionLabel = kSlots[index].label;
    slotInfo.subtype = partitionSubtypeName(kSlots[index].subtype);

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, kSlots[index].subtype, kSlots[index].label);
    if (partition == nullptr) {
      continue;
    }

    slotInfo.partitionPresent = true;
    slotInfo.address = partition->address;
    slotInfo.sizeBytes = partition->size;
    slotInfo.running = (running != nullptr) && (running->address == partition->address);
    slotInfo.bootTarget = (boot != nullptr) && (boot->address == partition->address);

    esp_app_desc_t description{};
    if (esp_ota_get_partition_description(partition, &description) == ESP_OK) {
      slotInfo.validImage = true;
      slotInfo.projectName = appDescriptionField(description.project_name);
      slotInfo.version = appDescriptionField(description.version);
    }
  }

  statusDirty_ = false;
}

void BootControl::emitButtonHint(const String &protocol, unsigned long holdMs) {
  DynamicJsonDocument event(256);
  event["type"] = "boot_switch_hint";
  event["protocol"] = protocol;
  event["holdMs"] = holdMs;
  event["message"] = String("Release PRG now to switch to ") + protocol;
  writeDocument(serial_, event);
}

void BootControl::emitSwitchEvent(const char *stage,
                                  const String &protocol,
                                  const String &message,
                                  bool rebootNow) {
  DynamicJsonDocument event(320);
  event["type"] = "boot_switch";
  event["stage"] = stage != nullptr ? stage : "unknown";
  event["protocol"] = protocol;
  event["rebootNow"] = rebootNow;
  event["message"] = message;
  writeDocument(serial_, event);
}

}  // namespace lora20

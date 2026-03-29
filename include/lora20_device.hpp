#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lora20 {

constexpr const char *kFirmwareName = "lora20-device";
constexpr const char *kBackupAlgorithm = "AES-256-GCM+PBKDF2-SHA256";

constexpr uint8_t kOpDeploy = 0x01;
constexpr uint8_t kOpMint = 0x02;
constexpr uint8_t kOpTransfer = 0x03;
constexpr uint8_t kOpConfig = 0x10;
constexpr size_t kMaxMintProfiles = 8;

enum class ConnectionMode : uint8_t {
  kUsb = 0,
  kBle = 1,
  kWifi = 2
};

struct MintProfile {
  char tick[5];
  uint64_t amount = 1;
  bool enabled = true;

  MintProfile();
};

struct ConnectionConfig {
  ConnectionMode mode = ConnectionMode::kUsb;
  char wifiSsid[33];
  char wifiPassword[65];
  char rpcToken[65];
  bool wifiApFallback = false;
  uint16_t displaySleepSeconds = 60;
  uint8_t displayBrightness = 255;
  uint16_t bridgeWindowSeconds = 300;
  uint8_t powerSaveLevel = 1;

  ConnectionConfig();
};

struct DeviceConfig {
  bool autoMintEnabled = false;
  uint32_t autoMintIntervalSeconds = 1800;
  char defaultTick[5];
  uint64_t defaultMintAmount = 1;
  std::vector<MintProfile> mintProfiles;

  DeviceConfig();
};

struct BackupBlob {
  uint8_t version = 1;
  String algorithm = kBackupAlgorithm;
  String saltHex;
  String ivHex;
  String ciphertextHex;
  String tagHex;
  String deviceId;
};

struct LoRaWanConfig {
  bool autoDevEui = true;
  bool adr = true;
  bool confirmedUplink = false;
  uint8_t appPort = 1;
  uint8_t defaultDataRate = 3;
  bool hasDevEui = false;
  bool hasJoinEui = false;
  bool hasAppKey = false;
  std::array<uint8_t, 8> devEui{};
  std::array<uint8_t, 8> joinEui{};
  std::array<uint8_t, 16> appKey{};
};

struct HeltecLicenseConfig {
  bool hasLicense = false;
  std::array<uint8_t, 16> value{};
};

struct DeviceSnapshot {
  bool hasKey = false;
  std::array<uint8_t, 32> seed{};
  std::array<uint8_t, 32> publicKey{};
  std::array<uint8_t, 64> privateKey{};
  std::array<uint8_t, 8> deviceId{};
  uint32_t nextNonce = 1;
  ConnectionConfig connection;
  DeviceConfig config;
  LoRaWanConfig loRaWan;
  HeltecLicenseConfig heltecLicense;
};

struct PreparedPayload {
  uint32_t nonce = 0;
  bool committed = false;
  std::vector<uint8_t> unsignedPayload;
  std::array<uint8_t, 64> signature{};
  std::vector<uint8_t> payload;
};

class DeviceStateStore {
 public:
  DeviceStateStore();

  bool begin(String &error);
  const DeviceSnapshot &snapshot() const;

  bool generateKey(bool force, String &error);
  bool exportBackup(const String &passphrase, BackupBlob &backup, String &error) const;
  bool importBackup(const BackupBlob &backup, const String &passphrase, bool overwrite, String &error);

  bool updateConfig(const DeviceConfig &config, String &error);
  bool updateConnectionConfig(const ConnectionConfig &config, String &error);
  bool updateLoRaWanConfig(const LoRaWanConfig &config, String &error);
  bool updateHeltecLicense(const HeltecLicenseConfig &config, String &error);
  bool commitNonce(uint32_t usedNonce, String &error);
  bool applyCommittedConfig(const DeviceConfig &config, uint32_t usedNonce, String &error);

 private:
  bool loadSnapshot(String &error);
  bool persistSeed(String &error);
  bool persistConfig(String &error);
  bool persistConnectionConfig(String &error);
  bool persistLoRaWanConfig(String &error);
  bool persistHeltecLicense(String &error);
  bool persistNonce(String &error);
  bool persistAll(String &error);
  bool deriveKeyMaterial(String &error);

  Preferences prefs_;
  DeviceSnapshot snapshot_;
};

bool normalizeTick(const String &input, char output[5], String &error);
String tickToString(const char tick[5]);
String connectionModeToString(ConnectionMode mode);
bool parseConnectionMode(const String &input, ConnectionMode &out);
bool parseUint64(const String &text, uint64_t &value);
bool parseUint32(const String &text, uint32_t &value);
String toHex(const uint8_t *data, size_t length);
String toHex(const std::vector<uint8_t> &bytes);
bool hexToBytes(const String &input, uint8_t *out, size_t expectedLength);
void fillRandomBytes(uint8_t *target, size_t length);

bool buildDeployPayload(const DeviceSnapshot &snapshot,
                        const char tick[5],
                        uint64_t maxSupply,
                        uint64_t limitPerMint,
                        uint32_t nonce,
                        PreparedPayload &prepared,
                        String &error);

bool buildMintPayload(const DeviceSnapshot &snapshot,
                      const char tick[5],
                      uint64_t amount,
                      uint32_t nonce,
                      PreparedPayload &prepared,
                      String &error);

bool buildTransferPayload(const DeviceSnapshot &snapshot,
                          const char tick[5],
                          uint64_t amount,
                          const String &recipientDeviceIdHex,
                          uint32_t nonce,
                          PreparedPayload &prepared,
                          String &error);

bool buildConfigPayload(const DeviceSnapshot &snapshot,
                        const DeviceConfig &config,
                        uint32_t nonce,
                        PreparedPayload &prepared,
                        String &error);

template <size_t N>
String toHex(const std::array<uint8_t, N> &bytes) {
  return toHex(bytes.data(), bytes.size());
}

}  // namespace lora20

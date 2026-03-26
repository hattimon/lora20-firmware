#include "lora20_device.hpp"

#include <ed25519.h>
#include <esp_system.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *kPrefsNamespace = "lora20";
constexpr const char *kSeedKey = "seed";
constexpr const char *kNonceKey = "nonce";
constexpr const char *kAutoMintEnabledKey = "am_en";
constexpr const char *kAutoMintIntervalKey = "am_int";
constexpr const char *kDefaultTickKey = "df_tick";
constexpr const char *kDefaultAmountKey = "df_amt";

constexpr uint8_t kBackupVersion = 1;
constexpr size_t kBackupSaltLength = 16;
constexpr size_t kBackupIvLength = 12;
constexpr size_t kBackupTagLength = 16;
constexpr size_t kBackupPlaintextLength = 58;
constexpr uint32_t kBackupPbkdfIterations = 60000;

void writeUint32BE(uint32_t value, uint8_t *target) {
  target[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  target[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  target[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  target[3] = static_cast<uint8_t>(value & 0xFF);
}

void writeUint64BE(uint64_t value, uint8_t *target) {
  for (int index = 7; index >= 0; --index) {
    target[7 - index] = static_cast<uint8_t>((value >> (index * 8)) & 0xFF);
  }
}

uint32_t readUint32BE(const uint8_t *source) {
  return (static_cast<uint32_t>(source[0]) << 24) |
         (static_cast<uint32_t>(source[1]) << 16) |
         (static_cast<uint32_t>(source[2]) << 8) |
         static_cast<uint32_t>(source[3]);
}

uint64_t readUint64BE(const uint8_t *source) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<uint64_t>(source[index]);
  }
  return value;
}

bool deriveBackupKey(const String &passphrase,
                     const uint8_t *salt,
                     size_t saltLength,
                     std::array<uint8_t, 32> &key,
                     String &error) {
  mbedtls_md_context_t mdContext;
  mbedtls_md_init(&mdContext);

  const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mdInfo == nullptr) {
    error = "PBKDF2 SHA-256 is unavailable";
    mbedtls_md_free(&mdContext);
    return false;
  }

  if (mbedtls_md_setup(&mdContext, mdInfo, 1) != 0) {
    error = "Failed to initialize PBKDF2 context";
    mbedtls_md_free(&mdContext);
    return false;
  }

  const int rc = mbedtls_pkcs5_pbkdf2_hmac(
      &mdContext,
      reinterpret_cast<const unsigned char *>(passphrase.c_str()),
      passphrase.length(),
      salt,
      saltLength,
      kBackupPbkdfIterations,
      key.size(),
      key.data());

  mbedtls_md_free(&mdContext);

  if (rc != 0) {
    error = "PBKDF2 key derivation failed";
    return false;
  }

  return true;
}

bool encryptAesGcm(const std::vector<uint8_t> &plaintext,
                   const std::array<uint8_t, 32> &key,
                   const std::array<uint8_t, kBackupIvLength> &iv,
                   std::vector<uint8_t> &ciphertext,
                   std::array<uint8_t, kBackupTagLength> &tag,
                   String &error) {
  ciphertext.resize(plaintext.size());

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.data(), 256) != 0) {
    error = "Failed to initialize AES-GCM encryption";
    mbedtls_gcm_free(&gcm);
    return false;
  }

  const int rc = mbedtls_gcm_crypt_and_tag(&gcm,
                                           MBEDTLS_GCM_ENCRYPT,
                                           plaintext.size(),
                                           iv.data(),
                                           iv.size(),
                                           nullptr,
                                           0,
                                           plaintext.data(),
                                           ciphertext.data(),
                                           tag.size(),
                                           tag.data());
  mbedtls_gcm_free(&gcm);

  if (rc != 0) {
    error = "AES-GCM encryption failed";
    return false;
  }

  return true;
}

bool decryptAesGcm(const std::vector<uint8_t> &ciphertext,
                   const std::array<uint8_t, 32> &key,
                   const std::array<uint8_t, kBackupIvLength> &iv,
                   const std::array<uint8_t, kBackupTagLength> &tag,
                   std::vector<uint8_t> &plaintext,
                   String &error) {
  plaintext.resize(ciphertext.size());

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.data(), 256) != 0) {
    error = "Failed to initialize AES-GCM decryption";
    mbedtls_gcm_free(&gcm);
    return false;
  }

  const int rc = mbedtls_gcm_auth_decrypt(&gcm,
                                          ciphertext.size(),
                                          iv.data(),
                                          iv.size(),
                                          nullptr,
                                          0,
                                          tag.data(),
                                          tag.size(),
                                          ciphertext.data(),
                                          plaintext.data());
  mbedtls_gcm_free(&gcm);

  if (rc != 0) {
    error = "Backup decrypt failed, passphrase or blob is invalid";
    return false;
  }

  return true;
}

}  // namespace

namespace lora20 {

DeviceConfig::DeviceConfig() {
  std::memcpy(defaultTick, "LORA", 5);
}

DeviceStateStore::DeviceStateStore() = default;

bool DeviceStateStore::begin(String &error) {
  if (!prefs_.begin(kPrefsNamespace, false)) {
    error = "Failed to open NVS namespace";
    return false;
  }

  return loadSnapshot(error);
}

const DeviceSnapshot &DeviceStateStore::snapshot() const {
  return snapshot_;
}

bool DeviceStateStore::loadSnapshot(String &error) {
  snapshot_ = DeviceSnapshot();
  snapshot_.nextNonce = prefs_.getUInt(kNonceKey, 1);
  if (snapshot_.nextNonce == 0) {
    snapshot_.nextNonce = 1;
  }

  snapshot_.config.autoMintEnabled = prefs_.getBool(kAutoMintEnabledKey, false);
  snapshot_.config.autoMintIntervalSeconds = prefs_.getUInt(kAutoMintIntervalKey, 1800);

  String tick = prefs_.getString(kDefaultTickKey, "LORA");
  if (!normalizeTick(tick, snapshot_.config.defaultTick, error)) {
    std::memcpy(snapshot_.config.defaultTick, "LORA", 5);
    error = "";
  }

  const String amountText = prefs_.getString(kDefaultAmountKey, "1");
  uint64_t amount = 1;
  if (parseUint64(amountText, amount)) {
    snapshot_.config.defaultMintAmount = amount;
  }

  const size_t seedLength = prefs_.getBytesLength(kSeedKey);
  if (seedLength == 0) {
    snapshot_.hasKey = false;
    return true;
  }

  if (seedLength != snapshot_.seed.size()) {
    error = "Stored device seed has invalid length";
    return false;
  }

  if (prefs_.getBytes(kSeedKey, snapshot_.seed.data(), snapshot_.seed.size()) != snapshot_.seed.size()) {
    error = "Failed to load device seed from NVS";
    return false;
  }

  snapshot_.hasKey = true;
  return deriveKeyMaterial(error);
}

bool DeviceStateStore::generateKey(bool force, String &error) {
  if (snapshot_.hasKey && !force) {
    error = "Device key already exists; use force=true to replace it";
    return false;
  }

  fillRandomBytes(snapshot_.seed.data(), snapshot_.seed.size());
  snapshot_.hasKey = true;
  snapshot_.nextNonce = 1;

  if (!deriveKeyMaterial(error)) {
    snapshot_.hasKey = false;
    return false;
  }

  return persistAll(error);
}

bool DeviceStateStore::exportBackup(const String &passphrase, BackupBlob &backup, String &error) const {
  if (!snapshot_.hasKey) {
    error = "No device key is present";
    return false;
  }

  if (passphrase.length() < 8) {
    error = "Passphrase must be at least 8 characters";
    return false;
  }

  std::vector<uint8_t> plaintext(kBackupPlaintextLength, 0);
  plaintext[0] = 'L';
  plaintext[1] = '2';
  plaintext[2] = '0';
  plaintext[3] = 'B';
  plaintext[4] = kBackupVersion;
  std::memcpy(&plaintext[5], snapshot_.seed.data(), snapshot_.seed.size());
  writeUint32BE(snapshot_.nextNonce, &plaintext[37]);
  plaintext[41] = snapshot_.config.autoMintEnabled ? 0x01 : 0x00;
  writeUint32BE(snapshot_.config.autoMintIntervalSeconds, &plaintext[42]);
  std::memcpy(&plaintext[46], snapshot_.config.defaultTick, 4);
  writeUint64BE(snapshot_.config.defaultMintAmount, &plaintext[50]);

  std::array<uint8_t, kBackupSaltLength> salt{};
  std::array<uint8_t, kBackupIvLength> iv{};
  std::array<uint8_t, 32> key{};
  std::array<uint8_t, kBackupTagLength> tag{};
  std::vector<uint8_t> ciphertext;

  fillRandomBytes(salt.data(), salt.size());
  fillRandomBytes(iv.data(), iv.size());

  if (!deriveBackupKey(passphrase, salt.data(), salt.size(), key, error)) {
    return false;
  }

  if (!encryptAesGcm(plaintext, key, iv, ciphertext, tag, error)) {
    return false;
  }

  backup.version = kBackupVersion;
  backup.algorithm = kBackupAlgorithm;
  backup.saltHex = toHex(salt.data(), salt.size());
  backup.ivHex = toHex(iv.data(), iv.size());
  backup.ciphertextHex = toHex(ciphertext);
  backup.tagHex = toHex(tag.data(), tag.size());
  backup.deviceId = toHex(snapshot_.deviceId);
  return true;
}

bool DeviceStateStore::importBackup(const BackupBlob &backup,
                                    const String &passphrase,
                                    bool overwrite,
                                    String &error) {
  if (snapshot_.hasKey && !overwrite) {
    error = "Device already has a key; set overwrite=true to replace it";
    return false;
  }

  if (backup.version != kBackupVersion) {
    error = "Unsupported backup version";
    return false;
  }

  if (backup.algorithm != kBackupAlgorithm) {
    error = "Unsupported backup algorithm";
    return false;
  }

  std::array<uint8_t, kBackupSaltLength> salt{};
  std::array<uint8_t, kBackupIvLength> iv{};
  std::array<uint8_t, kBackupTagLength> tag{};
  std::array<uint8_t, 32> key{};

  if (!hexToBytes(backup.saltHex, salt.data(), salt.size())) {
    error = "Backup saltHex has invalid format";
    return false;
  }

  if (!hexToBytes(backup.ivHex, iv.data(), iv.size())) {
    error = "Backup ivHex has invalid format";
    return false;
  }

  if (!hexToBytes(backup.tagHex, tag.data(), tag.size())) {
    error = "Backup tagHex has invalid format";
    return false;
  }

  const size_t cipherLength = backup.ciphertextHex.length() / 2;
  if (backup.ciphertextHex.length() == 0 || (backup.ciphertextHex.length() % 2) != 0) {
    error = "Backup ciphertextHex has invalid format";
    return false;
  }

  std::vector<uint8_t> ciphertext(cipherLength);
  if (!hexToBytes(backup.ciphertextHex, ciphertext.data(), ciphertext.size())) {
    error = "Backup ciphertextHex has invalid hex data";
    return false;
  }

  if (!deriveBackupKey(passphrase, salt.data(), salt.size(), key, error)) {
    return false;
  }

  std::vector<uint8_t> plaintext;
  if (!decryptAesGcm(ciphertext, key, iv, tag, plaintext, error)) {
    return false;
  }

  if (plaintext.size() != kBackupPlaintextLength) {
    error = "Backup plaintext length is invalid";
    return false;
  }

  if (!(plaintext[0] == 'L' && plaintext[1] == '2' && plaintext[2] == '0' && plaintext[3] == 'B')) {
    error = "Backup magic header is invalid";
    return false;
  }

  if (plaintext[4] != kBackupVersion) {
    error = "Backup version inside blob is invalid";
    return false;
  }

  std::memcpy(snapshot_.seed.data(), &plaintext[5], snapshot_.seed.size());
  snapshot_.nextNonce = readUint32BE(&plaintext[37]);
  snapshot_.config.autoMintEnabled = plaintext[41] == 0x01;
  snapshot_.config.autoMintIntervalSeconds = readUint32BE(&plaintext[42]);
  std::memcpy(snapshot_.config.defaultTick, &plaintext[46], 4);
  snapshot_.config.defaultTick[4] = '\0';
  snapshot_.config.defaultMintAmount = readUint64BE(&plaintext[50]);
  snapshot_.hasKey = true;

  if (!deriveKeyMaterial(error)) {
    return false;
  }

  return persistAll(error);
}

bool DeviceStateStore::updateConfig(const DeviceConfig &config, String &error) {
  snapshot_.config = config;
  return persistConfig(error);
}

bool DeviceStateStore::commitNonce(uint32_t usedNonce, String &error) {
  snapshot_.nextNonce = usedNonce + 1;
  return persistNonce(error);
}

bool DeviceStateStore::applyCommittedConfig(const DeviceConfig &config, uint32_t usedNonce, String &error) {
  snapshot_.config = config;
  snapshot_.nextNonce = usedNonce + 1;
  return persistConfig(error) && persistNonce(error);
}

bool DeviceStateStore::persistSeed(String &error) {
  if (!snapshot_.hasKey) {
    if (!prefs_.remove(kSeedKey)) {
      error = "Failed to remove device seed from NVS";
      return false;
    }
    return true;
  }

  if (prefs_.putBytes(kSeedKey, snapshot_.seed.data(), snapshot_.seed.size()) != snapshot_.seed.size()) {
    error = "Failed to store device seed in NVS";
    return false;
  }

  return true;
}

bool DeviceStateStore::persistConfig(String &error) {
  char amountBuffer[32];
  snprintf(amountBuffer, sizeof(amountBuffer), "%llu", static_cast<unsigned long long>(snapshot_.config.defaultMintAmount));

  if (!prefs_.putBool(kAutoMintEnabledKey, snapshot_.config.autoMintEnabled)) {
    error = "Failed to persist autoMintEnabled";
    return false;
  }

  if (prefs_.putUInt(kAutoMintIntervalKey, snapshot_.config.autoMintIntervalSeconds) !=
      sizeof(uint32_t)) {
    error = "Failed to persist autoMintIntervalSeconds";
    return false;
  }

  if (prefs_.putString(kDefaultTickKey, tickToString(snapshot_.config.defaultTick)) == 0) {
    error = "Failed to persist defaultTick";
    return false;
  }

  if (prefs_.putString(kDefaultAmountKey, String(amountBuffer)) == 0) {
    error = "Failed to persist defaultMintAmount";
    return false;
  }

  return true;
}

bool DeviceStateStore::persistNonce(String &error) {
  if (prefs_.putUInt(kNonceKey, snapshot_.nextNonce) != sizeof(uint32_t)) {
    error = "Failed to persist nextNonce";
    return false;
  }

  return true;
}

bool DeviceStateStore::persistAll(String &error) {
  return persistSeed(error) && persistConfig(error) && persistNonce(error);
}

bool DeviceStateStore::deriveKeyMaterial(String &error) {
  if (!snapshot_.hasKey) {
    error = "No seed is available";
    return false;
  }

  ed25519_create_keypair(snapshot_.publicKey.data(), snapshot_.privateKey.data(), snapshot_.seed.data());

  std::array<uint8_t, 32> digest{};
  if (mbedtls_sha256_ret(snapshot_.publicKey.data(), snapshot_.publicKey.size(), digest.data(), 0) != 0) {
    error = "SHA-256 failed while deriving device_id";
    return false;
  }

  std::memcpy(snapshot_.deviceId.data(), digest.data(), snapshot_.deviceId.size());
  return true;
}

bool normalizeTick(const String &input, char output[5], String &error) {
  if (input.length() != 4) {
    error = "Ticker must be exactly 4 characters";
    return false;
  }

  for (size_t index = 0; index < 4; ++index) {
    const unsigned char raw = static_cast<unsigned char>(input[index]);
    const char upper = static_cast<char>(std::toupper(raw));
    if (!std::isalnum(static_cast<unsigned char>(upper))) {
      error = "Ticker must contain only ASCII letters or digits";
      return false;
    }
    output[index] = upper;
  }

  output[4] = '\0';
  return true;
}

String tickToString(const char tick[5]) {
  return String(tick).substring(0, 4);
}

bool parseUint64(const String &text, uint64_t &value) {
  if (text.isEmpty()) {
    return false;
  }

  char *end = nullptr;
  value = strtoull(text.c_str(), &end, 10);
  return end != nullptr && *end == '\0';
}

bool parseUint32(const String &text, uint32_t &value) {
  uint64_t raw = 0;
  if (!parseUint64(text, raw) || raw > 0xFFFFFFFFULL) {
    return false;
  }

  value = static_cast<uint32_t>(raw);
  return true;
}

String toHex(const uint8_t *data, size_t length) {
  constexpr char hexDigits[] = "0123456789abcdef";
  String output;
  output.reserve(length * 2);

  for (size_t index = 0; index < length; ++index) {
    output += hexDigits[(data[index] >> 4) & 0x0F];
    output += hexDigits[data[index] & 0x0F];
  }

  return output;
}

String toHex(const std::vector<uint8_t> &bytes) {
  return toHex(bytes.data(), bytes.size());
}

bool hexToBytes(const String &input, uint8_t *out, size_t expectedLength) {
  if (input.length() != expectedLength * 2) {
    return false;
  }

  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  for (size_t index = 0; index < expectedLength; ++index) {
    const int high = nibble(input[index * 2]);
    const int low = nibble(input[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[index] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

void fillRandomBytes(uint8_t *target, size_t length) {
  esp_fill_random(target, length);
}

}  // namespace lora20

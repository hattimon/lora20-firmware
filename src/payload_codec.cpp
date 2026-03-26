#include "lora20_device.hpp"

#include <ed25519.h>

#include <cstdint>
#include <cstring>

namespace {

void appendUint32BE(std::vector<uint8_t> &target, uint32_t value) {
  target.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  target.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  target.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  target.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendUint64BE(std::vector<uint8_t> &target, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    target.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
}

bool finalizePreparedPayload(const lora20::DeviceSnapshot &snapshot,
                             lora20::PreparedPayload &prepared,
                             String &error) {
  if (!snapshot.hasKey) {
    error = "Device key is not initialized";
    return false;
  }

  ed25519_sign(prepared.signature.data(),
               prepared.unsignedPayload.data(),
               prepared.unsignedPayload.size(),
               snapshot.publicKey.data(),
               snapshot.privateKey.data());

  prepared.payload = prepared.unsignedPayload;
  prepared.payload.insert(prepared.payload.end(), prepared.signature.begin(), prepared.signature.end());
  return true;
}

}  // namespace

namespace lora20 {

bool buildDeployPayload(const DeviceSnapshot &snapshot,
                        const char tick[5],
                        uint64_t maxSupply,
                        uint64_t limitPerMint,
                        uint32_t nonce,
                        PreparedPayload &prepared,
                        String &error) {
  if (!snapshot.hasKey) {
    error = "Device key is not initialized";
    return false;
  }
  if (maxSupply == 0) {
    error = "maxSupply must be greater than zero";
    return false;
  }
  if (limitPerMint == 0 || limitPerMint > maxSupply) {
    error = "limitPerMint must be greater than zero and not exceed maxSupply";
    return false;
  }

  prepared = PreparedPayload();
  prepared.nonce = nonce;
  prepared.unsignedPayload.reserve(1 + 4 + 8 + 8 + 4);
  prepared.unsignedPayload.push_back(kOpDeploy);
  prepared.unsignedPayload.insert(prepared.unsignedPayload.end(), tick, tick + 4);
  appendUint64BE(prepared.unsignedPayload, maxSupply);
  appendUint64BE(prepared.unsignedPayload, limitPerMint);
  appendUint32BE(prepared.unsignedPayload, nonce);
  return finalizePreparedPayload(snapshot, prepared, error);
}

bool buildMintPayload(const DeviceSnapshot &snapshot,
                      const char tick[5],
                      uint64_t amount,
                      uint32_t nonce,
                      PreparedPayload &prepared,
                      String &error) {
  if (!snapshot.hasKey) {
    error = "Device key is not initialized";
    return false;
  }
  if (amount == 0) {
    error = "amount must be greater than zero";
    return false;
  }

  prepared = PreparedPayload();
  prepared.nonce = nonce;
  prepared.unsignedPayload.reserve(1 + 4 + 8 + 4);
  prepared.unsignedPayload.push_back(kOpMint);
  prepared.unsignedPayload.insert(prepared.unsignedPayload.end(), tick, tick + 4);
  appendUint64BE(prepared.unsignedPayload, amount);
  appendUint32BE(prepared.unsignedPayload, nonce);
  return finalizePreparedPayload(snapshot, prepared, error);
}

bool buildTransferPayload(const DeviceSnapshot &snapshot,
                          const char tick[5],
                          uint64_t amount,
                          const String &recipientDeviceIdHex,
                          uint32_t nonce,
                          PreparedPayload &prepared,
                          String &error) {
  if (!snapshot.hasKey) {
    error = "Device key is not initialized";
    return false;
  }
  if (amount == 0) {
    error = "amount must be greater than zero";
    return false;
  }

  std::array<uint8_t, 8> recipient{};
  if (!hexToBytes(recipientDeviceIdHex, recipient.data(), recipient.size())) {
    error = "toDeviceId must be exactly 16 hex characters";
    return false;
  }

  prepared = PreparedPayload();
  prepared.nonce = nonce;
  prepared.unsignedPayload.reserve(1 + 4 + 8 + 4 + 8);
  prepared.unsignedPayload.push_back(kOpTransfer);
  prepared.unsignedPayload.insert(prepared.unsignedPayload.end(), tick, tick + 4);
  appendUint64BE(prepared.unsignedPayload, amount);
  appendUint32BE(prepared.unsignedPayload, nonce);
  prepared.unsignedPayload.insert(prepared.unsignedPayload.end(), recipient.begin(), recipient.end());
  return finalizePreparedPayload(snapshot, prepared, error);
}

bool buildConfigPayload(const DeviceSnapshot &snapshot,
                        const DeviceConfig &config,
                        uint32_t nonce,
                        PreparedPayload &prepared,
                        String &error) {
  if (!snapshot.hasKey) {
    error = "Device key is not initialized";
    return false;
  }

  if (config.autoMintEnabled && config.autoMintIntervalSeconds == 0) {
    error = "autoMintIntervalSeconds must be greater than zero when auto-mint is enabled";
    return false;
  }

  const uint8_t flags = config.autoMintEnabled ? 0x01 : 0x00;

  prepared = PreparedPayload();
  prepared.nonce = nonce;
  prepared.unsignedPayload.reserve(1 + 1 + 4 + 4);
  prepared.unsignedPayload.push_back(kOpConfig);
  prepared.unsignedPayload.push_back(flags);
  appendUint32BE(prepared.unsignedPayload, config.autoMintIntervalSeconds);
  appendUint32BE(prepared.unsignedPayload, nonce);
  return finalizePreparedPayload(snapshot, prepared, error);
}

}  // namespace lora20

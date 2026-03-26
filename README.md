# lora20 firmware bootstrap

Bootstrap firmware project for `Heltec WiFi LoRa 32 V4` focused on:

- USB Serial setup API for a remote dashboard
- device key generation and persistent storage in `NVS`
- `device_id` derivation from `SHA-256(public_key)`
- encrypted local backup export/import
- signed binary payload packing for `DEPLOY`, `MINT`, `TRANSFER`, and `CONFIG`

This first firmware phase is intentionally **USB-first**:

- it is suitable for the first flash on `Heltec V4`
- it already supports setup, config, key material, and signed payload generation
- it does **not** yet push uplinks over LoRaWAN radio

## Why this shape

- `Heltec V4` is based on `ESP32-S3`, so the first stable bootstrap can use a generic `ESP32-S3` PlatformIO target for USB flashing.
- This keeps the first flash focused on the hard parts that matter immediately: setup, keys, nonce handling, and payload correctness.
- LoRaWAN radio integration should be added only after the USB/API path is stable.

## Current commands

The firmware exposes newline-delimited JSON over `USB Serial`.

- `ping`
- `get_info`
- `get_config`
- `set_config`
- `generate_key`
- `get_public_key`
- `export_backup`
- `import_backup`
- `prepare_deploy`
- `prepare_mint`
- `prepare_transfer`
- `prepare_config`

Each `prepare_*` command returns:

- `unsignedPayloadHex`
- `signatureHex`
- `payloadHex`
- `payloadSize`
- `nonce`
- `committed`

## Quick start in VS Code

1. Install the VS Code extension `PlatformIO IDE`.
2. Open this folder in VS Code.
3. Wait until PlatformIO downloads the toolchain and libraries.
4. Connect the `Heltec V4` by USB-C.
5. In PlatformIO:
   - select environment `heltec_v4_bootstrap`
   - run `Build`
   - run `Upload`
   - open `Monitor`

The local shell in this workspace does **not** currently have `PlatformIO CLI`, so this project is prepared for you but was not built locally here.

## First serial test

Open the serial monitor at `115200` and send:

```json
{"id":"1","command":"ping"}
```

Then:

```json
{"id":"2","command":"generate_key","params":{"force":true}}
```

Then:

```json
{"id":"3","command":"get_info"}
```

## File map

- `platformio.ini` - PlatformIO project definition
- `src/main.cpp` - boot and main loop
- `src/device_state.cpp` - NVS-backed key/config state
- `src/payload_codec.cpp` - binary protocol packing and signing
- `src/serial_rpc.cpp` - JSON-over-serial command server
- `docs/serial-api.md` - wire format and command examples
- `docs/install-platformio-vscode.md` - how to verify/install PlatformIO in VS Code
- `docs/flashing-windows.md` - first flash workflow

## Important security notes

- The device uses a dedicated `Ed25519` keypair, separate from any Solana wallet key.
- `export_backup` exports an encrypted blob, not plaintext private material.
- Private key material is derived from a random 32-byte seed and stored in `NVS`.
- `prepare_*` defaults to preview mode unless `commit=true` is set.

## Next phase after first flash

1. confirm stable USB Serial setup on your Heltec V4
2. add hardware button confirmation for sensitive actions
3. add actual LoRaWAN uplink send
4. connect dashboard `Web Serial` to this API

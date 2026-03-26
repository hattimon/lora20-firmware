# Serial API

Transport:

- USB CDC serial
- `115200` baud
- newline-delimited UTF-8 JSON

Request envelope:

```json
{
  "id": "req-1",
  "command": "get_info",
  "params": {}
}
```

Success response:

```json
{
  "id": "req-1",
  "ok": true,
  "result": {}
}
```

Error response:

```json
{
  "id": "req-1",
  "ok": false,
  "error": {
    "code": "invalid_tick",
    "message": "Ticker must be exactly 4 uppercase ASCII alphanumeric characters"
  }
}
```

## Commands

### `ping`

Returns firmware identity and liveness.

### `get_info`

Returns:

- firmware version
- chip model
- free heap
- `hasKey`
- `deviceId`
- `publicKeyHex`
- `nextNonce`
- config values

### `generate_key`

Params:

```json
{
  "force": false
}
```

If `force=true`, replaces the existing device key and resets `nextNonce` to `1`.

### `get_public_key`

Returns `publicKeyHex` and `deviceId`.

### `export_backup`

Params:

```json
{
  "passphrase": "strong local passphrase"
}
```

Returns an encrypted backup object:

```json
{
  "version": 1,
  "algorithm": "AES-256-GCM+PBKDF2-SHA256",
  "saltHex": "...",
  "ivHex": "...",
  "ciphertextHex": "...",
  "tagHex": "...",
  "deviceId": "..."
}
```

### `import_backup`

Params:

```json
{
  "passphrase": "strong local passphrase",
  "overwrite": true,
  "backup": {
    "version": 1,
    "algorithm": "AES-256-GCM+PBKDF2-SHA256",
    "saltHex": "...",
    "ivHex": "...",
    "ciphertextHex": "...",
    "tagHex": "...",
    "deviceId": "..."
  }
}
```

### `get_config`

Returns local device defaults:

- `autoMintEnabled`
- `autoMintIntervalSeconds`
- `defaultTick`
- `defaultMintAmount`

### `set_config`

Updates local defaults only. This is not a LoRa protocol transaction.

### `prepare_deploy`

Params:

```json
{
  "tick": "LORA",
  "maxSupply": "1000000",
  "limitPerMint": "100",
  "commit": false
}
```

### `prepare_mint`

Params:

```json
{
  "tick": "LORA",
  "amount": "25",
  "commit": false
}
```

### `prepare_transfer`

Params:

```json
{
  "tick": "LORA",
  "amount": "5",
  "toDeviceId": "0011223344556677",
  "commit": false
}
```

### `prepare_config`

Params:

```json
{
  "autoMintEnabled": true,
  "autoMintIntervalSeconds": 1800,
  "commit": false
}
```

If `commit=true`, the firmware:

- persists the new config
- advances nonce
- returns the signed payload


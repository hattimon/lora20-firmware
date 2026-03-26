# First flash from Windows

## Recommended path

Use `VS Code + PlatformIO IDE` on the Windows host. Do not route the USB board through the Debian VM unless you have to.

## Steps

1. Install the `PlatformIO IDE` extension in VS Code.
2. Open the folder `C:\Users\Kosmo\Documents\Playground\lora20-firmware`.
3. Connect the Heltec V4 over `USB-C`.
4. In PlatformIO:
   - select environment `heltec_v4_bootstrap`
   - run `Build`
   - run `Upload`
   - run `Monitor`

## Expected first run

The board should print a JSON boot event and then wait for JSON commands over serial.

Start with:

```json
{"id":"1","command":"ping"}
```

Then:

```json
{"id":"2","command":"get_info"}
```

Then generate the first device key:

```json
{"id":"3","command":"generate_key","params":{"force":true}}
```

## Why generic ESP32-S3 first

For the bootstrap phase the firmware uses a generic `ESP32-S3` PlatformIO target, because:

- it is sufficient for USB setup and signing work
- it avoids blocking on Heltec-specific radio integration
- it gives you a stable first flash path

The next firmware phase should add Heltec-specific radio support once the setup API is stable.


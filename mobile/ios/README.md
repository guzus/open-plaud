# Open Plaud iOS

SwiftPM package for the Open Plaud iOS companion work.

Current targets:

- `OpenPlaudMobileTranscription`: on-device transcription utilities.
- `OpenPlaudCompanion`: CoreBluetooth device client, BLE command contract, and
  status models.
- `OpenPlaud`: basic SwiftUI companion app named **Open Plaud**.

The app is a control/setup companion with a first BLE transfer slice: it can
configure Wi-Fi/server settings, start Wi-Fi sync, select a transfer preference,
and read raw or IMA ADPCM clip chunks from the firmware GATT service.

## Build and verify

```bash
cd mobile/ios
swift run open-plaud-mobile-verify
swift run open-plaud-companion-verify
swift build --product OpenPlaud
```

## Transcription contract

`OnDeviceSpeechTranscriber` accepts a local audio file URL and returns:

- `text`: best formatted transcript
- `languageIdentifier`: recognizer locale
- `engine`: `apple_speech_on_device`
- `isOnDevice`: always `true`
- `segments[]`: timestamped text segments from `SFTranscription`

Runtime transcription still needs the normal Apple Speech user authorization and
an iOS device/simulator locale that supports on-device recognition.

## BLE companion contract

The provisional GATT profile uses one primary service:

- Service: `6F504C41-5544-0001-9000-000000000001`
- Command write characteristic: `6F504C41-5544-0002-9000-000000000001`
- Status notify/read characteristic: `6F504C41-5544-0003-9000-000000000001`
- Clip manifest characteristic: `6F504C41-5544-0004-9000-000000000001`
- Clip chunk characteristic: `6F504C41-5544-0005-9000-000000000001`

Commands are UTF-8 JSON. Supported app-side commands:

- `request_status`
- `start_wifi_sync`
- `stop_sync`
- `configure_wifi`
- `configure_server`
- `set_transfer_preference`

Transfer preferences expose `wifi` or `bluetooth` routes and `mono_wav`,
`ima_adpcm`, or `opus` compression choices. Firmware currently supports mono WAV
and IMA ADPCM; `opus` is reserved for a future codec.

Do not hardcode hotspot credentials or auth tokens in source. Enter them in the
app at runtime and send them to the device over the BLE command characteristic.

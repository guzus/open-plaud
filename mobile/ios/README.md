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
xcodegen generate
xcodebuild -project OpenPlaud.xcodeproj -scheme OpenPlaudApp -destination 'generic/platform=iOS Simulator' build
xcodebuild -project OpenPlaud.xcodeproj -scheme OpenPlaudApp -destination 'generic/platform=iOS' -configuration Release -archivePath build/OpenPlaudApp.xcarchive archive
xcodebuild -exportArchive -archivePath build/OpenPlaudApp.xcarchive -exportPath build/OpenPlaudApp-Export -exportOptionsPlist ExportOptions.plist -allowProvisioningUpdates
```

The SwiftPM `OpenPlaud` product is useful for fast compile checks, but it
archives as a command-line binary. Use the generated Xcode project when making
an `.xcarchive` for TestFlight.

TestFlight upload requires an authenticated Apple Developer account in Xcode or
an App Store Connect API key passed to `xcodebuild` with
`-authenticationKeyPath`, `-authenticationKeyID`, and
`-authenticationKeyIssuerID`.

## Xcode Cloud

Xcode Cloud builds from the Git checkout it receives, so keep
`OpenPlaud.xcodeproj` and its shared `OpenPlaudApp` scheme committed. The
`project.yml` file remains the reproducible source for regenerating the project
locally with XcodeGen, but Xcode Cloud should build the checked-in project
directly.

To create the first workflow:

1. Push this repository to GitHub with `mobile/ios/OpenPlaud.xcodeproj`
   committed.
2. Open `mobile/ios/OpenPlaud.xcodeproj` in Xcode.
3. Sign in to Xcode with the Apple Developer team `GGYW4A4D65`.
4. Select `OpenPlaudApp` as the active scheme.
5. Use `Product > Xcode Cloud > Create Workflow`.
6. Choose the repository `guzus/open-plaud`, the project
   `mobile/ios/OpenPlaud.xcodeproj`, and the scheme `OpenPlaudApp`.
7. Add an archive action for `Release` and enable TestFlight distribution after
   the first cloud archive succeeds.

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
`ima_adpcm`, or `opus` compression choices. Firmware Wi-Fi sync prefers Ogg Opus
when enabled and falls back to IMA ADPCM; Bluetooth chunk transfer currently uses
raw WAV or IMA ADPCM.

Do not hardcode hotspot credentials or auth tokens in source. Enter them in the
app at runtime and send them to the device over the BLE command characteristic.

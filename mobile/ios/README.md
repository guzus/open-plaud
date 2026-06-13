# Open Plaud mobile transcription

SwiftPM package for on-device iOS transcription. The first mobile backend uses
Apple Speech with `requiresOnDeviceRecognition = true`; it does not bundle a
Whisper Core ML or TFLite model yet.

## Build and verify

```bash
cd mobile/ios
swift run open-plaud-mobile-verify
```

## Contract

`OnDeviceSpeechTranscriber` accepts a local audio file URL and returns:

- `text`: best formatted transcript
- `languageIdentifier`: recognizer locale
- `engine`: `apple_speech_on_device`
- `isOnDevice`: always `true`
- `segments[]`: timestamped text segments from `SFTranscription`

Runtime transcription still needs the normal Apple Speech user authorization and
an iOS device/simulator locale that supports on-device recognition.

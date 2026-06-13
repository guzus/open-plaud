import Foundation
import Speech

#if canImport(OpenPlaudMobileTranscription)
import OpenPlaudMobileTranscription
#endif

@main
enum VerifyMobileTranscription {
    static func main() throws {
        let tempURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("wav")
        FileManager.default.createFile(atPath: tempURL.path, contents: Data(), attributes: nil)
        defer { try? FileManager.default.removeItem(at: tempURL) }

        let transcriber = OnDeviceSpeechTranscriber(localeIdentifier: "en_US")
        let request = try transcriber.makeRecognitionRequest(for: tempURL)
        precondition(request.requiresOnDeviceRecognition)
        precondition(!request.shouldReportPartialResults)

        let transcript = MobileTranscript(
            text: "hello world",
            languageIdentifier: "en_US",
            engine: OnDeviceSpeechTranscriber.engineName,
            isOnDevice: true,
            segments: [
                MobileTranscriptSegment(startSeconds: 0, endSeconds: 0.4, text: "hello"),
                MobileTranscriptSegment(startSeconds: 0.4, endSeconds: 1.0, text: "world"),
            ]
        )
        let data = try JSONEncoder().encode(transcript)
        let decoded = try JSONDecoder().decode(MobileTranscript.self, from: data)
        precondition(decoded == transcript)
        precondition(decoded.engine == "apple_speech_on_device")
        precondition(decoded.isOnDevice)

        do {
            _ = try transcriber.makeRecognitionRequest(for: tempURL.deletingLastPathComponent()
                .appendingPathComponent(UUID().uuidString)
                .appendingPathExtension("wav"))
            fatalError("Expected missing audio file error")
        } catch let error as MobileTranscriptionError {
            guard case .audioFileMissing = error else {
                throw error
            }
        }

        print("open-plaud mobile transcription verifier passed")
    }
}

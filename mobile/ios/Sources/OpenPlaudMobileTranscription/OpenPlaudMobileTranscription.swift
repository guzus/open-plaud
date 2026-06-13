import Foundation
import Speech

public struct MobileTranscriptSegment: Codable, Equatable, Sendable {
    public let startSeconds: TimeInterval
    public let endSeconds: TimeInterval
    public let text: String

    public init(startSeconds: TimeInterval, endSeconds: TimeInterval, text: String) {
        self.startSeconds = startSeconds
        self.endSeconds = endSeconds
        self.text = text
    }
}

public struct MobileTranscript: Codable, Equatable, Sendable {
    public let text: String
    public let languageIdentifier: String
    public let engine: String
    public let isOnDevice: Bool
    public let segments: [MobileTranscriptSegment]

    public init(
        text: String,
        languageIdentifier: String,
        engine: String,
        isOnDevice: Bool,
        segments: [MobileTranscriptSegment]
    ) {
        self.text = text
        self.languageIdentifier = languageIdentifier
        self.engine = engine
        self.isOnDevice = isOnDevice
        self.segments = segments
    }
}

public enum MobileTranscriptionError: Error, Equatable, LocalizedError, Sendable {
    case audioFileMissing(String)
    case authorizationDenied(String)
    case unsupportedLocale(String)
    case recognizerUnavailable(String)
    case recognitionFailed(String)
    case noTranscription

    public var errorDescription: String? {
        switch self {
        case .audioFileMissing(let path):
            return "Audio file does not exist: \(path)"
        case .authorizationDenied(let status):
            return "Speech recognition authorization denied: \(status)"
        case .unsupportedLocale(let locale):
            return "Speech recognition does not support locale: \(locale)"
        case .recognizerUnavailable(let locale):
            return "Speech recognizer is unavailable for locale: \(locale)"
        case .recognitionFailed(let message):
            return "Speech recognition failed: \(message)"
        case .noTranscription:
            return "Speech recognition returned no transcript"
        }
    }
}

public final class OnDeviceSpeechTranscriber {
    public static let engineName = "apple_speech_on_device"

    private let locale: Locale

    public init(localeIdentifier: String = Locale.current.identifier) {
        self.locale = Locale(identifier: localeIdentifier)
    }

    public static func requestAuthorization() async -> SFSpeechRecognizerAuthorizationStatus {
        await withCheckedContinuation { continuation in
            SFSpeechRecognizer.requestAuthorization { status in
                continuation.resume(returning: status)
            }
        }
    }

    public func makeRecognitionRequest(for audioURL: URL) throws -> SFSpeechURLRecognitionRequest {
        guard FileManager.default.fileExists(atPath: audioURL.path) else {
            throw MobileTranscriptionError.audioFileMissing(audioURL.path)
        }

        let request = SFSpeechURLRecognitionRequest(url: audioURL)
        request.requiresOnDeviceRecognition = true
        request.shouldReportPartialResults = false
        return request
    }

    public func transcribeFile(at audioURL: URL, requestAuthorization: Bool = true) async throws -> MobileTranscript {
        let request = try makeRecognitionRequest(for: audioURL)

        if requestAuthorization {
            let status = await Self.requestAuthorization()
            guard status == .authorized else {
                throw MobileTranscriptionError.authorizationDenied(Self.authorizationStatusName(status))
            }
        }

        let languageIdentifier = locale.identifier
        guard let recognizer = SFSpeechRecognizer(locale: locale) else {
            throw MobileTranscriptionError.unsupportedLocale(languageIdentifier)
        }
        guard recognizer.isAvailable else {
            throw MobileTranscriptionError.recognizerUnavailable(languageIdentifier)
        }

        return try await runRecognition(
            request: request,
            recognizer: recognizer,
            languageIdentifier: languageIdentifier
        )
    }

    private func runRecognition(
        request: SFSpeechURLRecognitionRequest,
        recognizer: SFSpeechRecognizer,
        languageIdentifier: String
    ) async throws -> MobileTranscript {
        try await withCheckedThrowingContinuation { continuation in
            let resumeBox = RecognitionResumeBox()
            let task = recognizer.recognitionTask(with: request) { result, error in
                if let result, result.isFinal {
                    let transcript = Self.transcript(
                        from: result,
                        languageIdentifier: languageIdentifier
                    )
                    resumeBox.resumeSuccess(continuation, transcript)
                    return
                }

                if let error {
                    resumeBox.resumeFailure(
                        continuation,
                        MobileTranscriptionError.recognitionFailed(error.localizedDescription)
                    )
                }
            }
            resumeBox.retain(task)
        }
    }

    private static func transcript(
        from result: SFSpeechRecognitionResult,
        languageIdentifier: String
    ) -> MobileTranscript {
        let transcription = result.bestTranscription
        let text = transcription.formattedString.trimmingCharacters(in: .whitespacesAndNewlines)
        let segments = transcription.segments.map { segment in
            MobileTranscriptSegment(
                startSeconds: segment.timestamp,
                endSeconds: segment.timestamp + segment.duration,
                text: segment.substring
            )
        }

        return MobileTranscript(
            text: text,
            languageIdentifier: languageIdentifier,
            engine: engineName,
            isOnDevice: true,
            segments: segments
        )
    }

    private static func authorizationStatusName(_ status: SFSpeechRecognizerAuthorizationStatus) -> String {
        switch status {
        case .authorized:
            return "authorized"
        case .denied:
            return "denied"
        case .notDetermined:
            return "notDetermined"
        case .restricted:
            return "restricted"
        @unknown default:
            return "unknown"
        }
    }
}

private final class RecognitionResumeBox: @unchecked Sendable {
    private let lock = NSLock()
    private var didResume = false
    private var retainedTask: SFSpeechRecognitionTask?

    func retain(_ task: SFSpeechRecognitionTask) {
        lock.lock()
        retainedTask = task
        lock.unlock()
    }

    func resumeSuccess(
        _ continuation: CheckedContinuation<MobileTranscript, Error>,
        _ transcript: MobileTranscript
    ) {
        guard claimResume() else {
            return
        }

        continuation.resume(returning: transcript)
    }

    func resumeFailure(
        _ continuation: CheckedContinuation<MobileTranscript, Error>,
        _ error: MobileTranscriptionError
    ) {
        guard claimResume() else {
            return
        }

        continuation.resume(throwing: error)
    }

    private func claimResume() -> Bool {
        lock.lock()
        guard !didResume else {
            lock.unlock()
            return false
        }
        didResume = true
        retainedTask = nil
        lock.unlock()
        return true
    }
}

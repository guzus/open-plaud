import Foundation

/// The server's `Recording` response, decoded tolerantly. Mirrors the live Go
/// server's struct (server-go/main.go): `id, status, filename, size_bytes,
/// created_at, content_type, stored_name, transcript, summary, error`.
///
/// Everything except `id`/`status` is optional and decoded best-effort — a
/// successful (2xx) upload must NEVER be reported as a failure just because the
/// body shape surprised us. `error` is `any` server-side (null | string | object),
/// so we capture it as a free-form string for display only.
public struct RecordingResponse: Sendable, Equatable {
    public let id: String
    public let status: String
    public let filename: String?
    public let sizeBytes: Int?
    public let createdAt: String?
    public let transcriptText: String?
    public let summaryText: String?
    public let error: String?

    public init(
        id: String,
        status: String,
        filename: String? = nil,
        sizeBytes: Int? = nil,
        createdAt: String? = nil,
        transcriptText: String? = nil,
        summaryText: String? = nil,
        error: String? = nil
    ) {
        self.id = id
        self.status = status
        self.filename = filename
        self.sizeBytes = sizeBytes
        self.createdAt = createdAt
        self.transcriptText = transcriptText
        self.summaryText = summaryText
        self.error = error
    }

    /// Best-effort decode from raw response bytes. Returns nil only if the body
    /// isn't a JSON object at all; callers treat a 2xx with nil here as success
    /// with an unknown id.
    public static func decode(from data: Data) -> RecordingResponse? {
        guard let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            return nil
        }
        let id = (obj["id"] as? String) ?? ""
        let status = (obj["status"] as? String) ?? ""
        let filename = obj["filename"] as? String
        let size = (obj["size_bytes"] as? NSNumber)?.intValue
        let created = obj["created_at"] as? String

        var transcript: String?
        if let t = obj["transcript"] as? [String: Any] { transcript = t["text"] as? String }

        var summary: String?
        if let s = obj["summary"] as? [String: Any] { summary = s["summary"] as? String }

        // `error` may be null, a string, or an object — stringify defensively.
        var errStr: String?
        switch obj["error"] {
        case let s as String where !s.isEmpty: errStr = s
        case let dict as [String: Any]: errStr = (dict["message"] as? String) ?? "\(dict)"
        default: errStr = nil
        }

        return RecordingResponse(
            id: id, status: status, filename: filename, sizeBytes: size,
            createdAt: created, transcriptText: transcript, summaryText: summary, error: errStr
        )
    }
}

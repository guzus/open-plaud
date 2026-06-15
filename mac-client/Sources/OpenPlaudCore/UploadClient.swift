import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// Why an upload failed, split by whether retrying could plausibly help.
public enum UploadError: Error, Equatable, Sendable {
    /// Network-level problem (timeout, connection lost) — safe to retry.
    case transient(String)
    /// Server said 5xx or 429 — safe to retry (with backoff).
    case serverTemporary(status: Int)
    /// Auth missing/invalid (401/403) — do NOT retry; user must set a token.
    case unauthorized(status: Int)
    /// Client error (400/413/…) — do NOT retry; the request itself is wrong.
    case permanent(status: Int, body: String)
    /// Couldn't read the source file.
    case fileUnreadable(String)

    public var isRetriable: Bool {
        switch self {
        case .transient, .serverTemporary: return true
        case .unauthorized, .permanent, .fileUnreadable: return false
        }
    }

    /// A short, user-safe message. NEVER contains the auth token.
    public var userMessage: String {
        switch self {
        case .transient(let m): return "Network error: \(m)"
        case .serverTemporary(let s): return "Server busy (HTTP \(s))"
        case .unauthorized(let s): return "Unauthorized (HTTP \(s)) — set your token in Settings"
        case .permanent(let s, _): return "Upload rejected (HTTP \(s))"
        case .fileUnreadable(let m): return "Can't read file: \(m)"
        }
    }
}

/// Connection settings for the upload path. The token is held only in memory for
/// the duration of a sync; it is never logged or persisted by this type.
public struct ServerConfig: Sendable {
    public var baseURL: URL
    public var token: String?

    public init(baseURL: URL, token: String? = nil) {
        self.baseURL = baseURL
        self.token = token
    }

    public var recordingsURL: URL { baseURL.appendingPathComponent("v1/recordings") }
    public var healthURL: URL { baseURL.appendingPathComponent("healthz") }
}

/// Uploads single WAV clips to `POST {base}/v1/recordings`.
///
/// Token handling: the `Authorization: Bearer …` header is set on the request
/// but is *never* logged. We never print request headers.
public final class UploadClient: @unchecked Sendable {
    private let session: URLSession
    private let config: ServerConfig

    public init(config: ServerConfig, session: URLSession? = nil) {
        self.config = config
        if let session {
            self.session = session
        } else {
            let cfg = URLSessionConfiguration.ephemeral
            cfg.timeoutIntervalForRequest = 60
            cfg.timeoutIntervalForResource = 300
            cfg.waitsForConnectivity = false
            self.session = URLSession(configuration: cfg)
        }
    }

    /// Liveness probe: GET /healthz, expects `{"status":"ok"}`. Open even with auth on.
    @discardableResult
    public func health() async throws -> Bool {
        var req = URLRequest(url: config.healthURL)
        req.httpMethod = "GET"
        let (data, response) = try await session.data(for: req)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            return false
        }
        if let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            return (obj["status"] as? String) == "ok"
        }
        return true
    }

    /// Upload one WAV file. Returns the decoded Recording on any 2xx.
    public func upload(fileURL: URL, filename: String? = nil) async throws -> RecordingResponse {
        let name = filename ?? fileURL.lastPathComponent
        let fileData: Data
        do {
            fileData = try Data(contentsOf: fileURL, options: .mappedIfSafe)
        } catch {
            throw UploadError.fileUnreadable(error.localizedDescription)
        }
        return try await upload(fileData: fileData, filename: name)
    }

    /// Upload from an in-memory buffer (used by the round-trip test).
    public func upload(fileData: Data, filename: String) async throws -> RecordingResponse {
        let body = Multipart.wavBody(fileData: fileData, filename: filename)

        var req = URLRequest(url: config.recordingsURL)
        req.httpMethod = "POST"
        req.setValue(body.contentType, forHTTPHeaderField: "Content-Type")
        if let token = config.token, !token.isEmpty {
            // SECURITY: this header value is a secret. Never log `req.allHTTPHeaderFields`.
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        req.httpBody = body.data

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: req)
        } catch let urlErr as URLError {
            throw UploadError.transient(urlErr.localizedDescription)
        } catch {
            throw UploadError.transient(error.localizedDescription)
        }

        guard let http = response as? HTTPURLResponse else {
            throw UploadError.transient("No HTTP response")
        }

        switch http.statusCode {
        case 200..<300:
            // Any 2xx is success. Decode best-effort; never fail on shape.
            if let rec = RecordingResponse.decode(from: data) {
                return rec
            }
            return RecordingResponse(id: "", status: "received")
        case 401, 403:
            throw UploadError.unauthorized(status: http.statusCode)
        case 429:
            throw UploadError.serverTemporary(status: http.statusCode)
        case 500..<600:
            throw UploadError.serverTemporary(status: http.statusCode)
        default:
            // 400/413/… — permanent. Capture a bounded, secret-free body snippet.
            let snippet = String(data: data.prefix(256), encoding: .utf8) ?? ""
            throw UploadError.permanent(status: http.statusCode, body: snippet)
        }
    }
}

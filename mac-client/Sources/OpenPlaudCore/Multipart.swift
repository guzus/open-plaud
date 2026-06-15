import Foundation

/// Builds a `multipart/form-data` body with a single file part, matching the
/// device's upload exactly: field name **`file`**, part `Content-Type: audio/wav`.
/// (See firmware/main/net_upload.c — same field name and part content type.)
public enum Multipart {
    /// A random boundary token. The device uses a fixed boundary; a client should
    /// use a random one to avoid any chance of it appearing in the payload.
    public static func makeBoundary() -> String {
        "open-plaud-mac-\(UUID().uuidString)"
    }

    public struct Body {
        public let data: Data
        public let contentType: String
    }

    /// Assemble the body for one WAV file already loaded into memory.
    public static func wavBody(
        fileData: Data,
        filename: String,
        boundary: String = makeBoundary(),
        partContentType: String = "audio/wav"
    ) -> Body {
        var d = Data()
        func add(_ s: String) { d.append(contentsOf: Array(s.utf8)) }

        add("--\(boundary)\r\n")
        // Field name MUST be "file" to match the server's multipart parser.
        add("Content-Disposition: form-data; name=\"file\"; filename=\"\(sanitize(filename))\"\r\n")
        add("Content-Type: \(partContentType)\r\n\r\n")
        d.append(fileData)
        add("\r\n--\(boundary)--\r\n")

        return Body(data: d, contentType: "multipart/form-data; boundary=\(boundary)")
    }

    /// Strip CR/LF/quotes from a filename so it can't break the header line.
    private static func sanitize(_ name: String) -> String {
        name.replacingOccurrences(of: "\"", with: "")
            .replacingOccurrences(of: "\r", with: "")
            .replacingOccurrences(of: "\n", with: "")
    }
}

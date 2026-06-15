import Foundation

/// Format metadata parsed from a WAV (RIFF) file header.
///
/// We parse the chunk structure rather than assuming the canonical 44-byte
/// header, because real encoders interleave extra chunks (`LIST`, `fact`,
/// `JUNK`, …) and the `fmt ` chunk can be larger than 16 bytes (WAVE_FORMAT_EXTENSIBLE).
public struct WAVInfo: Equatable, Sendable {
    public let sampleRate: Int
    public let channels: Int
    public let bitsPerSample: Int
    /// Number of bytes of PCM sample data (the `data` chunk size).
    public let dataByteCount: Int

    public init(sampleRate: Int, channels: Int, bitsPerSample: Int, dataByteCount: Int) {
        self.sampleRate = sampleRate
        self.channels = channels
        self.bitsPerSample = bitsPerSample
        self.dataByteCount = dataByteCount
    }

    /// Duration in seconds, derived from the data-chunk size and the byte rate.
    /// Returns 0 when the format is degenerate (avoids a divide-by-zero).
    public var duration: TimeInterval {
        let bytesPerSecond = sampleRate * channels * (bitsPerSample / 8)
        guard bytesPerSecond > 0 else { return 0 }
        return TimeInterval(dataByteCount) / TimeInterval(bytesPerSecond)
    }
}

public enum WAVParseError: Error, Equatable {
    case tooShort
    case notRIFF
    case notWAVE
    case missingFmtChunk
    case missingDataChunk
}

public enum WAVParser {
    /// Parse just enough of a WAV header to get format + data size, reading only
    /// the file head (not the whole multi-MB body).
    ///
    /// We read a bounded prefix and walk the RIFF chunk list. The `data` chunk's
    /// declared size is authoritative for duration; if the file was truncated we
    /// clamp to what's actually on disk so the duration never over-reports.
    public static func parse(url: URL) throws -> WAVInfo {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }

        // 64 KiB is far more than enough to walk past any leading metadata chunks
        // to reach `fmt ` and the `data` chunk header.
        let prefix = try readPrefix(handle: handle, maxBytes: 64 * 1024)
        let attrSize = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int
        return try parse(bytes: prefix, totalFileSize: attrSize ?? prefix.count)
    }

    /// Parse from an in-memory buffer. `totalFileSize` is used to clamp a
    /// `data` chunk size that exceeds the bytes actually present (truncated clip).
    public static func parse(bytes: [UInt8], totalFileSize: Int? = nil) throws -> WAVInfo {
        guard bytes.count >= 12 else { throw WAVParseError.tooShort }
        guard fourCC(bytes, 0) == "RIFF" else { throw WAVParseError.notRIFF }
        guard fourCC(bytes, 8) == "WAVE" else { throw WAVParseError.notWAVE }

        var offset = 12
        var sampleRate: Int?
        var channels: Int?
        var bits: Int?
        var dataSize: Int?
        var dataOffset: Int?

        // Walk chunks: [4-byte id][4-byte little-endian size][payload, padded to even].
        while offset + 8 <= bytes.count {
            let id = fourCC(bytes, offset)
            let size = Int(le32(bytes, offset + 4))
            let payload = offset + 8

            if id == "fmt " {
                // PCM fmt is >= 16 bytes: audioFormat(2) channels(2) sampleRate(4)
                // byteRate(4) blockAlign(2) bitsPerSample(2).
                if payload + 16 <= bytes.count {
                    channels = Int(le16(bytes, payload + 2))
                    sampleRate = Int(le32(bytes, payload + 4))
                    bits = Int(le16(bytes, payload + 14))
                }
            } else if id == "data" {
                dataSize = size
                dataOffset = payload
                // `data` is typically last; we can stop once we also have fmt.
                if sampleRate != nil { break }
            }

            // Advance to the next chunk; chunks are word-aligned (pad odd sizes).
            var advance = size
            if advance % 2 == 1 { advance += 1 }
            // Guard against a corrupt size that would loop or overflow.
            guard advance >= 0 else { break }
            offset = payload + advance
        }

        guard let sr = sampleRate, let ch = channels, let bp = bits else {
            throw WAVParseError.missingFmtChunk
        }
        guard var ds = dataSize, let doff = dataOffset else {
            throw WAVParseError.missingDataChunk
        }

        // Clamp a declared data size that runs past the real end of file
        // (truncated/in-progress recording), so duration is never inflated.
        if let total = totalFileSize {
            let maxData = max(0, total - doff)
            if ds > maxData { ds = maxData }
        }

        return WAVInfo(sampleRate: sr, channels: ch, bitsPerSample: bp, dataByteCount: ds)
    }

    // MARK: - byte helpers

    private static func readPrefix(handle: FileHandle, maxBytes: Int) throws -> [UInt8] {
        let data = try handle.read(upToCount: maxBytes) ?? Data()
        return [UInt8](data)
    }

    private static func fourCC(_ b: [UInt8], _ i: Int) -> String {
        guard i + 4 <= b.count else { return "" }
        return String(bytes: b[i..<i + 4], encoding: .ascii) ?? ""
    }

    private static func le16(_ b: [UInt8], _ i: Int) -> UInt16 {
        guard i + 2 <= b.count else { return 0 }
        return UInt16(b[i]) | (UInt16(b[i + 1]) << 8)
    }

    private static func le32(_ b: [UInt8], _ i: Int) -> UInt32 {
        guard i + 4 <= b.count else { return 0 }
        return UInt32(b[i]) | (UInt32(b[i + 1]) << 8) | (UInt32(b[i + 2]) << 16) | (UInt32(b[i + 3]) << 24)
    }
}

// MARK: - Minimal WAV writer (used by tests + CLI for the round-trip check)

public enum WAVWriter {
    /// Build a minimal, valid canonical 44-byte-header PCM WAV in memory.
    /// Matches the device format by default: 16 kHz / 16-bit / stereo.
    public static func makePCM(
        sampleRate: Int = 16_000,
        channels: Int = 2,
        bitsPerSample: Int = 16,
        sampleFrames: Int = 1_600 // 0.1s of silence at 16 kHz
    ) -> Data {
        let bytesPerSample = bitsPerSample / 8
        let dataSize = sampleFrames * channels * bytesPerSample
        let byteRate = sampleRate * channels * bytesPerSample
        let blockAlign = channels * bytesPerSample

        var d = Data()
        func ascii(_ s: String) { d.append(contentsOf: Array(s.utf8)) }
        func u32(_ v: UInt32) { var x = v.littleEndian; withUnsafeBytes(of: &x) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { var x = v.littleEndian; withUnsafeBytes(of: &x) { d.append(contentsOf: $0) } }

        ascii("RIFF")
        u32(UInt32(36 + dataSize)) // file size - 8
        ascii("WAVE")
        ascii("fmt ")
        u32(16)                    // PCM fmt chunk size
        u16(1)                     // audioFormat = PCM
        u16(UInt16(channels))
        u32(UInt32(sampleRate))
        u32(UInt32(byteRate))
        u16(UInt16(blockAlign))
        u16(UInt16(bitsPerSample))
        ascii("data")
        u32(UInt32(dataSize))
        d.append(Data(count: dataSize)) // silence
        return d
    }
}

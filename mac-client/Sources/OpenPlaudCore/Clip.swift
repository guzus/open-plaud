import Foundation

/// Sync state of a single recording, as shown by the status pill in the UI.
public enum SyncState: String, Codable, Sendable, Equatable {
    case queued
    case syncing
    case synced
    case failed
}

/// A stable identity for a source WAV that survives re-scans and app restarts
/// without re-reading file contents. This is the idempotency key: the same
/// (filename, size, mtime) is treated as "already handled" so re-runs never
/// re-upload. We deliberately use the *filename* (not the absolute path) so a
/// clip keeps its identity if the same SD card mounts at a different path.
///
/// Tradeoff (documented): two genuinely different clips that share filename +
/// byte size + mtime would collide. For `rec_NNNN.wav` clips this is effectively
/// impossible within one device; the safe upgrade is to add a content hash
/// (see README roadmap). Cheap-and-correct-enough for v1.
public struct ClipIdentity: Codable, Hashable, Sendable {
    public let filename: String
    public let sizeBytes: Int
    /// Whole-second mtime; sub-second precision is dropped so the key is stable
    /// across filesystems that round mtime differently (FAT has 2s resolution).
    public let mtimeEpoch: Int

    public init(filename: String, sizeBytes: Int, mtimeEpoch: Int) {
        self.filename = filename
        self.sizeBytes = sizeBytes
        self.mtimeEpoch = mtimeEpoch
    }

    /// Compact, filesystem-safe string used as the JSON key in the state store.
    public var key: String { "\(filename)|\(sizeBytes)|\(mtimeEpoch)" }
}

/// A discovered recording plus everything the UI needs to render one row.
public struct Clip: Identifiable, Sendable, Equatable {
    public var id: String { identity.key }
    public let url: URL
    public let identity: ClipIdentity
    public let modifiedAt: Date
    /// Parsed WAV duration in seconds; nil if the header couldn't be read.
    public let duration: TimeInterval?
    public var state: SyncState
    /// Last error message (for the `failed` state), never contains secrets.
    public var lastError: String?
    /// Server-assigned recording id once uploaded, when known.
    public var remoteID: String?

    public init(
        url: URL,
        identity: ClipIdentity,
        modifiedAt: Date,
        duration: TimeInterval?,
        state: SyncState = .queued,
        lastError: String? = nil,
        remoteID: String? = nil
    ) {
        self.url = url
        self.identity = identity
        self.modifiedAt = modifiedAt
        self.duration = duration
        self.state = state
        self.lastError = lastError
        self.remoteID = remoteID
    }

    public var filename: String { url.lastPathComponent }
    public var sizeBytes: Int { identity.sizeBytes }
}

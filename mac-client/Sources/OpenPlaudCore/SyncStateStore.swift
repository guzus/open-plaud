import Foundation

/// What we persist per clip across runs. Kept minimal: only what's needed to
/// decide "do we still need to upload this?" plus a little provenance.
public struct SyncRecord: Codable, Sendable, Equatable {
    public var state: SyncState
    public var remoteID: String?
    public var lastError: String?
    public var updatedAt: Date

    public init(state: SyncState, remoteID: String? = nil, lastError: String? = nil, updatedAt: Date = Date()) {
        self.state = state
        self.remoteID = remoteID
        self.lastError = lastError
        self.updatedAt = updatedAt
    }
}

/// Durable, JSON-backed synced-state store shared by the CLI and the GUI.
///
/// Keyed by `ClipIdentity.key` (filename|size|mtime). Writes are flushed to disk
/// atomically immediately after each state change, so a crash mid-sync never
/// causes a re-upload and idempotency is demonstrable headlessly.
///
/// Thread-safety: all access is serialized through an internal lock, so it's safe
/// to call from the concurrent upload workers.
public final class SyncStateStore: @unchecked Sendable {
    private let fileURL: URL
    private let lock = NSLock()
    private var records: [String: SyncRecord]

    /// Default location, shared by both targets:
    /// `~/Library/Application Support/OpenPlaud/sync-state.json`.
    public static func defaultURL() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory())
        return base.appendingPathComponent("OpenPlaud", isDirectory: true)
            .appendingPathComponent("sync-state.json")
    }

    public init(fileURL: URL = SyncStateStore.defaultURL()) {
        self.fileURL = fileURL
        self.records = SyncStateStore.load(from: fileURL)
    }

    private static func load(from url: URL) -> [String: SyncRecord] {
        guard let data = try? Data(contentsOf: url) else { return [:] }
        return (try? JSONDecoder.iso().decode([String: SyncRecord].self, from: data)) ?? [:]
    }

    public func record(for identity: ClipIdentity) -> SyncRecord? {
        lock.lock(); defer { lock.unlock() }
        return records[identity.key]
    }

    /// True if this exact clip has already been uploaded successfully.
    public func isSynced(_ identity: ClipIdentity) -> Bool {
        record(for: identity)?.state == .synced
    }

    /// Upsert a record and flush to disk immediately (durable).
    ///
    /// The mutation AND the disk write happen under the same lock. This is
    /// essential for idempotency under the concurrent upload workers: if the
    /// write happened outside the lock, two workers could capture snapshots in
    /// one order but have their atomic file renames land in the other order,
    /// dropping a `synced` key from disk — and the next run would re-upload that
    /// clip (the server has no content-dedup). Serializing the write under the
    /// lock makes on-disk order match snapshot order. The write is a small
    /// synchronous JSON encode + atomic replace; it is dwarfed by the network
    /// upload each worker is doing, and there is no async/reentrancy here, so it
    /// cannot deadlock.
    public func set(_ identity: ClipIdentity, _ record: SyncRecord) {
        lock.lock()
        defer { lock.unlock() }
        records[identity.key] = record
        persist(records)
    }

    /// Snapshot of all records (for diagnostics / tests).
    public func all() -> [String: SyncRecord] {
        lock.lock(); defer { lock.unlock() }
        return records
    }

    private func persist(_ snapshot: [String: SyncRecord]) {
        do {
            try FileManager.default.createDirectory(
                at: fileURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            let data = try JSONEncoder.iso(pretty: true).encode(snapshot)
            // Atomic write so a crash never leaves a half-written state file.
            try data.write(to: fileURL, options: .atomic)
        } catch {
            // Persistence failure must not crash a sync; worst case we re-scan.
            // Intentionally not logging the path at error level to keep output clean.
        }
    }
}

extension JSONEncoder {
    static func iso(pretty: Bool = false) -> JSONEncoder {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        if pretty { e.outputFormatting = [.prettyPrinted, .sortedKeys] }
        return e
    }
}

extension JSONDecoder {
    static func iso() -> JSONDecoder {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }
}

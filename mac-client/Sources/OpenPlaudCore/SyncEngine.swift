import Foundation

/// Progress callback payload for one clip transition during a sync run.
public struct SyncProgress: Sendable {
    public let identity: ClipIdentity
    public let filename: String
    public let state: SyncState
    public let remoteID: String?
    public let error: String?
}

/// Aggregate result of a sync run.
public struct SyncSummary: Sendable, Equatable {
    public var uploaded: Int = 0
    public var skipped: Int = 0   // already synced (idempotent)
    public var failed: Int = 0
    public init(uploaded: Int = 0, skipped: Int = 0, failed: Int = 0) {
        self.uploaded = uploaded
        self.skipped = skipped
        self.failed = failed
    }
}

/// Drives concurrency-limited, retrying, idempotent uploads of queued clips.
///
/// Idempotency: before uploading, each clip is checked against the persisted
/// `SyncStateStore`; already-synced clips are skipped. After a successful upload
/// the store is updated (and flushed) immediately, so a re-run — even after a
/// crash — never re-uploads. The server itself is not idempotent, so this local
/// store is the only thing preventing duplicate uploads from this client.
public final class SyncEngine: @unchecked Sendable {
    private let client: UploadClient
    private let store: SyncStateStore
    private let maxConcurrent: Int
    private let maxRetries: Int

    public init(
        client: UploadClient,
        store: SyncStateStore,
        maxConcurrent: Int = 3,
        maxRetries: Int = 3
    ) {
        self.client = client
        self.store = store
        self.maxConcurrent = max(1, maxConcurrent)
        self.maxRetries = max(0, maxRetries)
    }

    /// Sync the given clips. Only clips not already `.synced` in the store are
    /// uploaded. Returns an aggregate summary.
    ///
    /// - Parameter onProgress: called (off the main actor) on every state change.
    @discardableResult
    public func sync(
        clips: [Clip],
        onProgress: (@Sendable (SyncProgress) -> Void)? = nil
    ) async -> SyncSummary {
        // Partition: skip already-synced (idempotent fast path).
        var pending: [Clip] = []
        var summary = SyncSummary()
        for clip in clips {
            if store.isSynced(clip.identity) {
                summary.skipped += 1
            } else {
                pending.append(clip)
            }
        }

        guard !pending.isEmpty else { return summary }

        // Concurrency-limited worker pool over a shared index.
        let counters = Counters()
        await withTaskGroup(of: Void.self) { group in
            let lane = AsyncIndex(count: pending.count)
            let workers = min(maxConcurrent, pending.count)
            for _ in 0..<workers {
                group.addTask { [weak self] in
                    guard let self else { return }
                    while let i = await lane.next() {
                        await self.process(clip: pending[i], counters: counters, onProgress: onProgress)
                    }
                }
            }
            await group.waitForAll()
        }

        let final = await counters.snapshot()
        summary.uploaded = final.uploaded
        summary.failed = final.failed
        return summary
    }

    private func process(
        clip: Clip,
        counters: Counters,
        onProgress: (@Sendable (SyncProgress) -> Void)?
    ) async {
        let id = clip.identity

        // Mark syncing for the UI only — we deliberately do NOT persist "syncing"
        // to disk. A crash mid-upload must leave the clip re-queued (so it's
        // retried), not stuck in a "syncing" limbo, and only a terminal state
        // (synced/failed) is worth a durable write. Not writing here also halves
        // the per-clip disk writes, keeping the durable-write path light.
        emit(onProgress, id, clip.filename, .syncing, nil, nil)

        var attempt = 0
        while true {
            do {
                let rec = try await client.upload(fileURL: clip.url, filename: clip.filename)
                store.set(id, SyncRecord(state: .synced, remoteID: rec.id.isEmpty ? nil : rec.id, lastError: nil))
                emit(onProgress, id, clip.filename, .synced, rec.id.isEmpty ? nil : rec.id, nil)
                await counters.didUpload()
                return
            } catch let err as UploadError {
                attempt += 1
                if err.isRetriable && attempt <= maxRetries {
                    // Exponential backoff with a small cap: 0.5s, 1s, 2s …
                    let delayMs = UInt64(min(8.0, 0.5 * pow(2.0, Double(attempt - 1))) * 1_000)
                    try? await Task.sleep(nanoseconds: delayMs * 1_000_000)
                    continue
                }
                store.set(id, SyncRecord(state: .failed, remoteID: nil, lastError: err.userMessage))
                emit(onProgress, id, clip.filename, .failed, nil, err.userMessage)
                await counters.didFail()
                return
            } catch {
                store.set(id, SyncRecord(state: .failed, remoteID: nil, lastError: error.localizedDescription))
                emit(onProgress, id, clip.filename, .failed, nil, error.localizedDescription)
                await counters.didFail()
                return
            }
        }
    }

    private func emit(
        _ cb: (@Sendable (SyncProgress) -> Void)?,
        _ id: ClipIdentity, _ name: String, _ state: SyncState,
        _ remoteID: String?, _ error: String?
    ) {
        cb?(SyncProgress(identity: id, filename: name, state: state, remoteID: remoteID, error: error))
    }
}

// MARK: - small concurrency helpers

/// Hands out the next index to a fixed pool of workers, exactly once each.
private actor AsyncIndex {
    private var i = 0
    private let count: Int
    init(count: Int) { self.count = count }
    func next() -> Int? {
        guard i < count else { return nil }
        defer { i += 1 }
        return i
    }
}

private actor Counters {
    private(set) var uploaded = 0
    private(set) var failed = 0
    func didUpload() { uploaded += 1 }
    func didFail() { failed += 1 }
    func snapshot() -> (uploaded: Int, failed: Int) { (uploaded, failed) }
}

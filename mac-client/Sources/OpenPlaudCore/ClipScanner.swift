import Foundation

/// Recursively finds `rec_*.wav` files (case-insensitive) under a folder or a
/// mounted volume, parses each WAV header for duration, and reconciles each with
/// the persisted sync state so the UI shows queued / syncing / synced / failed.
public enum ClipScanner {

    /// Case-insensitive match for the device's clip naming: `rec_*.wav`.
    /// The firmware emits `rec_NNNN.wav` (12 chars), but we accept any
    /// `rec_*.wav` so manually-renamed or future variants still sync.
    public static func isRecWAV(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower.hasPrefix("rec_") && lower.hasSuffix(".wav")
    }

    /// Scan `root` recursively. Returns clips sorted newest-first (by mtime),
    /// matching the mock's "just now / 2h ago / yesterday" ordering.
    ///
    /// - Parameter store: when provided, each clip's `state`/`remoteID`/`lastError`
    ///   are hydrated from the persisted record so synced clips render as synced.
    public static func scan(root: URL, store: SyncStateStore? = nil) -> [Clip] {
        let fm = FileManager.default
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey, .contentModificationDateKey, .isDirectoryKey]

        guard let enumerator = fm.enumerator(
            at: root,
            includingPropertiesForKeys: keys,
            options: [.skipsHiddenFiles],
            errorHandler: { _, _ in true } // skip unreadable entries, keep going
        ) else {
            return []
        }

        var clips: [Clip] = []
        for case let url as URL in enumerator {
            guard isRecWAV(url.lastPathComponent) else { continue }
            guard let values = try? url.resourceValues(forKeys: Set(keys)) else { continue }
            if values.isDirectory == true { continue }
            let size = values.fileSize ?? 0
            let mtime = values.contentModificationDate ?? Date(timeIntervalSince1970: 0)

            let identity = ClipIdentity(
                filename: url.lastPathComponent,
                sizeBytes: size,
                mtimeEpoch: Int(mtime.timeIntervalSince1970)
            )
            let duration = (try? WAVParser.parse(url: url))?.duration

            var clip = Clip(url: url, identity: identity, modifiedAt: mtime, duration: duration)
            if let rec = store?.record(for: identity) {
                clip.state = rec.state == .syncing ? .queued : rec.state // never resume "syncing" from disk
                clip.remoteID = rec.remoteID
                clip.lastError = rec.lastError
            }
            clips.append(clip)
        }

        clips.sort { $0.modifiedAt > $1.modifiedAt }
        return clips
    }
}

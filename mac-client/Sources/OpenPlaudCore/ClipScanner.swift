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

    /// Name of the device's "already uploaded" directory. The firmware records
    /// pending clips in the SD card root and *moves* each clip to `/sdcard/sent`
    /// after it has uploaded it directly (over Wi-Fi). See `firmware/.../rec_store`.
    public static let sentSubdir = "sent"

    /// True when `fileURL`'s first path component under `root` is `sent/`
    /// (case-insensitive). We use this to skip clips the device already sent.
    ///
    /// Why this matters: when the microSD mounts on the Mac, its root holds the
    /// pending `rec_*.wav` clips *and* a `sent/` folder of clips the device
    /// already uploaded. Without this guard the recursive scan would descend into
    /// `sent/`, re-list those clips as `queued`, and the Mac would re-upload every
    /// one on the first Sync (the server has no content-dedup, and the Mac's
    /// sync-state store has no record of device-side uploads). We deliberately
    /// only skip a *top-level* `sent/` so an ordinary user folder that happens to
    /// contain a `sent` subdir elsewhere is untouched.
    static func isInSentDir(fileURL: URL, root: URL) -> Bool {
        let rootComps = root.standardizedFileURL.pathComponents
        let fileComps = fileURL.standardizedFileURL.pathComponents
        guard fileComps.count > rootComps.count else { return false }
        // The component immediately below `root`.
        let first = fileComps[rootComps.count]
        return first.compare(sentSubdir, options: .caseInsensitive) == .orderedSame
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
            // Skip the device's already-uploaded `sent/` directory entirely so we
            // never re-list (and re-upload) clips the device sent over Wi-Fi.
            // `skipDescendants()` also avoids walking a card with hundreds of
            // sent clips.
            if isInSentDir(fileURL: url, root: root) {
                if (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
                    enumerator.skipDescendants()
                }
                continue
            }
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

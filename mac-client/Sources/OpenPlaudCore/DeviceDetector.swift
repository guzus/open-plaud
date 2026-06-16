import Foundation

/// A mounted volume that content-looks like the open-plaud recorder's storage.
///
/// "Content-looks like" is deliberate: we identify the device by what's on the
/// card (`rec_*.wav` clips), NOT by a volume name or USB VID/PID. That makes the
/// exact same code work for an SD in a card reader today AND a future
/// USB-Mass-Storage drive exposed directly by the device — both just present a
/// mounted volume whose root holds the recorder's clips.
public struct DetectedVolume: Identifiable, Sendable, Equatable {
    /// The mounted volume's root URL (e.g. `/Volumes/SD16G`). Stable identity for
    /// the UI list; also the `root` you hand to `ClipScanner.scan`.
    public let url: URL
    /// Human-readable volume name (e.g. `SD16G`), best-effort. Falls back to the
    /// last path component when the volume name resource isn't available.
    public let name: String
    /// How many `rec_*.wav` clips the shallow probe saw (root + `/sent`). This is
    /// a fast lower-bound signal for the card, NOT the authoritative clip count —
    /// the UI's real count comes from the deep `ClipScanner.scan` after selection.
    public let clipCountHint: Int

    public var id: String { url.path }

    public init(url: URL, name: String, clipCountHint: Int) {
        self.url = url
        self.name = name
        self.clipCountHint = clipCountHint
    }
}

/// Pure, Foundation-only detection of open-plaud device storage.
///
/// Lives in OpenPlaudCore (no DiskArbitration, no AppKit) so it stays
/// unit-testable and runs headlessly under `swift build` / `openplaud selftest` /
/// `openplaud detect`. The App layer wires this to DiskArbitration for live
/// mount/unmount reactions; this type only ever answers "does this look like the
/// device?" and "which mounted volumes look like the device?".
public enum DeviceDetector {

    /// Subdirectory the recorder moves uploaded clips into. We probe it too so a
    /// card whose root clips have all been filed under `/sent` still reads as the
    /// device.
    public static let sentSubdir = "sent"

    /// Does the directory at `url` look like open-plaud storage?
    ///
    /// Heuristic, content-based, and **shallow on purpose**: it reads only `url`
    /// itself plus a single `url/sent` subdirectory — it does NOT recurse. A
    /// recursive walk here would be a real performance trap, because the App
    /// runs this against *every* mounted volume (including the multi-million-file
    /// system disk) on every mount/unmount event. The deep recursive
    /// `ClipScanner.scan` runs only later, after a volume is selected.
    ///
    /// Returns a `DetectedVolume` (with a cheap clip-count hint) when the folder
    /// contains at least one `rec_*.wav` in its root or in `/sent`, else `nil`.
    public static func looksLikeOpenPlaudVolume(at url: URL) -> DetectedVolume? {
        let rootHits = recWAVCount(in: url)
        let sentHits = recWAVCount(in: url.appendingPathComponent(sentSubdir, isDirectory: true))
        let total = rootHits + sentHits
        guard total > 0 else { return nil }
        return DetectedVolume(url: url, name: volumeName(for: url), clipCountHint: total)
    }

    /// Enumerate currently-mounted volumes and return those that look like the
    /// device. Cheap enough to call on every mount/unmount event or on a poll
    /// tick: one `mountedVolumeURLs` call + one shallow probe per volume.
    ///
    /// Results are sorted by clip-count hint (most clips first), then name, so the
    /// UI's auto-pick is deterministic when (rarely) more than one card is in.
    public static func detectOpenPlaudVolumes(_ fm: FileManager = .default) -> [DetectedVolume] {
        // `.skipHiddenVolumes` keeps the system snapshot/preboot volumes out.
        let mounted = fm.mountedVolumeURLs(
            includingResourceValuesForKeys: [.volumeNameKey, .volumeIsRemovableKey],
            options: [.skipHiddenVolumes]
        ) ?? []

        var found: [DetectedVolume] = []
        for url in mounted {
            if let dv = looksLikeOpenPlaudVolume(at: url) { found.append(dv) }
        }
        found.sort {
            $0.clipCountHint != $1.clipCountHint
                ? $0.clipCountHint > $1.clipCountHint
                : $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
        return found
    }

    // MARK: - Internals

    /// Count `rec_*.wav` entries directly inside `dir` (non-recursive). Returns 0
    /// for a missing/unreadable directory. Stops early once it has counted enough
    /// to be confident, since callers only need "is there at least one?" plus a
    /// rough hint — but counting the whole shallow listing is already O(one dir).
    static func recWAVCount(in dir: URL) -> Int {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: dir,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles, .skipsSubdirectoryDescendants]
        ) else {
            return 0
        }
        var count = 0
        for entry in entries where ClipScanner.isRecWAV(entry.lastPathComponent) {
            count += 1
        }
        return count
    }

    /// Best-effort human-readable volume name; falls back to the path's last
    /// component (which, for `/Volumes/SD16G`, is the same thing anyway).
    static func volumeName(for url: URL) -> String {
        if let values = try? url.resourceValues(forKeys: [.volumeNameKey]),
           let name = values.volumeName, !name.isEmpty {
            return name
        }
        let last = url.lastPathComponent
        return last.isEmpty ? url.path : last
    }
}

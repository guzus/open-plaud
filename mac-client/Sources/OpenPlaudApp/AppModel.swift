import Foundation
import Combine
import OpenPlaudCore
import OpenPlaudUSB

/// The app's top-level device connection state, derived from TWO independent
/// signals: a mounted SD volume (content-based, from `DeviceWatcher`) and a USB
/// cable (VID-based, from `USBWatcher`).
///
/// Precedence is deliberate and non-regressing: a **volume always wins**. A
/// microSD in a generic card reader enumerates under the *reader's* USB vendor,
/// not Espressif's, so gating "connected" on a USB-VID match would break the
/// exact card-reader case the volume detection already handles. USB matching only
/// *adds* the new "recorder plugged in but no readable volume" state.
enum ConnectionState: Equatable {
    /// An SD volume is present (SD-in-reader today, or a future USB drive mode) →
    /// clips are readable, sync is enabled.
    case connectedWithVolume
    /// An Espressif/open-plaud device is on USB but exposes no volume (recorder
    /// mode, PID 0x1001) → cannot sync from the cable yet.
    case recorderNoVolume(USBDevice)
    /// Neither signal present.
    case none
}

/// Observable bridge between the SwiftUI views and the OpenPlaudCore engine.
/// Holds the current clip list, drives scans and syncs, and surfaces progress.
@MainActor
final class AppModel: ObservableObject {
    @Published var clips: [Clip] = []
    @Published var folderURL: URL?
    @Published var isSyncing = false
    @Published var isScanning = false
    @Published var statusLine: String = ""
    @Published var serverReachable: Bool? = nil

    /// Mounted volumes that content-look like the open-plaud device (from the
    /// background watcher). Drives the "Device connected" card / empty state.
    @Published var detectedVolumes: [DetectedVolume] = []

    /// Espressif (VID 0x303A) USB devices currently attached (from the USB
    /// watcher). Used for the "recorder mode, no volume" state and labeling.
    @Published var usbDevices: [USBDevice] = []

    /// True once the user has explicitly picked a folder (via the panel) or
    /// restored a saved bookmark. While true we never auto-clobber their choice
    /// with an auto-detected device.
    @Published private(set) var userChoseFolder = false

    let settings: AppSettings
    private let store: SyncStateStore
    private var watcher: DeviceWatcher?
    private var usbWatcher: USBWatcher?

    init(settings: AppSettings) {
        self.settings = settings
        self.store = SyncStateStore()
        if let url = settings.resolveFolderBookmark() {
            self.folderURL = url
            self.userChoseFolder = true
        }
    }

    var queuedCount: Int { clips.filter { $0.state == .queued || $0.state == .failed }.count }
    var syncedCount: Int { clips.filter { $0.state == .synced }.count }

    /// The detected device whose volume is the one we're currently scanning, if
    /// any (so the UI can label the card with the live clip counts).
    var activeDevice: DetectedVolume? {
        guard let folderURL else { return nil }
        return detectedVolumes.first { $0.url == folderURL }
    }

    /// True when the current sync source is an auto-detected device volume the
    /// user did not pick by hand.
    var isUsingAutoDetectedDevice: Bool {
        !userChoseFolder && activeDevice != nil
    }

    /// The open-plaud USB device currently attached, if any (first Espressif
    /// device). Used to label states and detect recorder mode.
    var usbRecorder: USBDevice? { usbDevices.first }

    /// Derived top-level connection state. Volume presence wins (non-regressing —
    /// see `ConnectionState`); USB only contributes the recorder-no-volume state.
    var connectionState: ConnectionState {
        if folderURL != nil {
            // A readable source is selected (auto-detected device, or a folder the
            // user chose). Either way clips are syncable. (`activeDevice != nil`
            // implies this, so checking `folderURL` alone suffices.)
            return .connectedWithVolume
        }
        if let usb = usbDevices.first {
            // Cable present but no mountable volume yet (recorder mode today).
            return .recorderNoVolume(usb)
        }
        return .none
    }

    // MARK: - Device detection wiring

    /// Start watching for the device mounting/unmounting (SD volume) AND for the
    /// device being plugged in over USB. Safe to call once at launch; both
    /// watchers deliver an immediate snapshot so an already-present device shows.
    func startWatchingForDevice() {
        if watcher == nil {
            let w = DeviceWatcher { [weak self] volumes in
                self?.deviceVolumesChanged(volumes)
            }
            watcher = w
            w.start()
        }
        if usbWatcher == nil {
            let u = USBWatcher { [weak self] devices in
                self?.usbDevicesChanged(devices)
            }
            usbWatcher = u
            u.start()
        }
    }

    /// React to a change in the set of attached open-plaud USB devices. Pure
    /// presence/labeling — it never changes the sync *source* (volume detection
    /// owns that). Equality-guarded so duplicate snapshots are no-ops.
    private func usbDevicesChanged(_ devices: [USBDevice]) {
        guard devices != usbDevices else { return }
        usbDevices = devices
    }

    /// React to a change in the set of detected device volumes.
    ///
    /// Rules (don't-clobber + auto-pick):
    ///  - If the user picked a folder by hand, never change their selection here;
    ///    just keep the detected list fresh for the UI.
    ///  - Otherwise auto-pick the top detected device as the sync source (kept
    ///    in-memory only — a transient SD mount is never persisted as a bookmark).
    ///  - If the auto-picked device unmounts, drop the selection and clear clips.
    private func deviceVolumesChanged(_ volumes: [DetectedVolume]) {
        // Ignore steady-state polls: the watcher re-probes every ~2 s, so without
        // this guard an unchanged set would republish to SwiftUI and (worse, for
        // an all-sent device whose root is empty) re-fire scan() forever. Only act
        // on a genuine change to the detected set. `DetectedVolume` is Equatable.
        guard volumes != detectedVolumes else { return }
        let previous = detectedVolumes
        detectedVolumes = volumes
        reconcileAutoSelection(previous: previous)
    }

    /// Pick (or release) the auto-detected device based on the current
    /// `detectedVolumes`, honoring a manual choice. Safe to call whenever the set
    /// changes or when switching back to auto-detect.
    private func reconcileAutoSelection(previous: [DetectedVolume]) {
        guard !userChoseFolder else { return } // never clobber a manual choice

        if let current = folderURL, detectedVolumes.contains(where: { $0.url == current }) {
            // We're already on a detected device that's still mounted → stay put.
            // (Deliberately do NOT jump to a newly-inserted second card with more
            // clips; switching an in-progress source out from under the user is
            // surprising. The replace case — this device unmounting — is handled
            // below.)
            return
        }

        if let device = detectedVolumes.first {
            // No current selection (or the one we had vanished) → adopt the top
            // detected device. Kept in-memory only; a transient SD mount is never
            // persisted as a bookmark.
            selectSource(device.url, userChosen: false)
        } else if folderURL != nil, previous.contains(where: { $0.url == folderURL }) {
            // The auto-detected device we were using went away → reset to empty.
            folderURL = nil
            clips = []
            statusLine = "Device removed."
        }
    }

    func chooseFolder(_ url: URL) {
        settings.saveFolderBookmark(url)
        selectSource(url, userChosen: true)
    }

    /// Forget a hand-picked folder and fall back to auto-detection. Lets the user
    /// "un-stick" a manual choice so an inserted device takes over again.
    func useAutoDetect() {
        settings.clearFolderBookmark()
        userChoseFolder = false
        folderURL = nil
        clips = []
        // Re-evaluate against whatever is currently mounted. (Pass the current set
        // as `previous` so the unmount branch isn't taken; there's nothing to
        // "lose" here — we just adopt whatever is mounted, or land on empty.)
        reconcileAutoSelection(previous: detectedVolumes)
    }

    /// Switch the sync source and (re)scan. Clears the previous clip list up front
    /// so the UI never shows the new source labeled with the old source's count.
    private func selectSource(_ url: URL, userChosen: Bool) {
        folderURL = url
        userChoseFolder = userChosen
        clips = []          // avoid a stale count flashing in the device banner
        scan()
    }

    func scan() {
        guard let folder = folderURL else { return }
        isScanning = true
        statusLine = "Scanning…"
        let store = self.store
        // Capture the source this scan is for, so that if the user/device switches
        // sources mid-scan, a late-finishing scan discards its result instead of
        // overwriting the current list (lost-update race on rapid switches).
        Task.detached {
            // Access the security scope while scanning a bookmarked folder.
            let scoped = folder.startAccessingSecurityScopedResource()
            defer { if scoped { folder.stopAccessingSecurityScopedResource() } }
            let found = ClipScanner.scan(root: folder, store: store)
            await MainActor.run {
                // Discard a stale result if the source changed while we scanned.
                guard self.folderURL == folder else { return }
                self.clips = found
                self.isScanning = false
                self.statusLine = found.isEmpty ? "No rec_*.wav found in this folder." : "\(found.count) clip(s)."
            }
        }
    }

    func checkHealth() {
        guard let base = settings.serverURL else { serverReachable = false; return }
        let client = UploadClient(config: ServerConfig(baseURL: base))
        Task {
            let ok = (try? await client.health()) ?? false
            await MainActor.run { self.serverReachable = ok }
        }
    }

    func sync() {
        guard let folder = folderURL, let base = settings.serverURL else { return }
        guard !isSyncing else { return }
        let token = settings.currentToken() // read from Keychain only at use time
        let toSync = clips.filter { $0.state == .queued || $0.state == .failed }
        guard !toSync.isEmpty else { statusLine = "Nothing to sync."; return }

        isSyncing = true
        statusLine = "Syncing \(toSync.count) clip(s)…"

        let store = self.store // capture locally so the detached task doesn't retain self
        let client = UploadClient(config: ServerConfig(baseURL: base, token: token))
        let engine = SyncEngine(client: client, store: store, maxConcurrent: 3)

        Task.detached {
            let scoped = folder.startAccessingSecurityScopedResource()
            defer { if scoped { folder.stopAccessingSecurityScopedResource() } }

            let summary = await engine.sync(clips: toSync) { progress in
                // Update the matching row on the main actor as each clip transitions.
                Task { @MainActor in
                    if let idx = self.clips.firstIndex(where: { $0.identity == progress.identity }) {
                        self.clips[idx].state = progress.state
                        self.clips[idx].remoteID = progress.remoteID
                        self.clips[idx].lastError = progress.error
                    }
                }
            }
            // Re-hydrate from the store so the final view matches durable truth.
            let refreshed = ClipScanner.scan(root: folder, store: store)
            await MainActor.run {
                self.clips = refreshed
                self.isSyncing = false
                var line = "Uploaded \(summary.uploaded), skipped \(summary.skipped)"
                if summary.failed > 0 { line += ", failed \(summary.failed)" }
                self.statusLine = line + "."
            }
        }
    }
}

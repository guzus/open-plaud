import Foundation
import Combine
import OpenPlaudCore

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

    let settings: AppSettings
    private let store: SyncStateStore

    init(settings: AppSettings) {
        self.settings = settings
        self.store = SyncStateStore()
        if let url = settings.resolveFolderBookmark() {
            self.folderURL = url
        }
    }

    var queuedCount: Int { clips.filter { $0.state == .queued || $0.state == .failed }.count }
    var syncedCount: Int { clips.filter { $0.state == .synced }.count }

    func chooseFolder(_ url: URL) {
        settings.saveFolderBookmark(url)
        folderURL = url
        scan()
    }

    func scan() {
        guard let folder = folderURL else { return }
        isScanning = true
        statusLine = "Scanning…"
        let store = self.store
        Task.detached {
            // Access the security scope while scanning a bookmarked folder.
            let scoped = folder.startAccessingSecurityScopedResource()
            defer { if scoped { folder.stopAccessingSecurityScopedResource() } }
            let found = ClipScanner.scan(root: folder, store: store)
            await MainActor.run {
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

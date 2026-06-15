import SwiftUI
import UniformTypeIdentifiers
import OpenPlaudCore

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var settings: AppSettings
    @State private var showingSettings = false
    @State private var confirmNoToken = false

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if model.folderURL == nil {
                emptyState
            } else if model.clips.isEmpty {
                emptyFolderState
            } else {
                clipList
            }
            Divider()
            footer
        }
        .sheet(isPresented: $showingSettings) {
            SettingsView(asSheet: true) { showingSettings = false }
                .environmentObject(settings)
                .environmentObject(model)
                .frame(width: 460)
        }
        .alert("No auth token set", isPresented: $confirmNoToken) {
            Button("Open Settings") { showingSettings = true }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The server requires an Authorization token. Add it in Settings before syncing.")
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(model.serverReachable == true ? Color.green : (model.serverReachable == false ? Color.red : Color.secondary))
                .frame(width: 9, height: 9)
            VStack(alignment: .leading, spacing: 1) {
                Text(model.folderURL == nil ? "No folder selected" : (model.folderURL?.lastPathComponent ?? ""))
                    .font(.headline)
                Text(serverStatusText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                chooseFolder()
            } label: {
                Label(model.folderURL == nil ? "Choose Folder…" : "Change…", systemImage: "folder")
            }
            Button {
                showingSettings = true
            } label: {
                Label("Settings", systemImage: "gearshape")
            }
        }
        .padding(12)
    }

    private var serverStatusText: String {
        let host = settings.serverURL?.host ?? settings.serverURLString
        switch model.serverReachable {
        case .some(true): return "Connected · \(host)"
        case .some(false): return "Unreachable · \(host)"
        case .none: return host
        }
    }

    // MARK: - States

    private var emptyState: some View {
        VStack(spacing: 14) {
            Spacer()
            Image(systemName: "waveform.circle")
                .font(.system(size: 52))
                .foregroundStyle(.secondary)
            Text("Sync your open-plaud recordings")
                .font(.title3.weight(.semibold))
            Text("Insert the device's microSD card (or choose a folder of clips).\nThe app finds every rec_*.wav and uploads it to your server.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                chooseFolder()
            } label: {
                Label("Choose Folder or SD Card…", systemImage: "folder.badge.plus")
            }
            .controlSize(.large)
            .buttonStyle(.borderedProminent)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    private var emptyFolderState: some View {
        VStack(spacing: 10) {
            Spacer()
            Image(systemName: "tray")
                .font(.system(size: 40))
                .foregroundStyle(.secondary)
            Text("No rec_*.wav files here")
                .font(.headline)
            Text(model.folderURL?.path ?? "")
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .truncationMode(.middle)
            Button("Rescan") { model.scan() }
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    private var clipList: some View {
        List(model.clips) { clip in
            ClipRow(clip: clip)
        }
        .listStyle(.inset)
    }

    // MARK: - Footer

    private var footer: some View {
        HStack {
            Text(model.statusLine.isEmpty ? "\(model.clips.count) clip(s) · \(model.syncedCount) synced" : model.statusLine)
                .font(.caption)
                .foregroundStyle(.secondary)
            Spacer()
            if model.isSyncing {
                ProgressView().controlSize(.small)
            }
            Button {
                startSync()
            } label: {
                Text(model.queuedCount > 0 ? "Sync \(model.queuedCount) clip\(model.queuedCount == 1 ? "" : "s")" : "All Synced")
                    .fontWeight(.semibold)
            }
            .buttonStyle(.borderedProminent)
            .disabled(model.isSyncing || model.queuedCount == 0 || model.folderURL == nil)
            .keyboardShortcut("s", modifiers: [.command])
        }
        .padding(12)
    }

    // MARK: - Actions

    private func startSync() {
        if !settings.hasToken {
            confirmNoToken = true
            return
        }
        model.sync()
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Select the folder or mounted SD card containing rec_*.wav clips"
        if panel.runModal() == .OK, let url = panel.url {
            model.chooseFolder(url)
        }
    }
}

/// One clip row: filename, duration · relative time, and a status pill.
struct ClipRow: View {
    let clip: Clip

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "waveform")
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(clip.filename)
                    .font(.system(.body, design: .monospaced))
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            StatusPill(state: clip.state)
        }
        .padding(.vertical, 3)
        .help(clip.lastError ?? "")
    }

    private var subtitle: String {
        var parts: [String] = []
        if let d = clip.duration { parts.append(formatDuration(d)) }
        parts.append(formatSize(clip.sizeBytes))
        parts.append(relativeTime(clip.modifiedAt))
        return parts.joined(separator: " · ")
    }

    private func formatDuration(_ s: TimeInterval) -> String {
        let t = Int(s.rounded()); return String(format: "%d:%02d", t / 60, t % 60)
    }
    private func formatSize(_ b: Int) -> String {
        if b >= 1_048_576 { return String(format: "%.1f MB", Double(b) / 1_048_576) }
        if b >= 1024 { return String(format: "%.0f KB", Double(b) / 1024) }
        return "\(b) B"
    }
    private func relativeTime(_ date: Date) -> String {
        let f = RelativeDateTimeFormatter(); f.unitsStyle = .abbreviated
        return f.localizedString(for: date, relativeTo: Date())
    }
}

struct StatusPill: View {
    let state: SyncState

    var body: some View {
        Text(label)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .background(color.opacity(0.18), in: Capsule())
            .foregroundStyle(color)
    }

    private var label: String {
        switch state {
        case .queued: return "queued"
        case .syncing: return "syncing"
        case .synced: return "synced ✓"
        case .failed: return "failed"
        }
    }
    private var color: Color {
        switch state {
        case .queued: return .orange
        case .syncing: return .blue
        case .synced: return .green
        case .failed: return .red
        }
    }
}

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
            if model.activeDevice != nil {
                deviceBanner
                Divider()
            }
            content
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
            // Escape hatch for "don't clobber": if the user picked a folder by
            // hand but a device is now mounted, let them switch to it.
            if model.userChoseFolder, let dev = model.detectedVolumes.first, dev.url != model.folderURL {
                Button {
                    model.useAutoDetect()
                } label: {
                    Label("Use Device", systemImage: "externaldrive")
                }
                .help("Switch to the detected open-plaud device (\(dev.name))")
            }
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

    // MARK: - Device banner (distinct from the server-reachability dot)

    /// "Device connected — <volume>, N clips (M to sync)", matching the prototype.
    /// Uses its own green dot so it doesn't collide with the header's server dot.
    private var deviceBanner: some View {
        HStack(spacing: 8) {
            Circle().fill(Color.green).frame(width: 8, height: 8)
            Text("Device connected")
                .font(.subheadline.weight(.semibold))
            if let dev = model.activeDevice {
                Text(deviceDetail(dev))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if model.isUsingAutoDetectedDevice {
                Text("auto-detected")
                    .font(.caption2.weight(.medium))
                    .padding(.horizontal, 6).padding(.vertical, 2)
                    .background(Color.green.opacity(0.15), in: Capsule())
                    .foregroundStyle(.green)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(Color.green.opacity(0.06))
    }

    private func deviceDetail(_ dev: DetectedVolume) -> String {
        let n = model.clips.count
        let m = model.queuedCount
        let clips = "\(n) clip\(n == 1 ? "" : "s")"
        return m > 0 ? "\(dev.name) · \(clips) (\(m) to sync)" : "\(dev.name) · \(clips)"
    }

    // MARK: - States

    /// Main content area, chosen by the derived connection state. A selected
    /// source (device or folder) shows the clip list / empty-folder; otherwise we
    /// show recorder-mode (USB cable, no volume) or the plain empty state.
    @ViewBuilder
    private var content: some View {
        if model.folderURL != nil {
            if model.clips.isEmpty { emptyFolderState } else { clipList }
        } else if case .recorderNoVolume(let usb) = model.connectionState {
            recorderModeState(usb)
        } else {
            emptyState
        }
    }

    /// USB cable present but no mountable volume (recorder firmware today).
    /// Deliberately distinct from "Device connected" and does NOT offer Sync —
    /// clips aren't readable over the cable yet.
    private func recorderModeState(_ usb: USBDevice) -> some View {
        VStack(spacing: 14) {
            Spacer()
            Image(systemName: "cable.connector")
                .font(.system(size: 48))
                .foregroundStyle(.orange)
                .symbolRenderingMode(.hierarchical)
            Text("\(usb.displayName) connected (recorder mode)")
                .font(.title3.weight(.semibold))
                .multilineTextAlignment(.center)
            Text("The device is plugged in over USB, but its clips aren't readable\nover the cable yet. Insert its microSD here, or sync over Wi-Fi.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Text(usbDetail(usb))
                .font(.caption.monospaced())
                .foregroundStyle(.tertiary)
            Button {
                chooseFolder()
            } label: {
                Label("Insert microSD / choose a folder…", systemImage: "sdcard")
            }
            .controlSize(.large)
            .buttonStyle(.bordered)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }

    private func usbDetail(_ usb: USBDevice) -> String {
        let vid = String(format: "0x%04X", usb.vendorID)
        let pid = String(format: "0x%04X", usb.productID)
        return "USB \(vid):\(pid) · \(usb.mode.label)"
    }

    private var emptyState: some View {
        VStack(spacing: 14) {
            Spacer()
            Image(systemName: "externaldrive.badge.questionmark")
                .font(.system(size: 50))
                .foregroundStyle(.secondary)
                .symbolRenderingMode(.hierarchical)
            Text("Plug in the device or insert its microSD")
                .font(.title3.weight(.semibold))
                .multilineTextAlignment(.center)
            Text("open-plaud recognizes the device over USB and auto-detects its\nstorage the moment it mounts, then syncs every rec_*.wav to your server.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            // Searching indicator so the empty state doesn't feel inert.
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text("Waiting for a device…").font(.caption).foregroundStyle(.secondary)
            }
            .padding(.top, 2)
            Button {
                chooseFolder()
            } label: {
                Label("Or choose a folder of clips…", systemImage: "folder.badge.plus")
            }
            .controlSize(.large)
            .buttonStyle(.bordered)
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

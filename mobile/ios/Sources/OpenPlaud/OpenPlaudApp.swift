import OpenPlaudCompanion
import SwiftUI

@main
struct OpenPlaudApp: App {
    @StateObject private var deviceClient = OpenPlaudDeviceClient()

    var body: some Scene {
        WindowGroup {
            OpenPlaudRootView(deviceClient: deviceClient)
        }
    }
}

struct OpenPlaudRootView: View {
    private enum AppTab: Hashable {
        case connect
        case upload
    }

    @ObservedObject var deviceClient: OpenPlaudDeviceClient

    @State private var selectedTab: AppTab = .connect
    @State private var wifiSSID = ""
    @State private var wifiPassword = ""
    @State private var serverURL = ""
    @State private var authToken = ""
    @State private var isSetupExpanded = false
    @State private var isAdvancedExpanded = false
    @State private var route: OpenPlaudTransferRoute = .wifi
    @State private var compression: OpenPlaudTransferCompression = .imaAdpcm

    var body: some View {
        TabView(selection: $selectedTab) {
            connectTab
                .tabItem {
                    Label("Connect", systemImage: "dot.radiowaves.left.and.right")
                }
                .tag(AppTab.connect)

            uploadTab
                .tabItem {
                    Label("Upload", systemImage: "arrow.up.circle")
                }
                .tag(AppTab.upload)
        }
    }

    private var connectTab: some View {
        NavigationStack {
            ScrollView {
                LazyVStack(spacing: 16) {
                    connectionCard

                    if let lastError = deviceClient.lastError {
                        ErrorBanner(message: lastError)
                    }

                    deviceStatusCard
                    devicesCard
                }
                .padding(16)
                .padding(.bottom, 72)
            }
            .background(OpenPlaudTheme.screenBackground.ignoresSafeArea())
            .navigationTitle("Connect")
            .openPlaudNavigationTitleMode()
            .toolbar {
                ToolbarItem {
                    Button {
                        deviceClient.requestStatus()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .disabled(!isReady)
                    .accessibilityLabel("Refresh status")
                }
            }
        }
    }

    private var uploadTab: some View {
        NavigationStack {
            ScrollView {
                LazyVStack(spacing: 16) {
                    if let status = deviceClient.status {
                        uploadSummaryCard(status)
                    }

                    if let lastError = deviceClient.lastError {
                        ErrorBanner(message: lastError)
                    }

                    uploadActionCard
                    setupCard
                    advancedCard
                }
                .padding(16)
                .padding(.bottom, 72)
            }
            .background(OpenPlaudTheme.screenBackground.ignoresSafeArea())
            .navigationTitle("Upload")
            .openPlaudNavigationTitleMode()
            .toolbar {
                ToolbarItem {
                    Button {
                        deviceClient.requestStatus()
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .disabled(!isReady)
                    .accessibilityLabel("Refresh status")
                }
            }
        }
    }

    private var connectionCard: some View {
        OpenPlaudCard {
            VStack(alignment: .leading, spacing: 18) {
                HStack(alignment: .top, spacing: 14) {
                    ZStack {
                        Circle()
                            .fill(connectionTint.opacity(0.14))
                        Image(systemName: connectionIconName)
                            .font(.system(size: 24, weight: .semibold))
                            .foregroundStyle(connectionTint)
                    }
                    .frame(width: 54, height: 54)

                    VStack(alignment: .leading, spacing: 5) {
                        Text(connectionStateText)
                            .font(.title2.weight(.semibold))
                            .foregroundStyle(.primary)
                        Text("Bluetooth \(bluetoothStateText.lowercased())")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }

                    Spacer(minLength: 0)

                    StatusPill(title: readyPillTitle, tint: connectionTint)
                }

                HStack(spacing: 10) {
                    Button {
                        deviceClient.startScanning()
                    } label: {
                        Label(scanButtonTitle, systemImage: "dot.radiowaves.left.and.right")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    .disabled(deviceClient.bluetoothState != .poweredOn)

                    Button(role: .destructive) {
                        deviceClient.disconnect()
                    } label: {
                        Image(systemName: "xmark")
                            .frame(width: 42, height: 42)
                    }
                    .buttonStyle(.bordered)
                    .disabled(!isConnected)
                    .accessibilityLabel("Disconnect")
                }
            }
        }
    }

    private var deviceStatusCard: some View {
        OpenPlaudCard {
            VStack(alignment: .leading, spacing: 16) {
                SectionHeader("Device", systemImage: "record.circle")

                if let status = deviceClient.status {
                    VStack(alignment: .leading, spacing: 14) {
                        HStack(alignment: .top, spacing: 12) {
                            MetricTile(
                                title: "Battery",
                                value: status.batteryDisplay,
                                systemImage: status.batteryIconName,
                                tint: status.batteryTint
                            )
                            MetricTile(
                                title: "Queue",
                                value: status.queueDisplay,
                                systemImage: "tray.full",
                                tint: status.queueTint
                            )
                        }

                        VStack(spacing: 10) {
                            DetailRow(title: "Sync", value: status.syncState.displayName, systemImage: "arrow.triangle.2.circlepath")
                            DetailRow(title: "Wi-Fi", value: status.wifiSSID ?? "Not set", systemImage: "wifi")
                            if let firmwareVersion = status.firmwareVersion {
                                DetailRow(title: "Firmware", value: firmwareVersion, systemImage: "cpu")
                            }
                            if let activeClip = status.activeClip {
                                DetailRow(title: "Clip", value: activeClip, systemImage: "waveform")
                            }
                            if let preference = status.transferPreference {
                                DetailRow(
                                    title: "Transfer",
                                    value: "\(preference.route.displayName) / \(preference.compression.label)",
                                    systemImage: "point.3.connected.trianglepath.dotted"
                                )
                            }
                        }

                        if let transferPercent = status.transferPercent {
                            VStack(alignment: .leading, spacing: 8) {
                                HStack {
                                    Text("Transfer")
                                        .font(.subheadline.weight(.medium))
                                    Spacer()
                                    Text("\(transferPercent)%")
                                        .font(.subheadline.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                }
                                ProgressView(value: Double(transferPercent), total: 100)
                            }
                        }

                        if let message = status.message {
                            Text(message)
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(10)
                                .background(OpenPlaudTheme.subtleFill, in: RoundedRectangle(cornerRadius: 8))
                        }
                    }
                } else {
                    EmptyState(
                        title: "No status yet",
                        systemImage: "waveform.badge.magnifyingglass",
                        detail: "Connect to an Open Plaud device, then refresh."
                    )
                }
            }
        }
    }

    private var devicesCard: some View {
        OpenPlaudCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    SectionHeader("Open Plaud", systemImage: "antenna.radiowaves.left.and.right")
                    Spacer()
                    Text("\(deviceClient.discoveredDevices.count)")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .padding(.horizontal, 9)
                        .padding(.vertical, 5)
                        .background(OpenPlaudTheme.subtleFill, in: Capsule())
                }

                if deviceClient.discoveredDevices.isEmpty {
                    EmptyState(
                        title: "No Open Plaud found",
                        systemImage: "antenna.radiowaves.left.and.right.slash",
                        detail: "Tap Scan while Open Plaud is awake."
                    )
                } else {
                    VStack(spacing: 0) {
                        ForEach(deviceClient.discoveredDevices) { device in
                            Button {
                                deviceClient.connect(to: device)
                            } label: {
                                DeviceRow(device: device)
                            }
                            .buttonStyle(.plain)
                            .disabled(!device.isConnectable)

                            if device.id != deviceClient.discoveredDevices.last?.id {
                                Divider()
                                    .padding(.leading, 48)
                            }
                        }
                    }
                }
            }
        }
    }

    private var setupCard: some View {
        OpenPlaudCard {
            DisclosureGroup(isExpanded: $isSetupExpanded) {
                VStack(alignment: .leading, spacing: 16) {
                    VStack(spacing: 12) {
                        TextField("Wi-Fi SSID", text: $wifiSSID)
                            .openPlaudCredentialInputStyle()
                            .openPlaudFieldStyle()
                        SecureField("Wi-Fi password", text: $wifiPassword)
                            .openPlaudCredentialInputStyle()
                            .openPlaudFieldStyle()

                        Button {
                            deviceClient.configureWiFi(ssid: wifiSSID, password: wifiPassword)
                        } label: {
                            Label("Send Wi-Fi", systemImage: "wifi")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!isReady || wifiSSID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }

                    Divider()

                    VStack(spacing: 12) {
                        TextField("Server URL", text: $serverURL)
                            .openPlaudURLInputStyle()
                            .openPlaudFieldStyle()
                        SecureField("Auth token", text: $authToken)
                            .openPlaudFieldStyle()

                        Button {
                            guard let url = URL(string: serverURL) else {
                                return
                            }
                            deviceClient.configureServer(url: url, authToken: authToken)
                        } label: {
                            Label("Send Server", systemImage: "server.rack")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                        .disabled(!isReady || URL(string: serverURL) == nil)
                    }
                }
                .padding(.top, 12)
            } label: {
                SectionHeader("Setup", systemImage: "slider.horizontal.3")
            }
            .tint(.primary)
        }
    }

    private var uploadActionCard: some View {
        OpenPlaudCard {
            VStack(alignment: .leading, spacing: 16) {
                SectionHeader("Recordings", systemImage: "tray.full")

                if deviceClient.status == nil {
                    EmptyState(
                        title: "No device status",
                        systemImage: "waveform.badge.magnifyingglass",
                        detail: "Connect to Open Plaud first."
                    )
                }

                Button {
                    deviceClient.setTransferPreference(route: .wifi, compression: .imaAdpcm)
                    deviceClient.startWiFiSync()
                } label: {
                    Label("Upload recordings", systemImage: "arrow.up.circle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(!isReady)

                Button(role: .destructive) {
                    deviceClient.stopSync()
                } label: {
                    Label("Stop upload", systemImage: "stop.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(!isReady)
            }
        }
    }

    private var advancedCard: some View {
        OpenPlaudCard {
            DisclosureGroup(isExpanded: $isAdvancedExpanded) {
                VStack(alignment: .leading, spacing: 16) {
                    VStack(alignment: .leading, spacing: 12) {
                        Picker("Route", selection: $route) {
                            ForEach(OpenPlaudTransferRoute.allCases, id: \.self) { route in
                                Text(route.displayName).tag(route)
                            }
                        }
                        .pickerStyle(.segmented)

                        Picker("Compression", selection: $compression) {
                            ForEach(OpenPlaudTransferCompression.allCases, id: \.self) { compression in
                                Text(compression.shortLabel).tag(compression)
                            }
                        }
                        .pickerStyle(.segmented)
                    }

                    Button {
                        deviceClient.setTransferPreference(route: route, compression: compression)
                    } label: {
                        Label("Set transfer preference", systemImage: "checkmark.circle")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .disabled(!isReady)

                    Divider()

                    HStack(spacing: 10) {
                        Button {
                            deviceClient.prepareBluetoothClipTransfer(codec: .imaAdpcm)
                        } label: {
                            Label("Prepare BLE", systemImage: "point.3.connected.trianglepath.dotted")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                        .disabled(!isReady)

                        Button {
                            deviceClient.readNextClipChunk()
                        } label: {
                            Label("Read Chunk", systemImage: "square.and.arrow.down")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.bordered)
                        .disabled(!isReady)
                    }

                    if let cursor = deviceClient.clipTransferCursor {
                        DetailRow(
                            title: "BLE cursor",
                            value: "\(cursor.codec.rawValue) @ \(cursor.offset)",
                            systemImage: "location"
                        )
                    }

                    if let chunk = deviceClient.lastClipChunk {
                        DetailRow(
                            title: "BLE chunk",
                            value: "\(chunk.state.label), \(chunk.payloadLength) B, \(chunk.offsetEnd)/\(chunk.totalBytes)",
                            systemImage: "shippingbox"
                        )
                    }

                    if !deviceClient.commandLog.isEmpty {
                        Divider()
                        commandLogRows
                    }
                }
                .padding(.top, 12)
            } label: {
                SectionHeader("Advanced", systemImage: "wrench.and.screwdriver")
            }
            .tint(.primary)
        }
    }

    private func uploadSummaryCard(_ status: OpenPlaudDeviceStatus) -> some View {
        OpenPlaudCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .top, spacing: 12) {
                    ZStack {
                        Circle()
                            .fill(status.syncState.tint.opacity(0.14))
                        Image(systemName: status.syncState.iconName)
                            .font(.system(size: 22, weight: .semibold))
                            .foregroundStyle(status.syncState.tint)
                    }
                    .frame(width: 48, height: 48)

                    VStack(alignment: .leading, spacing: 4) {
                        Text(status.syncState.displayName)
                            .font(.title3.weight(.semibold))
                        Text(status.queueSummary)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }

                    Spacer(minLength: 0)
                }

                if let transferPercent = status.transferPercent {
                    ProgressView(value: Double(transferPercent), total: 100)
                }
            }
        }
    }

    private var commandLogRows: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionHeader("Command log", systemImage: "terminal")

            VStack(spacing: 0) {
                ForEach(Array(deviceClient.commandLog.enumerated()), id: \.offset) { index, entry in
                    HStack(spacing: 10) {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                        Text(entry)
                            .font(.subheadline)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .padding(.vertical, 8)

                    if index < deviceClient.commandLog.count - 1 {
                        Divider()
                            .padding(.leading, 28)
                    }
                }
            }
        }
    }

    private var isConnected: Bool {
        if case .disconnected = deviceClient.connectionState {
            return false
        }
        if case .failed = deviceClient.connectionState {
            return false
        }
        return true
    }

    private var isReady: Bool {
        if case .ready = deviceClient.connectionState {
            return true
        }
        return false
    }

    private var readyPillTitle: String {
        isReady ? "Ready" : "Offline"
    }

    private var scanButtonTitle: String {
        if case .scanning = deviceClient.connectionState {
            return "Scanning"
        }
        return "Scan"
    }

    private var bluetoothStateText: String {
        switch deviceClient.bluetoothState {
        case .unknown:
            return "Unknown"
        case .unsupported:
            return "Unsupported"
        case .unauthorized:
            return "Unauthorized"
        case .poweredOff:
            return "Off"
        case .poweredOn:
            return "On"
        }
    }

    private var connectionStateText: String {
        switch deviceClient.connectionState {
        case .disconnected:
            return "Disconnected"
        case .scanning:
            return "Scanning"
        case .connecting:
            return "Connecting"
        case .discovering:
            return "Discovering"
        case .ready:
            return "Connected"
        case .failed(let message):
            return "Failed: \(message)"
        }
    }

    private var connectionIconName: String {
        switch deviceClient.connectionState {
        case .ready:
            return "checkmark.circle.fill"
        case .scanning, .connecting, .discovering:
            return "dot.radiowaves.left.and.right"
        case .failed:
            return "exclamationmark.triangle.fill"
        case .disconnected:
            return "wave.3.right"
        }
    }

    private var connectionTint: Color {
        switch deviceClient.connectionState {
        case .ready:
            return .green
        case .scanning, .connecting, .discovering:
            return .blue
        case .failed:
            return .red
        case .disconnected:
            return .secondary
        }
    }
}

private struct OpenPlaudCard<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        content
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(OpenPlaudTheme.cardBackground, in: RoundedRectangle(cornerRadius: 8))
            .overlay {
                RoundedRectangle(cornerRadius: 8)
                    .stroke(OpenPlaudTheme.cardBorder)
            }
    }
}

private struct SectionHeader: View {
    let title: String
    let systemImage: String

    init(_ title: String, systemImage: String) {
        self.title = title
        self.systemImage = systemImage
    }

    var body: some View {
        Label(title, systemImage: systemImage)
            .font(.headline)
            .foregroundStyle(.primary)
    }
}

private struct StatusPill: View {
    let title: String
    let tint: Color

    var body: some View {
        Text(title)
            .font(.caption.weight(.semibold))
            .foregroundStyle(tint)
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(tint.opacity(0.12), in: Capsule())
    }
}

private struct MetricTile: View {
    let title: String
    let value: String
    let systemImage: String
    let tint: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Image(systemName: systemImage)
                    .foregroundStyle(tint)
                Spacer()
            }
            Text(value)
                .font(.title3.weight(.semibold))
                .lineLimit(1)
                .minimumScaleFactor(0.75)
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(tint.opacity(0.10), in: RoundedRectangle(cornerRadius: 8))
    }
}

private struct DetailRow: View {
    let title: String
    let value: String
    let systemImage: String

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: systemImage)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .frame(width: 24)

            Text(title)
                .font(.subheadline)
                .foregroundStyle(.secondary)

            Spacer(minLength: 12)

            Text(value)
                .font(.subheadline.weight(.medium))
                .multilineTextAlignment(.trailing)
                .foregroundStyle(.primary)
        }
    }
}

private struct DeviceRow: View {
    let device: OpenPlaudDiscoveredDevice

    var body: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(device.isConnectable ? Color.blue.opacity(0.12) : Color.secondary.opacity(0.10))
                Image(systemName: device.isConnectable ? "link" : "lock")
                    .foregroundStyle(device.isConnectable ? .blue : .secondary)
            }
            .frame(width: 36, height: 36)

            VStack(alignment: .leading, spacing: 3) {
                Text(device.name)
                    .font(.body.weight(.medium))
                    .foregroundStyle(device.isConnectable ? .primary : .secondary)
                Text("RSSI \(device.rssi)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Image(systemName: "chevron.right")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.tertiary)
        }
        .contentShape(Rectangle())
        .padding(.vertical, 10)
    }
}

private struct EmptyState: View {
    let title: String
    let systemImage: String
    let detail: String

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: systemImage)
                .font(.title2)
                .foregroundStyle(.secondary)
            Text(title)
                .font(.subheadline.weight(.semibold))
            Text(detail)
                .font(.footnote)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 16)
    }
}

private struct ErrorBanner: View {
    let message: String

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
            Text(message)
                .font(.footnote)
                .foregroundStyle(.primary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(12)
        .background(Color.red.opacity(0.10), in: RoundedRectangle(cornerRadius: 8))
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.red.opacity(0.18))
        }
    }
}

private enum OpenPlaudTheme {
    static var screenBackground: Color {
        #if os(iOS)
        Color(uiColor: .systemGroupedBackground)
        #elseif os(macOS)
        Color(nsColor: .windowBackgroundColor)
        #else
        Color(.systemBackground)
        #endif
    }

    static var cardBackground: Color {
        #if os(iOS)
        Color(uiColor: .secondarySystemGroupedBackground)
        #elseif os(macOS)
        Color(nsColor: .controlBackgroundColor)
        #else
        Color(.secondarySystemBackground)
        #endif
    }

    static var subtleFill: Color {
        #if os(iOS)
        Color(uiColor: .tertiarySystemFill)
        #elseif os(macOS)
        Color(nsColor: .separatorColor).opacity(0.20)
        #else
        Color.gray.opacity(0.12)
        #endif
    }

    static var fieldBackground: Color {
        #if os(iOS)
        Color(uiColor: .systemBackground)
        #elseif os(macOS)
        Color(nsColor: .textBackgroundColor)
        #else
        Color.white
        #endif
    }

    static var cardBorder: Color {
        #if os(iOS)
        Color(uiColor: .separator).opacity(0.18)
        #elseif os(macOS)
        Color(nsColor: .separatorColor).opacity(0.24)
        #else
        Color.gray.opacity(0.20)
        #endif
    }
}

private extension OpenPlaudClipChunk.State {
    var label: String {
        switch self {
        case .data:
            return "data"
        case .end:
            return "end"
        case .error:
            return "error"
        }
    }
}

private extension OpenPlaudTransferRoute {
    var displayName: String {
        switch self {
        case .wifi:
            return "Wi-Fi"
        case .bluetooth:
            return "Bluetooth"
        }
    }
}

private extension OpenPlaudTransferCompression {
    var label: String {
        switch self {
        case .monoWav:
            return "Mono WAV"
        case .imaAdpcm:
            return "IMA ADPCM"
        case .opus:
            return "Opus"
        }
    }

    var shortLabel: String {
        switch self {
        case .monoWav:
            return "WAV"
        case .imaAdpcm:
            return "ADPCM"
        case .opus:
            return "Opus"
        }
    }
}

private extension OpenPlaudSyncState {
    var displayName: String {
        switch self {
        case .idle:
            return "Idle"
        case .scanningWiFi:
            return "Scanning Wi-Fi"
        case .syncing:
            return "Syncing"
        case .complete:
            return "Complete"
        case .failed:
            return "Failed"
        }
    }

    var iconName: String {
        switch self {
        case .idle:
            return "pause.circle"
        case .scanningWiFi:
            return "wifi"
        case .syncing:
            return "arrow.triangle.2.circlepath"
        case .complete:
            return "checkmark.circle.fill"
        case .failed:
            return "exclamationmark.triangle.fill"
        }
    }

    var tint: Color {
        switch self {
        case .idle:
            return .secondary
        case .scanningWiFi, .syncing:
            return .blue
        case .complete:
            return .green
        case .failed:
            return .red
        }
    }
}

private extension OpenPlaudDeviceStatus {
    var batteryDisplay: String {
        guard let batteryPercent else {
            return "--"
        }
        return "\(batteryPercent)%"
    }

    var batteryIconName: String {
        if isCharging == true {
            return "battery.100.bolt"
        }
        guard let batteryPercent else {
            return "battery.0percent"
        }
        if batteryPercent >= 75 {
            return "battery.100percent"
        }
        if batteryPercent >= 35 {
            return "battery.50percent"
        }
        return "battery.25percent"
    }

    var batteryTint: Color {
        if isCharging == true {
            return .green
        }
        guard let batteryPercent else {
            return .secondary
        }
        return batteryPercent <= 20 ? .red : .green
    }

    var queueDisplay: String {
        "\(queuedClips ?? 0)"
    }

    var queueTint: Color {
        guard let queuedClips, queuedClips > 0 else {
            return .blue
        }
        return .orange
    }

    var queueSummary: String {
        let count = queuedClips ?? 0
        if count == 0 {
            return "No clips waiting"
        }
        if count == 1 {
            return "1 clip waiting"
        }
        return "\(count) clips waiting"
    }
}

private extension View {
    @ViewBuilder
    func openPlaudCredentialInputStyle() -> some View {
        #if os(iOS)
        self
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
        #else
        self
        #endif
    }

    @ViewBuilder
    func openPlaudURLInputStyle() -> some View {
        #if os(iOS)
        self
            .keyboardType(.URL)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
        #else
        self
        #endif
    }

    func openPlaudFieldStyle() -> some View {
        padding(.horizontal, 12)
            .padding(.vertical, 11)
            .background(OpenPlaudTheme.fieldBackground, in: RoundedRectangle(cornerRadius: 8))
            .overlay {
                RoundedRectangle(cornerRadius: 8)
                    .stroke(OpenPlaudTheme.cardBorder)
            }
    }

    @ViewBuilder
    func openPlaudNavigationTitleMode() -> some View {
        #if os(iOS)
        self.navigationBarTitleDisplayMode(.large)
        #else
        self
        #endif
    }
}

struct OpenPlaudRootViewPreviews: PreviewProvider {
    static var previews: some View {
        OpenPlaudRootView(deviceClient: OpenPlaudDeviceClient())
    }
}

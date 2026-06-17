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
    @ObservedObject var deviceClient: OpenPlaudDeviceClient

    @State private var wifiSSID = ""
    @State private var wifiPassword = ""
    @State private var serverURL = ""
    @State private var authToken = ""
    @State private var route: OpenPlaudTransferRoute = .wifi
    @State private var compression: OpenPlaudTransferCompression = .monoWav

    var body: some View {
        NavigationStack {
            List {
                bluetoothSection
                devicesSection
                statusSection
                configurationSection
                commandSection
                logSection
            }
            .navigationTitle("Open Plaud")
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

    private var bluetoothSection: some View {
        Section("Bluetooth") {
            LabeledContent("State", value: bluetoothStateText)
            LabeledContent("Connection", value: connectionStateText)

            HStack {
                Button {
                    deviceClient.startScanning()
                } label: {
                    Label("Scan", systemImage: "dot.radiowaves.left.and.right")
                }
                .disabled(deviceClient.bluetoothState != .poweredOn)

                Spacer()

                Button(role: .destructive) {
                    deviceClient.disconnect()
                } label: {
                    Label("Disconnect", systemImage: "xmark.circle")
                }
                .disabled(!isConnected)
            }

            if let lastError = deviceClient.lastError {
                Text(lastError)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }
        }
    }

    private var devicesSection: some View {
        Section("Devices") {
            if deviceClient.discoveredDevices.isEmpty {
                ContentUnavailableView(
                    "No Open Plaud nearby",
                    systemImage: "antenna.radiowaves.left.and.right.slash"
                )
            } else {
                ForEach(deviceClient.discoveredDevices) { device in
                    Button {
                        deviceClient.connect(to: device)
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(device.name)
                                Text("RSSI \(device.rssi)")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: device.isConnectable ? "link" : "lock")
                                .foregroundStyle(.secondary)
                        }
                    }
                    .disabled(!device.isConnectable)
                }
            }
        }
    }

    private var statusSection: some View {
        Section("Status") {
            if let status = deviceClient.status {
                LabeledContent("Sync", value: status.syncState.rawValue.replacingOccurrences(of: "_", with: " "))
                if let queuedClips = status.queuedClips {
                    LabeledContent("Queued clips", value: "\(queuedClips)")
                }
                if let batteryPercent = status.batteryPercent {
                    LabeledContent("Battery", value: "\(batteryPercent)%")
                }
                if let firmwareVersion = status.firmwareVersion {
                    LabeledContent("Firmware", value: firmwareVersion)
                }
                if let wifiSSID = status.wifiSSID {
                    LabeledContent("Wi-Fi", value: wifiSSID)
                }
                if let activeClip = status.activeClip {
                    LabeledContent("Clip", value: activeClip)
                }
                if let transferPercent = status.transferPercent {
                    ProgressView(value: Double(transferPercent), total: 100) {
                        Text("Transfer")
                    } currentValueLabel: {
                        Text("\(transferPercent)%")
                    }
                }
                if let message = status.message {
                    Text(message)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                if let cursor = deviceClient.clipTransferCursor {
                    LabeledContent(
                        "BLE cursor",
                        value: "\(cursor.codec.rawValue) @ \(cursor.offset)"
                    )
                }
                if let chunk = deviceClient.lastClipChunk {
                    LabeledContent(
                        "BLE chunk",
                        value: "\(chunk.state.label), \(chunk.payloadLength) B, \(chunk.offsetEnd)/\(chunk.totalBytes)"
                    )
                }
            } else {
                ContentUnavailableView("No status yet", systemImage: "waveform.badge.magnifyingglass")
            }
        }
    }

    private var configurationSection: some View {
        Section("Configuration") {
            TextField("Wi-Fi SSID", text: $wifiSSID)
                .openPlaudCredentialInputStyle()
            SecureField("Wi-Fi password", text: $wifiPassword)
            Button {
                deviceClient.configureWiFi(ssid: wifiSSID, password: wifiPassword)
            } label: {
                Label("Send Wi-Fi", systemImage: "wifi")
            }
            .disabled(!isReady || wifiSSID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

            TextField("Server URL", text: $serverURL)
                .openPlaudURLInputStyle()
            SecureField("Auth token", text: $authToken)
            Button {
                guard let url = URL(string: serverURL) else {
                    return
                }
                deviceClient.configureServer(url: url, authToken: authToken)
            } label: {
                Label("Send Server", systemImage: "server.rack")
            }
            .disabled(!isReady || URL(string: serverURL) == nil)

            Picker("Route", selection: $route) {
                ForEach(OpenPlaudTransferRoute.allCases, id: \.self) { route in
                    Text(route.rawValue.capitalized).tag(route)
                }
            }

            Picker("Compression", selection: $compression) {
                ForEach(OpenPlaudTransferCompression.allCases, id: \.self) { compression in
                    Text(compression.label).tag(compression)
                }
            }

            Button {
                deviceClient.setTransferPreference(route: route, compression: compression)
            } label: {
                Label("Set Transfer", systemImage: "slider.horizontal.3")
            }
            .disabled(!isReady)
        }
    }

    private var commandSection: some View {
        Section("Commands") {
            Button {
                deviceClient.startWiFiSync()
            } label: {
                Label("Start Sync", systemImage: "arrow.triangle.2.circlepath")
            }
            .disabled(!isReady)

            Button(role: .destructive) {
                deviceClient.stopSync()
            } label: {
                Label("Stop Sync", systemImage: "stop.circle")
            }
            .disabled(!isReady)

            Button {
                deviceClient.prepareBluetoothClipTransfer(codec: .imaAdpcm)
            } label: {
                Label("Prepare BLE Transfer", systemImage: "point.3.connected.trianglepath.dotted")
            }
            .disabled(!isReady)

            Button {
                deviceClient.readNextClipChunk()
            } label: {
                Label("Read BLE Chunk", systemImage: "square.and.arrow.down")
            }
            .disabled(!isReady)
        }
    }

    private var logSection: some View {
        Section("Recent Commands") {
            if deviceClient.commandLog.isEmpty {
                Text("No commands sent")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(deviceClient.commandLog, id: \.self) { entry in
                    Text(entry)
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
            return "Ready"
        case .failed(let message):
            return "Failed: \(message)"
        }
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
}

struct OpenPlaudRootViewPreviews: PreviewProvider {
    static var previews: some View {
        OpenPlaudRootView(deviceClient: OpenPlaudDeviceClient())
    }
}

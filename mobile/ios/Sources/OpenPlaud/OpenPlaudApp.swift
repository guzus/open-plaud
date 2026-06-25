import OpenPlaudCompanion
import SwiftUI

@main
struct OpenPlaudApp: App {
    @StateObject private var deviceClient = OpenPlaudDeviceClient()
    @State private var isShowingStartup = true

    var body: some Scene {
        WindowGroup {
            ZStack {
                OpenPlaudRootView(deviceClient: deviceClient)
                    .opacity(isShowingStartup ? 0 : 1)
                    .allowsHitTesting(!isShowingStartup)

                if isShowingStartup {
                    OpenPlaudStartupScreen {
                        isShowingStartup = false
                    }
                    .transition(.opacity.combined(with: .scale(scale: 1.015)))
                }
            }
            .openPlaudStartupChromeHidden(isShowingStartup)
        }
    }
}

private struct OpenPlaudStartupScreen: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let onComplete: () -> Void

    @State private var isLive = false
    @State private var isExiting = false
    @State private var activeStep = 0

    private let startupSteps = ["Pair", "Record", "Sync", "Summarize"]

    var body: some View {
        ZStack {
            OpenPlaudStartupBackdrop(isLive: isLive && !reduceMotion)

            VStack(spacing: 30) {
                Spacer(minLength: 30)

                OpenPlaudStartupMark(isLive: isLive && !reduceMotion)
                    .frame(width: 222, height: 222)
                    .scaleEffect(isExiting ? 0.88 : (isLive ? 1 : 0.92))
                    .opacity(isExiting ? 0 : 1)

                VStack(spacing: 9) {
                    Text("Open Plaud")
                        .font(.system(size: 42, weight: .black, design: .rounded))
                        .foregroundStyle(.white)

                    Text("Private voice capture, ready to sync.")
                        .font(.callout.weight(.medium))
                        .foregroundStyle(.white.opacity(0.70))
                        .multilineTextAlignment(.center)
                }
                .offset(y: isLive ? 0 : 12)
                .opacity(isExiting ? 0 : (isLive ? 1 : 0))

                OpenPlaudStartupProgress(steps: startupSteps, activeStep: activeStep)
                    .frame(maxWidth: 360)
                    .opacity(isExiting ? 0 : 1)

                Spacer(minLength: 24)
            }
            .padding(.horizontal, 28)
            .padding(.vertical, 42)
        }
        .ignoresSafeArea()
        .task(id: reduceMotion) {
            await runStartupSequence()
        }
    }

    @MainActor
    private func runStartupSequence() async {
        activeStep = 0
        isExiting = false

        withAnimation(.spring(response: 0.72, dampingFraction: 0.78)) {
            isLive = true
        }

        guard !reduceMotion else {
            try? await Task.sleep(for: .milliseconds(650))
            onComplete()
            return
        }

        for index in startupSteps.indices.dropFirst() {
            try? await Task.sleep(for: .milliseconds(420))
            guard !Task.isCancelled else { return }

            withAnimation(.spring(response: 0.45, dampingFraction: 0.82)) {
                activeStep = index
            }
        }

        try? await Task.sleep(for: .milliseconds(520))
        guard !Task.isCancelled else { return }

        withAnimation(.easeInOut(duration: 0.42)) {
            isExiting = true
        }

        try? await Task.sleep(for: .milliseconds(360))
        guard !Task.isCancelled else { return }

        onComplete()
    }
}

private struct OpenPlaudStartupBackdrop: View {
    let isLive: Bool

    var body: some View {
        TimelineView(.animation) { timeline in
            let time = isLive ? timeline.date.timeIntervalSinceReferenceDate : 0

            Canvas { context, size in
                let bounds = CGRect(origin: .zero, size: size)

                context.fill(
                    Path(bounds),
                    with: .linearGradient(
                        Gradient(colors: [
                            Color(red: 0.02, green: 0.03, blue: 0.045),
                            Color(red: 0.03, green: 0.10, blue: 0.12),
                            Color(red: 0.015, green: 0.055, blue: 0.045),
                        ]),
                        startPoint: .zero,
                        endPoint: CGPoint(x: size.width, y: size.height)
                    )
                )

                drawPulseBands(in: &context, size: size, time: time)
                drawGrid(in: &context, size: size, time: time)
                drawWaveform(in: &context, size: size, time: time)
            }
        }
        .overlay {
            LinearGradient(
                colors: [
                    .black.opacity(0.14),
                    .clear,
                    .black.opacity(0.42),
                ],
                startPoint: .top,
                endPoint: .bottom
            )
        }
    }

    private func drawPulseBands(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let center = CGPoint(x: size.width * 0.5, y: size.height * 0.44)
        let maxRadius = max(size.width, size.height) * 0.72

        for index in 0..<5 {
            let progress = (time * 0.22 + Double(index) * 0.19).truncatingRemainder(dividingBy: 1)
            let radius = maxRadius * (0.18 + progress * 0.78)
            let alpha = max(0, 0.24 - progress * 0.22)
            let rect = CGRect(
                x: center.x - radius,
                y: center.y - radius,
                width: radius * 2,
                height: radius * 2
            )
            let color = index.isMultiple(of: 2)
                ? Color(red: 0.15, green: 0.88, blue: 0.78)
                : Color(red: 1.0, green: 0.63, blue: 0.18)

            context.stroke(
                Path(ellipseIn: rect),
                with: .color(color.opacity(alpha)),
                lineWidth: 1.15
            )
        }
    }

    private func drawGrid(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let spacing: CGFloat = 34
        let offset = CGFloat(time.truncatingRemainder(dividingBy: 1)) * spacing
        let color = Color.white.opacity(0.045)

        var vertical = Path()
        var x = -spacing + offset
        while x < size.width + spacing {
            vertical.move(to: CGPoint(x: x, y: 0))
            vertical.addLine(to: CGPoint(x: x, y: size.height))
            x += spacing
        }

        var horizontal = Path()
        var y = -spacing + offset * 0.55
        while y < size.height + spacing {
            horizontal.move(to: CGPoint(x: 0, y: y))
            horizontal.addLine(to: CGPoint(x: size.width, y: y))
            y += spacing
        }

        context.stroke(vertical, with: .color(color), lineWidth: 0.8)
        context.stroke(horizontal, with: .color(color), lineWidth: 0.8)
    }

    private func drawWaveform(in context: inout GraphicsContext, size: CGSize, time: TimeInterval) {
        let midY = size.height * 0.62
        let amplitude = min(size.width, size.height) * 0.035
        let phase = CGFloat(time * 2.3)

        for layer in 0..<3 {
            var path = Path()
            let layerOffset = CGFloat(layer) * 0.95
            let color: Color

            switch layer {
            case 0:
                color = Color(red: 0.16, green: 0.88, blue: 0.78).opacity(0.30)
            case 1:
                color = Color(red: 0.20, green: 0.53, blue: 1.0).opacity(0.22)
            default:
                color = Color(red: 1.0, green: 0.61, blue: 0.17).opacity(0.18)
            }

            var x: CGFloat = 0
            while x <= size.width {
                let normalized = x / max(size.width, 1)
                let y = midY
                    + sin(normalized * .pi * 5.6 + phase + layerOffset) * amplitude
                    + sin(normalized * .pi * 13.0 - phase * 0.7) * amplitude * 0.34

                if x == 0 {
                    path.move(to: CGPoint(x: x, y: y))
                } else {
                    path.addLine(to: CGPoint(x: x, y: y))
                }
                x += 5
            }

            context.stroke(path, with: .color(color), lineWidth: layer == 0 ? 2.0 : 1.2)
        }
    }
}

private struct OpenPlaudStartupMark: View {
    let isLive: Bool

    var body: some View {
        TimelineView(.animation) { timeline in
            let time = isLive ? timeline.date.timeIntervalSinceReferenceDate : 0
            let scannerOffset = CGFloat(sin(time * 2.1)) * 64
            let rotation = Angle.degrees(time * 18)

            ZStack {
                ForEach(0..<3) { index in
                    RoundedRectangle(cornerRadius: 38, style: .continuous)
                        .stroke(
                            ringGradient(index: index),
                            lineWidth: index == 0 ? 2.4 : 1.1
                        )
                        .frame(width: CGFloat(176 + index * 32), height: CGFloat(176 + index * 32))
                        .rotationEffect(rotation + .degrees(Double(index) * 24))
                        .opacity(0.62 - Double(index) * 0.16)
                }

                RoundedRectangle(cornerRadius: 36, style: .continuous)
                    .fill(
                        LinearGradient(
                            colors: [
                                Color.white.opacity(0.18),
                                Color.white.opacity(0.06),
                            ],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        )
                    )
                    .frame(width: 112, height: 154)
                    .overlay {
                        RoundedRectangle(cornerRadius: 36, style: .continuous)
                            .stroke(Color.white.opacity(0.32), lineWidth: 1)
                    }
                    .shadow(color: Color(red: 0.16, green: 0.88, blue: 0.78).opacity(0.24), radius: 28, x: 0, y: 12)

                VStack(spacing: 16) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(Color(red: 0.12, green: 0.92, blue: 0.57))
                            .frame(width: 8, height: 8)
                        Text("REC")
                            .font(.caption2.weight(.black))
                            .foregroundStyle(.white.opacity(0.78))
                    }

                    OpenPlaudStartupBars(time: time)
                        .frame(width: 68, height: 54)

                    Image(systemName: "antenna.radiowaves.left.and.right")
                        .font(.system(size: 26, weight: .semibold))
                        .symbolRenderingMode(.hierarchical)
                        .foregroundStyle(.white.opacity(0.78))
                }

                Capsule()
                    .fill(
                        LinearGradient(
                            colors: [
                                .clear,
                                Color.white.opacity(0.95),
                                Color(red: 0.16, green: 0.88, blue: 0.78).opacity(0.88),
                                .clear,
                            ],
                            startPoint: .leading,
                            endPoint: .trailing
                        )
                    )
                    .frame(width: 156, height: 3)
                    .offset(y: scannerOffset)
                    .blur(radius: 0.5)
                    .opacity(isLive ? 0.88 : 0)
            }
        }
    }

    private func ringGradient(index: Int) -> AngularGradient {
        AngularGradient(
            colors: [
                Color(red: 0.13, green: 0.85, blue: 0.72),
                Color(red: 0.22, green: 0.48, blue: 1.0),
                Color(red: 1.0, green: 0.59, blue: 0.17),
                Color(red: 0.13, green: 0.85, blue: 0.72),
            ],
            center: .center,
            angle: .degrees(Double(index) * 18)
        )
    }
}

private struct OpenPlaudStartupBars: View {
    let time: TimeInterval

    var body: some View {
        HStack(alignment: .center, spacing: 5) {
            ForEach(0..<9) { index in
                Capsule()
                    .fill(barColor(index: index))
                    .frame(width: 4, height: barHeight(index: index))
                    .shadow(color: barColor(index: index).opacity(0.32), radius: 5, x: 0, y: 0)
            }
        }
    }

    private func barHeight(index: Int) -> CGFloat {
        let wave = sin(time * 4.8 + Double(index) * 0.72)
        let counterWave = cos(time * 2.8 - Double(index) * 0.36)
        return 12 + CGFloat((wave + counterWave + 2) / 4) * 42
    }

    private func barColor(index: Int) -> Color {
        if index < 3 {
            return Color(red: 0.14, green: 0.88, blue: 0.72)
        }
        if index < 6 {
            return Color(red: 0.25, green: 0.58, blue: 1.0)
        }
        return Color(red: 1.0, green: 0.63, blue: 0.18)
    }
}

private struct OpenPlaudStartupProgress: View {
    let steps: [String]
    let activeStep: Int

    var body: some View {
        HStack(spacing: 10) {
            ForEach(Array(steps.enumerated()), id: \.offset) { index, step in
                VStack(spacing: 7) {
                    ZStack {
                        Circle()
                            .fill(index <= activeStep ? stepColor(index: index).opacity(0.24) : Color.white.opacity(0.08))
                        Image(systemName: stepIcon(index: index))
                            .font(.system(size: 13, weight: .bold))
                            .foregroundStyle(index <= activeStep ? stepColor(index: index) : Color.white.opacity(0.36))
                    }
                    .frame(width: 34, height: 34)
                    .scaleEffect(index == activeStep ? 1.12 : 1)

                    Text(step.uppercased())
                        .font(.system(size: 9, weight: .bold))
                        .foregroundStyle(index <= activeStep ? .white.opacity(0.82) : .white.opacity(0.34))
                        .lineLimit(1)
                        .minimumScaleFactor(0.72)
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(.horizontal, 4)
    }

    private func stepIcon(index: Int) -> String {
        switch index {
        case 0:
            return "dot.radiowaves.left.and.right"
        case 1:
            return "waveform"
        case 2:
            return "arrow.triangle.2.circlepath"
        default:
            return "sparkles"
        }
    }

    private func stepColor(index: Int) -> Color {
        switch index {
        case 0:
            return Color(red: 0.18, green: 0.82, blue: 1.0)
        case 1:
            return Color(red: 0.12, green: 0.92, blue: 0.57)
        case 2:
            return Color(red: 1.0, green: 0.62, blue: 0.20)
        default:
            return Color(red: 0.74, green: 0.63, blue: 1.0)
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
    @State private var compression: OpenPlaudTransferCompression = .opus

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
                    deviceClient.setTransferPreference(route: .wifi, compression: .opus)
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

    @ViewBuilder
    func openPlaudStartupChromeHidden(_ isHidden: Bool) -> some View {
        #if os(iOS)
        self.statusBarHidden(isHidden)
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

import Combine
@preconcurrency import CoreBluetooth
import Foundation

public enum OpenPlaudBluetoothState: Equatable, Sendable {
    case unknown
    case unsupported
    case unauthorized
    case poweredOff
    case poweredOn

    init(_ state: CBManagerState) {
        switch state {
        case .unknown, .resetting:
            self = .unknown
        case .unsupported:
            self = .unsupported
        case .unauthorized:
            self = .unauthorized
        case .poweredOff:
            self = .poweredOff
        case .poweredOn:
            self = .poweredOn
        @unknown default:
            self = .unknown
        }
    }
}

public enum OpenPlaudConnectionState: Equatable, Sendable {
    case disconnected
    case scanning
    case connecting(UUID)
    case discovering(UUID)
    case ready(UUID)
    case failed(String)
}

@MainActor
public final class OpenPlaudDeviceClient: NSObject, ObservableObject {
    @Published public private(set) var bluetoothState: OpenPlaudBluetoothState = .unknown
    @Published public private(set) var connectionState: OpenPlaudConnectionState = .disconnected
    @Published public private(set) var discoveredDevices: [OpenPlaudDiscoveredDevice] = []
    @Published public private(set) var status: OpenPlaudDeviceStatus?
    @Published public private(set) var clipTransferCursor: OpenPlaudClipTransferCursor?
    @Published public private(set) var lastClipChunk: OpenPlaudClipChunk?
    @Published public private(set) var lastError: String?
    @Published public private(set) var commandLog: [String] = []

    private lazy var centralManager = CBCentralManager(delegate: self, queue: .main)
    private var peripheralsByID: [UUID: CBPeripheral] = [:]
    private var connectedPeripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    private var clipManifestCharacteristic: CBCharacteristic?
    private var clipChunkCharacteristic: CBCharacteristic?

    public override init() {
        super.init()
        _ = centralManager
    }

    public func startScanning() {
        guard centralManager.state == .poweredOn else {
            lastError = "Bluetooth is not powered on."
            bluetoothState = OpenPlaudBluetoothState(centralManager.state)
            return
        }

        discoveredDevices.removeAll()
        lastError = nil
        connectionState = .scanning
        centralManager.scanForPeripherals(
            withServices: [OpenPlaudBLEProfile.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    public func stopScanning() {
        centralManager.stopScan()
        if case .scanning = connectionState {
            connectionState = .disconnected
        }
    }

    public func connect(to device: OpenPlaudDiscoveredDevice) {
        guard let peripheral = peripheralsByID[device.id] else {
            lastError = "Device is no longer available. Scan again."
            return
        }

        centralManager.stopScan()
        connectionState = .connecting(device.id)
        connectedPeripheral = peripheral
        peripheral.delegate = self
        centralManager.connect(peripheral)
    }

    public func disconnect() {
        guard let peripheral = connectedPeripheral else {
            connectionState = .disconnected
            return
        }

        centralManager.cancelPeripheralConnection(peripheral)
    }

    public func requestStatus() {
        send(.requestStatus)
    }

    public func startWiFiSync() {
        send(.startWiFiSync)
    }

    public func stopSync() {
        send(.stopSync)
    }

    public func configureWiFi(ssid: String, password: String) {
        send(.configureWiFi(ssid: ssid, password: password))
    }

    public func configureServer(url: URL, authToken: String?) {
        send(.configureServer(url: url, authToken: authToken?.nilIfBlank))
    }

    public func setTransferPreference(route: OpenPlaudTransferRoute, compression: OpenPlaudTransferCompression) {
        send(.setTransferPreference(OpenPlaudTransferPreference(route: route, compression: compression)))
    }

    public func prepareBluetoothClipTransfer(codec: OpenPlaudClipTransferCodec = .imaAdpcm) {
        guard let peripheral = connectedPeripheral, let characteristic = clipManifestCharacteristic else {
            lastError = "Open Plaud transfer control characteristic is not ready."
            return
        }
        do {
            let payload = try JSONEncoder().encode(OpenPlaudClipTransferCursor(path: nil, offset: 0, codec: codec))
            peripheral.writeValue(payload, for: characteristic, type: .withResponse)
            peripheral.readValue(for: characteristic)
            commandLog.insert("Prepared Bluetooth \(codec.rawValue) transfer", at: 0)
            commandLog = Array(commandLog.prefix(12))
        } catch {
            lastError = "Could not encode transfer cursor: \(error.localizedDescription)"
        }
    }

    public func readNextClipChunk() {
        guard let peripheral = connectedPeripheral, let characteristic = clipChunkCharacteristic else {
            lastError = "Open Plaud clip chunk characteristic is not ready."
            return
        }
        peripheral.readValue(for: characteristic)
    }

    private func send(_ command: OpenPlaudCommand) {
        guard let peripheral = connectedPeripheral, let characteristic = commandCharacteristic else {
            lastError = "Open Plaud command characteristic is not ready."
            return
        }

        do {
            let payload = try command.encodedPayload()
            let writeType: CBCharacteristicWriteType = characteristic.properties.contains(.write)
                ? .withResponse
                : .withoutResponse
            peripheral.writeValue(payload, for: characteristic, type: writeType)
            commandLog.insert(command.auditName, at: 0)
            commandLog = Array(commandLog.prefix(12))
        } catch {
            lastError = "Could not encode BLE command: \(error.localizedDescription)"
        }
    }

    private func updateDiscoveredDevice(_ device: OpenPlaudDiscoveredDevice, peripheral: CBPeripheral) {
        peripheralsByID[device.id] = peripheral
        if let index = discoveredDevices.firstIndex(where: { $0.id == device.id }) {
            discoveredDevices[index] = device
        } else {
            discoveredDevices.append(device)
        }
        discoveredDevices.sort { lhs, rhs in
            if lhs.rssi == rhs.rssi {
                return lhs.name < rhs.name
            }
            return lhs.rssi > rhs.rssi
        }
    }

    private func resetConnection() {
        connectedPeripheral = nil
        commandCharacteristic = nil
        statusCharacteristic = nil
        clipManifestCharacteristic = nil
        clipChunkCharacteristic = nil
        clipTransferCursor = nil
        lastClipChunk = nil
        connectionState = .disconnected
    }
}

extension OpenPlaudDeviceClient: CBCentralManagerDelegate {
    public nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated {
            bluetoothState = OpenPlaudBluetoothState(central.state)
            if central.state != .poweredOn {
                resetConnection()
            }
        }
    }

    public nonisolated func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let advertisedServices = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]
        let hasOpenPlaudService = advertisedServices?.contains(CBUUID(
            string: OpenPlaudBLEProfile.serviceUUIDString
        )) == true
        let isConnectable = (advertisementData[CBAdvertisementDataIsConnectable] as? NSNumber)?.boolValue ?? true
        let rssi = RSSI.intValue

        MainActor.assumeIsolated {
            let name = localName ?? peripheral.name ?? "Open Plaud"
            guard hasOpenPlaudService else {
                return
            }

            updateDiscoveredDevice(
                OpenPlaudDiscoveredDevice(
                    id: peripheral.identifier,
                    name: name,
                    rssi: rssi,
                    isConnectable: isConnectable
                ),
                peripheral: peripheral
            )
        }
    }

    public nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated {
            connectionState = .discovering(peripheral.identifier)
            peripheral.discoverServices([OpenPlaudBLEProfile.serviceUUID])
        }
    }

    public nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        MainActor.assumeIsolated {
            lastError = error?.localizedDescription ?? "Could not connect to Open Plaud."
            connectionState = .failed(lastError ?? "Connection failed")
            resetConnection()
        }
    }

    public nonisolated func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        MainActor.assumeIsolated {
            if let error {
                lastError = error.localizedDescription
            }
            resetConnection()
        }
    }
}

extension OpenPlaudDeviceClient: CBPeripheralDelegate {
    public nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated {
            if let error {
                lastError = error.localizedDescription
                connectionState = .failed(error.localizedDescription)
                return
            }

            guard let service = peripheral.services?.first(where: { $0.uuid == OpenPlaudBLEProfile.serviceUUID }) else {
                lastError = "Open Plaud BLE service was not found."
                connectionState = .failed(lastError ?? "Missing service")
                return
            }

            peripheral.discoverCharacteristics(
                [
                    OpenPlaudBLEProfile.commandCharacteristicUUID,
                    OpenPlaudBLEProfile.statusCharacteristicUUID,
                    OpenPlaudBLEProfile.clipManifestCharacteristicUUID,
                    OpenPlaudBLEProfile.clipChunkCharacteristicUUID,
                ],
                for: service
            )
        }
    }

    public nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        MainActor.assumeIsolated {
            if let error {
                lastError = error.localizedDescription
                connectionState = .failed(error.localizedDescription)
                return
            }

            for characteristic in service.characteristics ?? [] {
                switch characteristic.uuid {
                case OpenPlaudBLEProfile.commandCharacteristicUUID:
                    commandCharacteristic = characteristic
                case OpenPlaudBLEProfile.statusCharacteristicUUID:
                    statusCharacteristic = characteristic
                    peripheral.setNotifyValue(true, for: characteristic)
                    peripheral.readValue(for: characteristic)
                case OpenPlaudBLEProfile.clipManifestCharacteristicUUID:
                    clipManifestCharacteristic = characteristic
                    peripheral.readValue(for: characteristic)
                case OpenPlaudBLEProfile.clipChunkCharacteristicUUID:
                    clipChunkCharacteristic = characteristic
                default:
                    continue
                }
            }

            guard commandCharacteristic != nil else {
                lastError = "Open Plaud command characteristic was not found."
                connectionState = .failed(lastError ?? "Missing command characteristic")
                return
            }

            connectionState = .ready(peripheral.identifier)
            requestStatus()
        }
    }

    public nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        MainActor.assumeIsolated {
            if let error {
                lastError = error.localizedDescription
                return
            }

            guard let data = characteristic.value else {
                return
            }

            switch characteristic.uuid {
            case OpenPlaudBLEProfile.statusCharacteristicUUID:
                do {
                    status = try JSONDecoder().decode(OpenPlaudDeviceStatus.self, from: data)
                } catch {
                    lastError = "Could not decode device status: \(error.localizedDescription)"
                }
            case OpenPlaudBLEProfile.clipManifestCharacteristicUUID:
                do {
                    clipTransferCursor = try JSONDecoder().decode(OpenPlaudClipTransferCursor.self, from: data)
                } catch {
                    lastError = "Could not decode transfer cursor: \(error.localizedDescription)"
                }
            case OpenPlaudBLEProfile.clipChunkCharacteristicUUID:
                do {
                    lastClipChunk = try OpenPlaudClipChunk(data: data)
                } catch {
                    lastError = "Could not decode clip chunk: \(error.localizedDescription)"
                }
            default:
                return
            }
        }
    }
}

private extension OpenPlaudCommand {
    var auditName: String {
        switch self {
        case .requestStatus:
            return "Requested status"
        case .startWiFiSync:
            return "Started Wi-Fi sync"
        case .stopSync:
            return "Stopped sync"
        case .configureWiFi:
            return "Configured Wi-Fi"
        case .configureServer:
            return "Configured server"
        case .setTransferPreference(let preference):
            return "Set \(preference.route.rawValue) \(preference.compression.rawValue)"
        }
    }
}

private extension String {
    var nilIfBlank: String? {
        let trimmed = trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

@preconcurrency import CoreBluetooth
import Foundation

public enum OpenPlaudBLEProfile {
    public static let serviceUUIDString = "6F504C41-5544-0001-9000-000000000001"
    public static let commandCharacteristicUUIDString = "6F504C41-5544-0002-9000-000000000001"
    public static let statusCharacteristicUUIDString = "6F504C41-5544-0003-9000-000000000001"
    public static let clipManifestCharacteristicUUIDString = "6F504C41-5544-0004-9000-000000000001"
    public static let clipChunkCharacteristicUUIDString = "6F504C41-5544-0005-9000-000000000001"

    public static var serviceUUID: CBUUID { CBUUID(string: serviceUUIDString) }
    public static var commandCharacteristicUUID: CBUUID { CBUUID(string: commandCharacteristicUUIDString) }
    public static var statusCharacteristicUUID: CBUUID { CBUUID(string: statusCharacteristicUUIDString) }
    public static var clipManifestCharacteristicUUID: CBUUID { CBUUID(string: clipManifestCharacteristicUUIDString) }
    public static var clipChunkCharacteristicUUID: CBUUID { CBUUID(string: clipChunkCharacteristicUUIDString) }

    public static let advertisedNamePrefix = "Open Plaud"
    public static let commandEncoding = "utf-8 JSON"
}

public enum OpenPlaudTransferRoute: String, Codable, CaseIterable, Equatable, Sendable {
    case wifi
    case bluetooth
}

public enum OpenPlaudTransferCompression: String, Codable, CaseIterable, Equatable, Sendable {
    case monoWav = "mono_wav"
    case imaAdpcm = "ima_adpcm"
    case opus = "opus"
}

public struct OpenPlaudTransferPreference: Codable, Equatable, Sendable {
    public let route: OpenPlaudTransferRoute
    public let compression: OpenPlaudTransferCompression

    public init(route: OpenPlaudTransferRoute, compression: OpenPlaudTransferCompression) {
        self.route = route
        self.compression = compression
    }
}

public enum OpenPlaudSyncState: String, Codable, CaseIterable, Equatable, Sendable {
    case idle
    case scanningWiFi = "scanning_wifi"
    case syncing
    case complete
    case failed
}

public struct OpenPlaudDeviceStatus: Codable, Equatable, Sendable {
    public let batteryPercent: Int?
    public let isCharging: Bool?
    public let queuedClips: Int?
    public let firmwareVersion: String?
    public let wifiSSID: String?
    public let syncState: OpenPlaudSyncState
    public let activeClip: String?
    public let transferPercent: Int?
    public let transferPreference: OpenPlaudTransferPreference?
    public let message: String?

    public init(
        batteryPercent: Int? = nil,
        isCharging: Bool? = nil,
        queuedClips: Int? = nil,
        firmwareVersion: String? = nil,
        wifiSSID: String? = nil,
        syncState: OpenPlaudSyncState = .idle,
        activeClip: String? = nil,
        transferPercent: Int? = nil,
        transferPreference: OpenPlaudTransferPreference? = nil,
        message: String? = nil
    ) {
        self.batteryPercent = batteryPercent
        self.isCharging = isCharging
        self.queuedClips = queuedClips
        self.firmwareVersion = firmwareVersion
        self.wifiSSID = wifiSSID
        self.syncState = syncState
        self.activeClip = activeClip
        self.transferPercent = transferPercent
        self.transferPreference = transferPreference
        self.message = message
    }
}

public struct OpenPlaudDiscoveredDevice: Identifiable, Equatable, Sendable {
    public let id: UUID
    public let name: String
    public let rssi: Int
    public let isConnectable: Bool

    public init(id: UUID, name: String, rssi: Int, isConnectable: Bool) {
        self.id = id
        self.name = name
        self.rssi = rssi
        self.isConnectable = isConnectable
    }
}

public enum OpenPlaudClipTransferCodec: String, Codable, CaseIterable, Equatable, Sendable {
    case raw
    case imaAdpcm = "ima-adpcm"
}

public struct OpenPlaudClipTransferCursor: Codable, Equatable, Sendable {
    public let path: String?
    public let offset: UInt32
    public let codec: OpenPlaudClipTransferCodec
}

public struct OpenPlaudClipChunk: Equatable, Sendable {
    public enum State: UInt8, Equatable, Sendable {
        case data = 0
        case end = 1
        case error = 2
    }

    public let state: State
    public let codec: OpenPlaudClipTransferCodec
    public let payloadLength: UInt16
    public let offsetStart: UInt32
    public let offsetEnd: UInt32
    public let totalBytes: UInt32
    public let payload: Data

    public init(data: Data) throws {
        guard data.count >= 16 else {
            throw DecodingError.dataCorrupted(.init(
                codingPath: [],
                debugDescription: "BLE clip chunk is shorter than its 16-byte header."
            ))
        }
        state = State(rawValue: data[0]) ?? .error
        codec = data[1] == 1 ? .imaAdpcm : .raw
        payloadLength = data.openPlaudLE16(at: 2)
        offsetStart = data.openPlaudLE32(at: 4)
        offsetEnd = data.openPlaudLE32(at: 8)
        totalBytes = data.openPlaudLE32(at: 12)
        let expectedEnd = 16 + Int(payloadLength)
        guard data.count >= expectedEnd else {
            throw DecodingError.dataCorrupted(.init(
                codingPath: [],
                debugDescription: "BLE clip chunk payload is truncated."
            ))
        }
        payload = data.subdata(in: 16..<expectedEnd)
    }
}

private extension Data {
    func openPlaudLE16(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func openPlaudLE32(at offset: Int) -> UInt32 {
        UInt32(self[offset]) |
            (UInt32(self[offset + 1]) << 8) |
            (UInt32(self[offset + 2]) << 16) |
            (UInt32(self[offset + 3]) << 24)
    }
}

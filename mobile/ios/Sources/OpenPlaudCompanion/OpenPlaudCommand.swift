import Foundation

public enum OpenPlaudCommand: Equatable, Sendable {
    case requestStatus
    case startWiFiSync
    case stopSync
    case configureWiFi(ssid: String, password: String)
    case configureServer(url: URL, authToken: String?)
    case setTransferPreference(OpenPlaudTransferPreference)
}

extension OpenPlaudCommand: Codable {
    private enum CodingKeys: String, CodingKey {
        case op
        case ssid
        case password
        case url
        case authToken = "auth_token"
        case route
        case compression
    }

    private enum Operation: String, Codable {
        case requestStatus = "request_status"
        case startWiFiSync = "start_wifi_sync"
        case stopSync = "stop_sync"
        case configureWiFi = "configure_wifi"
        case configureServer = "configure_server"
        case setTransferPreference = "set_transfer_preference"
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        switch try container.decode(Operation.self, forKey: .op) {
        case .requestStatus:
            self = .requestStatus
        case .startWiFiSync:
            self = .startWiFiSync
        case .stopSync:
            self = .stopSync
        case .configureWiFi:
            self = .configureWiFi(
                ssid: try container.decode(String.self, forKey: .ssid),
                password: try container.decode(String.self, forKey: .password)
            )
        case .configureServer:
            self = .configureServer(
                url: try container.decode(URL.self, forKey: .url),
                authToken: try container.decodeIfPresent(String.self, forKey: .authToken)
            )
        case .setTransferPreference:
            self = .setTransferPreference(OpenPlaudTransferPreference(
                route: try container.decode(OpenPlaudTransferRoute.self, forKey: .route),
                compression: try container.decode(OpenPlaudTransferCompression.self, forKey: .compression)
            ))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .requestStatus:
            try container.encode(Operation.requestStatus, forKey: .op)
        case .startWiFiSync:
            try container.encode(Operation.startWiFiSync, forKey: .op)
        case .stopSync:
            try container.encode(Operation.stopSync, forKey: .op)
        case .configureWiFi(let ssid, let password):
            try container.encode(Operation.configureWiFi, forKey: .op)
            try container.encode(ssid, forKey: .ssid)
            try container.encode(password, forKey: .password)
        case .configureServer(let url, let authToken):
            try container.encode(Operation.configureServer, forKey: .op)
            try container.encode(url, forKey: .url)
            try container.encodeIfPresent(authToken, forKey: .authToken)
        case .setTransferPreference(let preference):
            try container.encode(Operation.setTransferPreference, forKey: .op)
            try container.encode(preference.route, forKey: .route)
            try container.encode(preference.compression, forKey: .compression)
        }
    }

    public func encodedPayload() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(self)
    }
}

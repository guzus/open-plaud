import Foundation
import OpenPlaudCompanion

@main
enum VerifyOpenPlaudCompanion {
    static func main() throws {
        let wifiPayload = try OpenPlaudCommand.configureWiFi(
            ssid: "RuntimeSSID",
            password: "RuntimePassword"
        ).encodedPayload()
        let wifiJSON = String(decoding: wifiPayload, as: UTF8.self)
        precondition(wifiJSON.contains(#""op":"configure_wifi""#))
        precondition(wifiJSON.contains(#""ssid":"RuntimeSSID""#))
        precondition(wifiJSON.contains(#""password":"RuntimePassword""#))

        let transferCommand = OpenPlaudCommand.setTransferPreference(OpenPlaudTransferPreference(
            route: .bluetooth,
            compression: .opus
        ))
        let transferPayload = try transferCommand.encodedPayload()
        let decodedTransferCommand = try JSONDecoder().decode(OpenPlaudCommand.self, from: transferPayload)
        precondition(decodedTransferCommand == transferCommand)

        let statusPayload = Data("""
        {
          "syncState": "syncing",
          "queuedClips": 8,
          "transferPercent": 42
        }
        """.utf8)
        let status = try JSONDecoder().decode(OpenPlaudDeviceStatus.self, from: statusPayload)
        precondition(status.syncState == .syncing)
        precondition(status.queuedClips == 8)
        precondition(status.transferPercent == 42)
        precondition(status.batteryPercent == nil)

        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let sourcesRoot = packageRoot.appendingPathComponent("Sources")
        let enumerator = FileManager.default.enumerator(
            at: sourcesRoot,
            includingPropertiesForKeys: nil
        )

        var source = ""
        while let fileURL = enumerator?.nextObject() as? URL {
            guard fileURL.pathExtension == "swift" else {
                continue
            }
            source += try String(contentsOf: fileURL)
        }

        if let forbidden = ProcessInfo.processInfo.environment["OPEN_PLAUD_FORBIDDEN_SOURCE_TEXT"],
           !forbidden.isEmpty {
            precondition(!source.contains(forbidden))
        }

        print("open-plaud companion verifier passed")
    }
}

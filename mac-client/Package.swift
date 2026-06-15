// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "OpenPlaud",
    platforms: [
        // macOS 13+ so SwiftUI / AppKit symbols resolve in the headless `swift build`.
        .macOS(.v13)
    ],
    products: [
        .library(name: "OpenPlaudCore", targets: ["OpenPlaudCore"]),
        .executable(name: "openplaud", targets: ["OpenPlaudCLI"]),
        .executable(name: "OpenPlaudApp", targets: ["OpenPlaudApp"]),
    ],
    targets: [
        // Pure, testable logic: scanning, WAV parsing, multipart, upload, retry, state.
        // No SwiftUI/AppKit here so it stays unit-testable and headless-friendly.
        .target(
            name: "OpenPlaudCore",
            path: "Sources/OpenPlaudCore"
        ),
        // Headless CLI for verification (scan + sync from flags/env, no Keychain/UserDefaults).
        .executableTarget(
            name: "OpenPlaudCLI",
            dependencies: ["OpenPlaudCore"],
            path: "Sources/OpenPlaudCLI"
        ),
        // SwiftUI app: UI + wiring only. Keychain (token) + UserDefaults (URL) live here.
        .executableTarget(
            name: "OpenPlaudApp",
            dependencies: ["OpenPlaudCore"],
            path: "Sources/OpenPlaudApp"
        ),
        // NOTE: No XCTest target. The target environment is macOS Command Line
        // Tools (no full Xcode), which ships `swift build` but NOT the XCTest /
        // swift-testing modules — so `swift test` cannot run here. The unit
        // assertions live in `OpenPlaudCore/SelfTest.swift` and run headlessly
        // via `swift run openplaud selftest` (exits non-zero on failure).
        // With Xcode installed, a standard .testTarget can be re-added trivially.
    ]
)

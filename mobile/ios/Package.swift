// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "OpenPlaudMobileTranscription",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
    ],
    products: [
        .library(
            name: "OpenPlaudMobileTranscription",
            targets: ["OpenPlaudMobileTranscription"]
        ),
        .executable(
            name: "open-plaud-mobile-verify",
            targets: ["OpenPlaudMobileTranscriptionVerifier"]
        ),
    ],
    targets: [
        .target(name: "OpenPlaudMobileTranscription"),
        .executableTarget(
            name: "OpenPlaudMobileTranscriptionVerifier",
            dependencies: ["OpenPlaudMobileTranscription"],
            path: "Verification"
        ),
    ]
)

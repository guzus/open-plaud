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
        .library(
            name: "OpenPlaudCompanion",
            targets: ["OpenPlaudCompanion"]
        ),
        .executable(
            name: "OpenPlaud",
            targets: ["OpenPlaud"]
        ),
        .executable(
            name: "open-plaud-companion-verify",
            targets: ["OpenPlaudCompanionVerifier"]
        ),
        .executable(
            name: "open-plaud-mobile-verify",
            targets: ["OpenPlaudMobileTranscriptionVerifier"]
        ),
    ],
    targets: [
        .target(name: "OpenPlaudMobileTranscription"),
        .target(name: "OpenPlaudCompanion"),
        .executableTarget(
            name: "OpenPlaud",
            dependencies: ["OpenPlaudCompanion", "OpenPlaudMobileTranscription"]
        ),
        .executableTarget(
            name: "OpenPlaudMobileTranscriptionVerifier",
            dependencies: ["OpenPlaudMobileTranscription"],
            path: "Verification"
        ),
        .executableTarget(
            name: "OpenPlaudCompanionVerifier",
            dependencies: ["OpenPlaudCompanion"],
            path: "CompanionVerification"
        ),
    ]
)

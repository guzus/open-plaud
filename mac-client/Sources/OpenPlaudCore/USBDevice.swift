import Foundation

/// Pure, Foundation-only model + interpretation of a USB device that *might* be
/// the open-plaud recorder.
///
/// Lives in OpenPlaudCore (no IOKit, no AppKit) so the VID/PID interpretation is
/// unit-testable and runs headlessly under `swift build` / `openplaud selftest`.
/// The App layer wires IOKit USB matching to deliver these; this file never
/// touches IOKit — it only answers "given a VID/PID/name, is this our device and
/// in what mode?".
///
/// First-principles note on identity: VID `0x303A` is **Espressif's** generic
/// vendor ID — every ESP32-S3 board shares it, so a VID match means "an Espressif
/// device", not "definitely an open-plaud". We label honestly ("open-plaud device
/// (ESP32)") because in this product's context it *is* the user's recorder, but
/// if the USB product string is literally "open-plaud" we trust that instead.

/// The USB Vendor ID the open-plaud recorder (Waveshare ESP32-S3) enumerates
/// under. Generic to all Espressif USB devices — see the note above.
public let espressifVendorID: Int = 0x303A

/// What a connected open-plaud USB device can do *right now*, inferred from its
/// USB Product ID. All PIDs below are under VID `0x303A`.
public enum USBDeviceMode: String, Sendable, Equatable {
    /// USB-Serial/JTAG (PID `0x1001`) — the recorder firmware as it ships today.
    /// The Mac sees a serial port, NOT a drive: clips are **not** readable over
    /// the cable yet (sync still needs the microSD or Wi-Fi).
    case recorder

    /// TinyUSB MSC (`0x4002`) or MSC+CDC (`0x4003`) — a future "drive mode" where
    /// the device exposes its storage as a USB Mass-Storage volume. In that mode
    /// the SD mounts and the existing content-based volume detection takes over.
    case drive

    /// A VID-`0x303A` Espressif device whose PID we don't specifically recognize.
    /// Still very likely the user's device; treated as present-but-mode-unknown.
    case unknown

    /// Human-readable, honest label for the UI / CLI.
    public var label: String {
        switch self {
        case .recorder: return "recorder mode (USB-Serial)"
        case .drive:    return "drive mode (USB Mass Storage)"
        case .unknown:  return "connected (mode unknown)"
        }
    }

    /// Whether this mode exposes a mountable volume. Today only `drive` does; the
    /// recorder's serial mode does not. (This is about the *cable* — clips may
    /// still be syncable from a separately-inserted microSD; that's decided by
    /// volume detection, not here.)
    public var exposesVolume: Bool { self == .drive }
}

/// A USB device the app cares about: an Espressif (VID `0x303A`) device, with its
/// PID-derived mode and best-effort product string. Equatable so the watcher can
/// suppress no-op republishes.
public struct USBDevice: Identifiable, Sendable, Equatable {
    public let vendorID: Int
    public let productID: Int
    /// USB "Product Name" string from the device descriptor, if it provided one.
    public let productName: String?

    /// Stable identity for de-duping the live set. VID+PID+name is enough here;
    /// we don't read per-unit serials (and wouldn't want to log them).
    public var id: String { "\(vendorID):\(productID):\(productName ?? "")" }

    public init(vendorID: Int, productID: Int, productName: String?) {
        self.vendorID = vendorID
        self.productID = productID
        self.productName = productName
    }

    /// True when this is an Espressif-vendor device (i.e. our recorder's VID).
    public var isEspressif: Bool { vendorID == espressifVendorID }

    /// Mode inferred from the PID. See `USBInterpreter.mode(forProductID:)`.
    public var mode: USBDeviceMode { USBInterpreter.mode(forProductID: productID) }

    /// Honest display name: prefer a literal "open-plaud" product string, else a
    /// generic ESP32 label (since VID `0x303A` is shared across all Espressif
    /// boards). Never asserts more identity than the descriptor actually gives.
    public var displayName: String {
        if let p = productName, p.lowercased().contains("open-plaud") || p.lowercased().contains("openplaud") {
            return p
        }
        return "open-plaud device (ESP32)"
    }
}

/// Pure VID/PID interpretation. Separated from `USBDevice` so the mapping is a
/// trivially-testable free function (covered by `openplaud selftest`).
public enum USBInterpreter {
    /// PIDs (all under VID `0x303A`) we recognize.
    public enum KnownPID {
        public static let usbSerialJTAG = 0x1001   // recorder firmware today
        public static let tinyusbMSC    = 0x4002   // drive mode (MSC)
        public static let tinyusbMSCCDC = 0x4003   // drive mode (MSC + CDC)
    }

    /// Map a USB Product ID to what the device can do. Unknown PIDs map to
    /// `.unknown` (present, but we make no capability claim).
    public static func mode(forProductID pid: Int) -> USBDeviceMode {
        switch pid {
        case KnownPID.usbSerialJTAG:
            return .recorder
        case KnownPID.tinyusbMSC, KnownPID.tinyusbMSCCDC:
            return .drive
        default:
            return .unknown
        }
    }

    /// Is this VID our recorder's vendor (Espressif)?
    public static func isOpenPlaudVendor(_ vid: Int) -> Bool { vid == espressifVendorID }
}

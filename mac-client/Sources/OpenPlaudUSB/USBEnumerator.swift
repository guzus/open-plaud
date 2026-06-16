import Foundation
import IOKit
import OpenPlaudCore

/// IOKit-backed USB enumeration. Lives outside OpenPlaudCore (which is
/// Foundation-only) but is shared by the CLI and the app so the IOKit read path
/// isn't duplicated. Pure VID/PID *interpretation* stays in Core (`USBInterpreter`).
public enum USBEnumerator {

    /// IORegistry property keys (verified against the macOS SDK's
    /// `IOUSBHostFamilyDefinitions.h`): vendor/product IDs are CFNumbers under
    /// `"idVendor"`/`"idProduct"`, the product string under `"USB Product Name"`.
    enum Key {
        static let idVendor = "idVendor"
        static let idProduct = "idProduct"
        static let productName = "USB Product Name"
    }

    /// Service-class names to try, in order. `IOUSBHostDevice` is the modern class
    /// (macOS 10.11+); `IOUSBDevice` is the legacy class. We query BOTH every time
    /// and de-dupe by registry entry ID (more robust than a try-then-fallback —
    /// whichever class a given device surfaces under, we catch it exactly once).
    static let serviceClasses = ["IOUSBHostDevice", "IOUSBDevice"]

    /// One-shot snapshot of **all** currently-attached USB devices (every vendor),
    /// each decoded into a `USBDevice`. The app filters to Espressif; the CLI shows
    /// everything (with Espressif flagged) as real evidence the IOKit path works.
    ///
    /// Returns devices de-duplicated across the two service classes.
    public static func allUSBDevices() -> [USBDevice] {
        // De-dupe the same physical device across the two service-class queries by
        // its IORegistry entry ID — a globally-unique 64-bit id that is STABLE for
        // a given registry entry across separate iterators. (The raw
        // `io_object_t` mach-port number is NOT stable across iterators, and a
        // VID:PID:name key would wrongly merge two genuinely-distinct devices of
        // the same model — e.g. two identical hubs — so neither is a correct
        // cross-iterator key.)
        var seenEntryIDs = Set<UInt64>()
        var devices: [USBDevice] = []

        for className in serviceClasses {
            // IOServiceGetMatchingServices consumes a reference to the matching
            // dictionary it's given, so build a fresh one for each call.
            guard let matching = IOServiceMatching(className) else { continue }
            var iterator: io_iterator_t = 0
            let kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
            guard kr == KERN_SUCCESS, iterator != 0 else { continue }
            defer { IOObjectRelease(iterator) }

            while case let service = IOIteratorNext(iterator), service != 0 {
                defer { IOObjectRelease(service) }
                var entryID: UInt64 = 0
                if IORegistryEntryGetRegistryEntryID(service, &entryID) == KERN_SUCCESS {
                    if seenEntryIDs.contains(entryID) { continue }
                    seenEntryIDs.insert(entryID)
                }
                if let dev = decode(service: service) { devices.append(dev) }
            }
        }
        return devices.sorted {
            $0.vendorID != $1.vendorID ? $0.vendorID < $1.vendorID
                : ($0.productID != $1.productID ? $0.productID < $1.productID
                   : ($0.productName ?? "") < ($1.productName ?? ""))
        }
    }

    /// Just the Espressif (VID `0x303A`) devices — i.e. open-plaud candidates.
    public static func openPlaudUSBDevices() -> [USBDevice] {
        allUSBDevices().filter { $0.isEspressif }
    }

    // MARK: - Registry decoding

    /// Decode a single USB service node into a `USBDevice`. Returns nil if it has
    /// no usable VID (a device without `idVendor` isn't something we can match).
    static func decode(service: io_registry_entry_t) -> USBDevice? {
        guard let vid = intProperty(service, Key.idVendor) else { return nil }
        let pid = intProperty(service, Key.idProduct) ?? 0
        let name = stringProperty(service, Key.productName)
        return USBDevice(vendorID: vid, productID: pid, productName: name)
    }

    /// Read a CFNumber registry property as Int.
    static func intProperty(_ service: io_registry_entry_t, _ key: String) -> Int? {
        guard let cf = IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0) else {
            return nil
        }
        let value = cf.takeRetainedValue()
        guard let number = value as? NSNumber else { return nil }
        return number.intValue
    }

    /// Read a CFString registry property as String (trimmed; nil if empty).
    static func stringProperty(_ service: io_registry_entry_t, _ key: String) -> String? {
        guard let cf = IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0) else {
            return nil
        }
        let value = cf.takeRetainedValue()
        guard let s = value as? String else { return nil }
        let trimmed = s.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

import Foundation
import IOKit
import OpenPlaudCore

/// Watches for the open-plaud device being plugged in / unplugged over USB and
/// reports the current set of Espressif (VID `0x303A`) USB devices.
///
/// Design mirrors `DeviceWatcher` (the DiskArbitration one): IOKit is used only
/// as a *change trigger*. The match/terminate callbacks drain their iterators
/// (which is what arms the notification AND surfaces already-connected devices),
/// then re-enumerate via `USBEnumerator.openPlaudUSBDevices()` and hand the set
/// back via `onChange` on `@MainActor`. We take the dispatch-queue route
/// (`IONotificationPortSetDispatchQueue`) — the direct analog of
/// `DASessionSetDispatchQueue` — so there's no manual run-loop-thread lifecycle.
///
/// Lifetime: the owner (AppModel) keeps this alive for the app's life. The C
/// callback context is `Unmanaged.passUnretained(self)`, valid because `stop()`
/// synchronously removes the notifications before `self` can go away.
public final class USBWatcher {
    /// Delivered on the main actor whenever the Espressif-device set changes.
    private let onChange: @MainActor ([USBDevice]) -> Void

    private let queue = DispatchQueue(label: "xyz.openplaud.usb-watcher")
    private var notifyPort: IONotificationPortRef?
    private var addedIterator: io_iterator_t = 0
    private var removedIterator: io_iterator_t = 0
    private var started = false

    public init(onChange: @escaping @MainActor ([USBDevice]) -> Void) {
        self.onChange = onChange
    }

    deinit { stop() }

    /// Begin watching. Registers first-match (added) + terminated (removed)
    /// notifications filtered to Espressif's VID, drains both iterators once to
    /// arm them and capture already-connected devices, and publishes the initial
    /// set. Idempotent. All IOKit setup runs on `queue`.
    public func start() {
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        queue.async { [weak self] in
            guard let self, !self.started else { return }
            self.started = true

            guard let port = IONotificationPortCreate(kIOMainPortDefault) else {
                // Without a notification port we can't get live events. Still
                // publish a one-shot snapshot so an already-connected device shows.
                self.publish()
                return
            }
            self.notifyPort = port
            // Deliver IOKit callbacks on our serial queue (analog of DA's
            // DASessionSetDispatchQueue) — no manual CFRunLoop thread needed.
            IONotificationPortSetDispatchQueue(port, self.queue)

            let matchCB: IOServiceMatchingCallback = { context, iterator in
                guard let context else { return }
                let me = Unmanaged<USBWatcher>.fromOpaque(context).takeUnretainedValue()
                me.drain(iterator)   // MUST drain or the notification never re-fires
                me.publish()
            }

            // IOServiceAddMatchingNotification CONSUMES one reference to the
            // matching dictionary, so build a fresh dict for each registration.
            if let addMatch = self.espressifMatchingDict() {
                let kr = IOServiceAddMatchingNotification(
                    port, kIOFirstMatchNotification, addMatch,
                    matchCB, ctx, &self.addedIterator)
                if kr == KERN_SUCCESS { self.drain(self.addedIterator) } // arm + catch existing
            }
            if let removeMatch = self.espressifMatchingDict() {
                let kr = IOServiceAddMatchingNotification(
                    port, kIOTerminatedNotification, removeMatch,
                    matchCB, ctx, &self.removedIterator)
                if kr == KERN_SUCCESS { self.drain(self.removedIterator) } // arm
            }

            self.publish()
        }
    }

    /// Stop watching and release all IOKit resources. Idempotent. Synchronous on
    /// `queue` so no callback can reference `self` after it returns (incl. deinit).
    public func stop() {
        queue.sync {
            if addedIterator != 0 { IOObjectRelease(addedIterator); addedIterator = 0 }
            if removedIterator != 0 { IOObjectRelease(removedIterator); removedIterator = 0 }
            if let port = notifyPort {
                IONotificationPortSetDispatchQueue(port, nil)
                IONotificationPortDestroy(port)
                notifyPort = nil
            }
            started = false
        }
    }

    // MARK: - Internals (on `queue`)

    /// Build a fresh matching dictionary for `IOUSBHostDevice` filtered to
    /// Espressif's VID. (Falls back to `IOUSBDevice` if the modern class is
    /// unavailable.) A *new* dict each call, because
    /// `IOServiceAddMatchingNotification` consumes one reference to whatever it's
    /// given — sharing one dict across the add+remove registrations would
    /// over-release it.
    ///
    /// We set the VID key through the `NSMutableDictionary` bridge rather than
    /// `CFDictionarySetValue(_, Unmanaged.passUnretained(x as CFString)...)`:
    /// bridging a Swift `String`/`Int` with `as CFString`/`as CFNumber` produces a
    /// *temporary* CF object, and taking its `.toOpaque()` pointer can dangle
    /// because the temporary may be released at the end of the statement, before
    /// the dictionary's key/value callbacks retain it. The bridge keeps proper
    /// ARC ownership, so the stored objects stay alive.
    private func espressifMatchingDict() -> CFMutableDictionary? {
        guard let dict = IOServiceMatching("IOUSBHostDevice") ?? IOServiceMatching("IOUSBDevice") else {
            return nil
        }
        let ns = dict as NSMutableDictionary
        // idVendor is a CFNumber in the registry; match on it so IOKit only
        // notifies us for Espressif devices, not every USB attach on the system.
        ns[USBEnumerator.Key.idVendor] = espressifVendorID
        return dict
    }

    /// Drain an IOKit iterator to exhaustion. Required: leaving objects in the
    /// iterator means the matching notification will never fire again.
    private func drain(_ iterator: io_iterator_t) {
        while case let service = IOIteratorNext(iterator), service != 0 {
            IOObjectRelease(service)
        }
    }

    /// Re-enumerate the current Espressif device set and publish on the main actor.
    private func publish() {
        let devices = USBEnumerator.openPlaudUSBDevices()
        let cb = onChange
        Task { @MainActor in cb(devices) }
    }
}

import Foundation
import DiskArbitration
import OpenPlaudCore

/// Watches for the open-plaud device's storage mounting/unmounting and reports
/// the set of currently-detected device volumes on the main actor.
///
/// Design (kept deliberately small): DiskArbitration is used only as a *change
/// trigger*, never as a data source. The appeared/disappeared C callbacks don't
/// parse disk descriptions — they just bump a debounced re-scan that re-runs the
/// pure `DeviceDetector.detectOpenPlaudVolumes()` (FileManager + the shallow
/// content probe) and hands the result back via `onChange` on `@MainActor`. A
/// ~2 s timer poll is wired as a belt-and-suspenders fallback in case a mount
/// event is ever missed. All filesystem work happens on a background queue.
///
/// Lifetime: the owner (AppModel) must keep this object alive for as long as the
/// session should run — the C callback context is `Unmanaged.passUnretained(self)`,
/// valid only while `self` outlives the registered session. `stop()`/`deinit`
/// tears the session down.
final class DeviceWatcher {
    /// Called whenever the detected set *might* have changed. Always delivered on
    /// the main actor. The array is the current detection (possibly empty).
    private let onChange: @MainActor ([DetectedVolume]) -> Void

    /// Serial queue for DA callbacks + all filesystem probing (off the main
    /// thread). DiskArbitration delivers its callbacks here (via
    /// `DASessionSetDispatchQueue`), and ALL mutable state below
    /// (`session`/`pollTimer`/`pending`/`callbacksRegistered`) is only ever read
    /// or written on this queue, so there is no data race on them.
    private let queue = DispatchQueue(label: "xyz.openplaud.device-watcher")
    private var session: DASession?
    private var pollTimer: DispatchSourceTimer?
    private var callbacksRegistered = false

    /// Debounce: a single mount can emit several DA events, and a disk can
    /// "appear" a beat before its volume is actually mounted. We coalesce bursts
    /// and re-probe shortly after so the volume path exists by the time we look.
    private var pending: DispatchWorkItem?
    private let debounce: DispatchTimeInterval = .milliseconds(400)
    private let pollInterval: DispatchTimeInterval = .seconds(2)

    // C callbacks are plain function pointers (no captured state — they recover
    // `self` from the opaque context). Stored so `stop()` can unregister them.
    private let appearedCallback: DADiskAppearedCallback = { _, context in
        guard let context else { return }
        Unmanaged<DeviceWatcher>.fromOpaque(context).takeUnretainedValue().scheduleRescan()
    }
    private let disappearedCallback: DADiskDisappearedCallback = { _, context in
        guard let context else { return }
        Unmanaged<DeviceWatcher>.fromOpaque(context).takeUnretainedValue().scheduleRescan()
    }

    init(onChange: @escaping @MainActor ([DetectedVolume]) -> Void) {
        self.onChange = onChange
    }

    deinit { stop() }

    /// Begin watching. Does an immediate sweep so an already-inserted card is
    /// caught regardless of DiskArbitration replay semantics, registers the
    /// appeared/disappeared triggers, and starts the poll fallback. Idempotent.
    ///
    /// All setup runs on `queue` so it can't race with an in-flight DA callback
    /// or with `stop()`.
    func start() {
        // Context = unretained pointer to self. Valid because the owner
        // (AppModel) keeps the watcher alive for the app's lifetime, and `stop()`
        // synchronously unregisters the callbacks before `self` can go away, so
        // no DA block can fire into freed memory.
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        queue.async { [weak self] in
            guard let self, self.session == nil else { return }
            self.rescanNow()
            self.startPollFallback()

            guard let session = DASessionCreate(kCFAllocatorDefault) else {
                // No DA session (extremely unlikely): the poll fallback still
                // delivers detection, just less instantly.
                return
            }
            self.session = session
            DASessionSetDispatchQueue(session, self.queue)
            DARegisterDiskAppearedCallback(session, nil, self.appearedCallback, ctx)
            DARegisterDiskDisappearedCallback(session, nil, self.disappearedCallback, ctx)
            self.callbacksRegistered = true
        }
    }

    /// Stop watching and release the session. Idempotent. Synchronous on `queue`
    /// so that once it returns, no further DA callback or debounced/poll work can
    /// reference `self` — making `passUnretained(self)` safe across teardown
    /// (incl. `deinit`).
    func stop() {
        queue.sync {
            pending?.cancel(); pending = nil
            pollTimer?.cancel(); pollTimer = nil
            if let session {
                if callbacksRegistered {
                    let ctx = Unmanaged.passUnretained(self).toOpaque()
                    DAUnregisterCallback(session, unsafeBitCast(appearedCallback, to: UnsafeMutableRawPointer.self), ctx)
                    DAUnregisterCallback(session, unsafeBitCast(disappearedCallback, to: UnsafeMutableRawPointer.self), ctx)
                    callbacksRegistered = false
                }
                DASessionSetDispatchQueue(session, nil)
                self.session = nil
            }
        }
    }

    // MARK: - Internals (all on `queue`)

    /// Coalesce a burst of DA events into one re-probe a short moment later, by
    /// which time a freshly-inserted volume has actually mounted. DA callbacks
    /// already arrive on `queue`, but we re-`async` so this is safe to call from
    /// anywhere and so `pending` is always touched on `queue`.
    private func scheduleRescan() {
        queue.async { [weak self] in
            guard let self else { return }
            self.pending?.cancel()
            let work = DispatchWorkItem { [weak self] in self?.rescanNow() }
            self.pending = work
            self.queue.asyncAfter(deadline: .now() + self.debounce, execute: work)
        }
    }

    /// Re-run the pure detector and publish the result on the main actor.
    ///
    /// Back-to-back rescans (a DA event landing next to a poll tick) could in
    /// theory deliver out of order via unstructured tasks, but that's harmless
    /// here: `AppModel.deviceVolumesChanged` ignores any delivery whose set equals
    /// the last one, so a stale duplicate is a no-op and the steady state always
    /// converges to the latest detection.
    private func rescanNow() {
        let found = DeviceDetector.detectOpenPlaudVolumes()
        let cb = onChange
        Task { @MainActor in cb(found) }
    }

    private func startPollFallback() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + pollInterval, repeating: pollInterval)
        timer.setEventHandler { [weak self] in self?.rescanNow() }
        timer.resume()
        pollTimer = timer
    }
}

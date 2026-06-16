import Foundation

/// Framework-independent self-tests for the pure logic in OpenPlaudCore.
///
/// Why not XCTest? The target environment (macOS Command Line Tools without
/// full Xcode) ships `swift build` but not the XCTest module, so `swift test`
/// can't run there. These tests therefore live in the library and run via the
/// CLI (`openplaud selftest`), giving runnable, headless evidence anywhere
/// `swift build` works. They cover only pure logic — never Keychain, never the
/// network — exactly the units that benefit from deterministic checks.
public enum SelfTest {
    public struct Result {
        public let name: String
        public let passed: Bool
        public let detail: String?
    }

    /// Run all checks. Returns (results, allPassed).
    public static func runAll() -> (results: [Result], ok: Bool) {
        var out: [Result] = []
        func check(_ name: String, _ body: () throws -> Bool) {
            do {
                let ok = try body()
                out.append(Result(name: name, passed: ok, detail: ok ? nil : "assertion false"))
            } catch {
                out.append(Result(name: name, passed: false, detail: "threw: \(error)"))
            }
        }

        // --- WAV parsing ---
        check("wav: canonical 16k/16/stereo duration == 1.0s") {
            let data = WAVWriter.makePCM(sampleRate: 16_000, channels: 2, bitsPerSample: 16, sampleFrames: 16_000)
            let info = try WAVParser.parse(bytes: [UInt8](data))
            return info.sampleRate == 16_000 && info.channels == 2 && info.bitsPerSample == 16
                && abs(info.duration - 1.0) < 0.001
        }
        check("wav: short clip duration == 0.1s") {
            let info = try WAVParser.parse(bytes: [UInt8](WAVWriter.makePCM()))
            return abs(info.duration - 0.1) < 0.001
        }
        check("wav: walks past a leading JUNK chunk before fmt") {
            var base = WAVWriter.makePCM(sampleFrames: 800) // 0.05s
            let junk: [UInt8] = Array("JUNK".utf8) + [4, 0, 0, 0] + [0, 0, 0, 0]
            let head = base.prefix(12)
            let tail = base.suffix(from: 12)
            var spliced = Data(head); spliced.append(contentsOf: junk); spliced.append(tail)
            let newRiffSize = UInt32(spliced.count - 8).littleEndian
            withUnsafeBytes(of: newRiffSize) { spliced.replaceSubrange(4..<8, with: $0) }
            base = spliced
            let info = try WAVParser.parse(bytes: [UInt8](base))
            return info.sampleRate == 16_000 && abs(info.duration - 0.05) < 0.001
        }
        check("wav: rejects non-RIFF input") {
            do { _ = try WAVParser.parse(bytes: [UInt8]("NOPEnotawavfile!".utf8)); return false }
            catch { return true }
        }
        check("wav: clamps truncated data size (no over-report)") {
            var d = WAVWriter.makePCM(sampleFrames: 16_000)  // claims 1s
            d = Data(d.prefix(44 + 400))                      // 100 frames present
            let info = try WAVParser.parse(bytes: [UInt8](d), totalFileSize: d.count)
            return info.duration < 0.01
        }

        // --- Multipart (must match the device's contract exactly) ---
        check("multipart: field name is 'file' + Content-Type audio/wav + closing boundary") {
            let body = Multipart.wavBody(fileData: Data("HELLO".utf8), filename: "rec_0007.wav", boundary: "BND")
            let t = String(data: body.data, encoding: .utf8)!
            return t.contains("name=\"file\"")
                && t.contains("filename=\"rec_0007.wav\"")
                && t.contains("Content-Type: audio/wav")
                && t.contains("--BND\r\n")
                && t.contains("\r\n--BND--\r\n")
                && body.contentType == "multipart/form-data; boundary=BND"
        }
        check("multipart: filename is sanitized (no CR/LF/quote injection)") {
            let body = Multipart.wavBody(fileData: Data(), filename: "evil\"\r\nname.wav", boundary: "B")
            let t = String(data: body.data, encoding: .utf8)!
            return !t.contains("evil\"\r\nname") && t.contains("filename=\"evilname.wav\"")
        }

        // --- Idempotency store ---
        check("idempotency: synced clip is skipped; identity key varies w/ size+mtime") {
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent("op-self-\(UUID().uuidString)", isDirectory: true)
                .appendingPathComponent("state.json")
            let store = SyncStateStore(fileURL: url)
            let id = ClipIdentity(filename: "rec_0001.wav", sizeBytes: 1234, mtimeEpoch: 1_700_000_000)
            if store.isSynced(id) { return false }
            store.set(id, SyncRecord(state: .synced, remoteID: "abc"))
            if !store.isSynced(id) { return false }
            // persists across instances
            let reopened = SyncStateStore(fileURL: url)
            if !reopened.isSynced(id) || reopened.record(for: id)?.remoteID != "abc" { return false }
            // identity key sensitivity
            let a = ClipIdentity(filename: "rec_0001.wav", sizeBytes: 10, mtimeEpoch: 1)
            let b = ClipIdentity(filename: "rec_0001.wav", sizeBytes: 11, mtimeEpoch: 1)
            let c = ClipIdentity(filename: "rec_0001.wav", sizeBytes: 10, mtimeEpoch: 2)
            return a.key != b.key && a.key != c.key
        }
        check("idempotency: concurrent set() never loses a synced key on disk") {
            // Regression test for the lost-update race: many workers writing
            // distinct keys at once must ALL survive to disk (write order must
            // match snapshot order). Before the fix (persist outside the lock)
            // an earlier snapshot's atomic rename could land last and drop keys;
            // a fresh store reading the file would then miss them → re-upload.
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent("op-race-\(UUID().uuidString)", isDirectory: true)
                .appendingPathComponent("state.json")
            let store = SyncStateStore(fileURL: url)
            let n = 200
            DispatchQueue.concurrentPerform(iterations: n) { i in
                let id = ClipIdentity(filename: "rec_\(i).wav", sizeBytes: i, mtimeEpoch: i)
                store.set(id, SyncRecord(state: .synced, remoteID: "r\(i)"))
            }
            // A brand-new store reads ONLY the durable file — proves disk state,
            // not the in-memory dictionary.
            let reopened = SyncStateStore(fileURL: url)
            for i in 0..<n {
                let id = ClipIdentity(filename: "rec_\(i).wav", sizeBytes: i, mtimeEpoch: i)
                if !reopened.isSynced(id) { return false }
            }
            return reopened.all().count == n
        }

        // --- Retry classification ---
        check("retry: transient/5xx/429 retriable; 401/400/file not retriable") {
            return UploadError.transient("t").isRetriable
                && UploadError.serverTemporary(status: 503).isRetriable
                && UploadError.serverTemporary(status: 429).isRetriable
                && !UploadError.unauthorized(status: 401).isRetriable
                && !UploadError.permanent(status: 400, body: "x").isRetriable
                && !UploadError.fileUnreadable("gone").isRetriable
        }
        check("retry: 401 message guides user to token; no message leaks 'Bearer'") {
            let auth = UploadError.unauthorized(status: 401).userMessage.lowercased()
            let msgs = [
                UploadError.transient("x").userMessage,
                UploadError.serverTemporary(status: 500).userMessage,
                UploadError.unauthorized(status: 401).userMessage,
                UploadError.permanent(status: 413, body: "too big").userMessage,
            ]
            return auth.contains("token") && msgs.allSatisfy { !$0.contains("Bearer") }
        }

        // --- Scanner ---
        check("scanner: matches rec_*.wav case-insensitively, ignores others") {
            return ClipScanner.isRecWAV("rec_0001.wav")
                && ClipScanner.isRecWAV("REC_0001.WAV")
                && ClipScanner.isRecWAV("Rec_9999.Wav")
                && !ClipScanner.isRecWAV("note_0001.wav")
                && !ClipScanner.isRecWAV("rec_0001.mp3")
        }
        check("scanner: finds recursively + hydrates persisted state") {
            let fm = FileManager.default
            let root = fm.temporaryDirectory.appendingPathComponent("op-scan-\(UUID().uuidString)", isDirectory: true)
            let sub = root.appendingPathComponent("DCIM", isDirectory: true)
            try fm.createDirectory(at: sub, withIntermediateDirectories: true)
            let wav = WAVWriter.makePCM(sampleFrames: 16_000)
            try wav.write(to: root.appendingPathComponent("rec_0001.wav"))
            try wav.write(to: sub.appendingPathComponent("REC_0002.WAV"))
            try Data("x".utf8).write(to: root.appendingPathComponent("notes.txt"))
            let store = SyncStateStore(fileURL: root.appendingPathComponent("state.json"))
            var clips = ClipScanner.scan(root: root, store: store)
            guard clips.count == 2, clips.allSatisfy({ $0.state == .queued }) else { return false }
            guard let dur = clips.first?.duration, abs(dur - 1.0) < 0.05 else { return false }
            let target = clips.first { $0.filename.lowercased() == "rec_0001.wav" }!
            store.set(target.identity, SyncRecord(state: .synced, remoteID: "r1"))
            clips = ClipScanner.scan(root: root, store: store)
            let again = clips.first { $0.filename.lowercased() == "rec_0001.wav" }!
            return again.state == .synced && again.remoteID == "r1"
        }
        check("scanner: skips a top-level sent/ (device already uploaded those)") {
            // Mirrors a real mounted microSD: pending clips in root, a sent/ dir
            // of device-uploaded clips, and an unrelated DCIM/ subdir. Only root +
            // DCIM clips should surface; sent/ must NOT be re-listed/re-uploaded.
            let fm = FileManager.default
            let root = fm.temporaryDirectory.appendingPathComponent("op-sent-\(UUID().uuidString)", isDirectory: true)
            let sent = root.appendingPathComponent("sent", isDirectory: true)
            let dcim = root.appendingPathComponent("DCIM", isDirectory: true)
            try fm.createDirectory(at: sent, withIntermediateDirectories: true)
            try fm.createDirectory(at: dcim, withIntermediateDirectories: true)
            let wav = WAVWriter.makePCM(sampleFrames: 800)
            try wav.write(to: root.appendingPathComponent("rec_0007.wav"))
            try wav.write(to: dcim.appendingPathComponent("rec_0008.wav"))
            try wav.write(to: sent.appendingPathComponent("rec_0001.wav"))
            try wav.write(to: sent.appendingPathComponent("rec_0002.wav"))
            let clips = ClipScanner.scan(root: root)
            let names = Set(clips.map { $0.filename.lowercased() })
            return clips.count == 2
                && names.contains("rec_0007.wav")   // root → kept
                && names.contains("rec_0008.wav")   // DCIM/ → kept (other subdirs untouched)
                && !names.contains("rec_0001.wav")  // sent/ → skipped
                && !names.contains("rec_0002.wav")
        }
        check("scanner: isInSentDir matches only a top-level sent/, case-insensitive") {
            let root = URL(fileURLWithPath: "/Volumes/SD16G")
            return ClipScanner.isInSentDir(fileURL: root.appendingPathComponent("sent/rec_0001.wav"), root: root)
                && ClipScanner.isInSentDir(fileURL: root.appendingPathComponent("SENT/rec_0001.wav"), root: root)
                && !ClipScanner.isInSentDir(fileURL: root.appendingPathComponent("rec_0001.wav"), root: root)
                && !ClipScanner.isInSentDir(fileURL: root.appendingPathComponent("DCIM/sent/rec_0001.wav"), root: root)
        }

        // --- Device detection (content-based, shallow) ---
        // Helper: build a throwaway dir tree and run the detector against it.
        func makeDir(_ build: (URL) throws -> Void) throws -> URL {
            let root = FileManager.default.temporaryDirectory
                .appendingPathComponent("op-detect-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            try build(root)
            return root
        }
        func touch(_ url: URL) throws { try Data("x".utf8).write(to: url) }

        check("detect: root with rec_*.wav looks like the device (name + hint)") {
            let root = try makeDir { r in
                try touch(r.appendingPathComponent("rec_0001.wav"))
                try touch(r.appendingPathComponent("REC_0002.WAV"))
                try touch(r.appendingPathComponent("notes.txt"))
            }
            guard let dv = DeviceDetector.looksLikeOpenPlaudVolume(at: root) else { return false }
            // Note: `name` comes from the volume the temp dir lives on (e.g.
            // "Macintosh HD"), not the dir's last component — that's correct
            // behavior; on a real `/Volumes/SD16G` mount it's the card's name.
            // So assert it's non-empty, not equal to the path component.
            return dv.clipCountHint == 2 && dv.url == root && !dv.name.isEmpty
        }
        check("detect: a folder with no rec_*.wav is NOT the device") {
            let root = try makeDir { r in
                try touch(r.appendingPathComponent("notes.txt"))
                try touch(r.appendingPathComponent("audio.mp3"))
            }
            return DeviceDetector.looksLikeOpenPlaudVolume(at: root) == nil
        }
        check("detect: empty root but clips under /sent still looks like the device") {
            let root = try makeDir { r in
                let sent = r.appendingPathComponent("sent", isDirectory: true)
                try FileManager.default.createDirectory(at: sent, withIntermediateDirectories: true)
                try touch(sent.appendingPathComponent("rec_0009.wav"))
            }
            guard let dv = DeviceDetector.looksLikeOpenPlaudVolume(at: root) else { return false }
            return dv.clipCountHint == 1
        }
        check("detect: probe is SHALLOW — a rec_*.wav buried deep does NOT trigger") {
            // Critical perf guard: detection must not recurse, or it would walk
            // every mounted volume (incl. the system disk) on each mount event.
            let root = try makeDir { r in
                let deep = r.appendingPathComponent("DCIM/sub/more", isDirectory: true)
                try FileManager.default.createDirectory(at: deep, withIntermediateDirectories: true)
                try touch(deep.appendingPathComponent("rec_0001.wav"))
            }
            return DeviceDetector.looksLikeOpenPlaudVolume(at: root) == nil
        }
        check("detect: missing/unreadable directory is safely not-the-device") {
            let ghost = FileManager.default.temporaryDirectory
                .appendingPathComponent("op-detect-missing-\(UUID().uuidString)", isDirectory: true)
            return DeviceDetector.looksLikeOpenPlaudVolume(at: ghost) == nil
        }

        // --- USB VID/PID interpretation (pure; the IOKit read path is App-side) ---
        check("usb: VID 0x303A is recognized as the open-plaud (Espressif) vendor") {
            return espressifVendorID == 0x303A
                && USBInterpreter.isOpenPlaudVendor(0x303A)
                && !USBInterpreter.isOpenPlaudVendor(0x05AC) // Apple's VID, not ours
        }
        check("usb: PID 0x1001 = recorder (USB-Serial/JTAG), exposes no volume") {
            let m = USBInterpreter.mode(forProductID: 0x1001)
            return m == .recorder && m.exposesVolume == false
        }
        check("usb: PIDs 0x4002 / 0x4003 = drive mode (MSC / MSC+CDC), exposes a volume") {
            return USBInterpreter.mode(forProductID: 0x4002) == .drive
                && USBInterpreter.mode(forProductID: 0x4003) == .drive
                && USBInterpreter.mode(forProductID: 0x4002).exposesVolume == true
        }
        check("usb: an unknown PID maps to .unknown (present, no capability claim)") {
            let m = USBInterpreter.mode(forProductID: 0x9999)
            return m == .unknown && m.exposesVolume == false
        }
        check("usb: device.mode + isEspressif derive from VID/PID") {
            let recorder = USBDevice(vendorID: 0x303A, productID: 0x1001, productName: nil)
            let drive = USBDevice(vendorID: 0x303A, productID: 0x4003, productName: nil)
            let other = USBDevice(vendorID: 0x1234, productID: 0x0001, productName: nil)
            return recorder.isEspressif && recorder.mode == .recorder
                && drive.isEspressif && drive.mode == .drive
                && !other.isEspressif
        }
        check("usb: displayName is honest — generic ESP32 unless product says open-plaud") {
            let generic = USBDevice(vendorID: 0x303A, productID: 0x1001, productName: "USB JTAG/serial debug unit")
            let branded = USBDevice(vendorID: 0x303A, productID: 0x1001, productName: "open-plaud")
            let nameless = USBDevice(vendorID: 0x303A, productID: 0x1001, productName: nil)
            return generic.displayName == "open-plaud device (ESP32)"
                && branded.displayName == "open-plaud"
                && nameless.displayName == "open-plaud device (ESP32)"
        }

        // --- Server response decode (tolerant) ---
        check("decode: full Recording (id/status/size/transcript/summary)") {
            let json = """
            {"id":"abc123","status":"received","filename":"rec_0001.wav","size_bytes":102444,
             "created_at":"2026-06-16T00:00:00Z","content_type":"audio/wav","stored_name":"abc123.wav",
             "transcript":{"text":"hello","language":"en","segments":[]},
             "summary":{"summary":"a greeting","action_items":[]},"error":null}
            """
            let rec = RecordingResponse.decode(from: Data(json.utf8))
            return rec?.id == "abc123" && rec?.status == "received" && rec?.sizeBytes == 102444
                && rec?.transcriptText == "hello" && rec?.summaryText == "a greeting" && rec?.error == nil
        }
        check("decode: minimal {id,status} still decodes (2xx success path)") {
            let rec = RecordingResponse.decode(from: Data(#"{"id":"x","status":"done"}"#.utf8))
            return rec?.id == "x" && rec?.status == "done"
        }
        check("decode: error as string OR object") {
            let s = RecordingResponse.decode(from: Data(#"{"id":"1","status":"failed","error":"boom"}"#.utf8))
            let o = RecordingResponse.decode(from: Data(#"{"id":"1","status":"failed","error":{"message":"nested boom"}}"#.utf8))
            return s?.error == "boom" && o?.error == "nested boom"
        }

        let ok = out.allSatisfy { $0.passed }
        return (out, ok)
    }
}

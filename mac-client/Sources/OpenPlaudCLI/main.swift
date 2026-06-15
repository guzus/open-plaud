import Foundation
import OpenPlaudCore

// Headless CLI for verification + scripted syncs.
//
// Driven entirely by flags/env so it never touches Keychain or UserDefaults
// (those are GUI-only). The token is read from --token or $OPEN_PLAUD_TOKEN and
// is NEVER printed back.
//
// Usage:
//   openplaud health   --url <base>
//   openplaud scan     --folder <path>
//   openplaud sync     --folder <path> --url <base> [--token <t>] [--concurrency N] [--state <file>]
//   openplaud make-wav --out <path>            (write a device-format test WAV)
//
// Token can also come from $OPEN_PLAUD_TOKEN; URL from $OPEN_PLAUD_URL.

let defaultURL = "https://ingest-production-4fd2.up.railway.app"

func env(_ k: String) -> String? {
    guard let v = ProcessInfo.processInfo.environment[k], !v.isEmpty else { return nil }
    return v
}

struct Args {
    var values: [String: String] = [:]
    var flags: Set<String> = []
    init(_ argv: [String]) {
        var i = 0
        while i < argv.count {
            let a = argv[i]
            if a.hasPrefix("--") {
                let key = String(a.dropFirst(2))
                if i + 1 < argv.count, !argv[i + 1].hasPrefix("--") {
                    values[key] = argv[i + 1]; i += 2
                } else {
                    flags.insert(key); i += 1
                }
            } else { i += 1 }
        }
    }
    func string(_ k: String) -> String? { values[k] }
}

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write(Data((msg + "\n").utf8))
    exit(1)
}

func parseBase(_ args: Args) -> URL {
    let raw = args.string("url") ?? env("OPEN_PLAUD_URL") ?? defaultURL
    guard let u = URL(string: raw) else { fail("Invalid --url: \(raw)") }
    return u
}

func fmtDuration(_ s: TimeInterval?) -> String {
    guard let s, s.isFinite else { return "  ?  " }
    let total = Int(s.rounded())
    return String(format: "%d:%02d", total / 60, total % 60)
}

func fmtSize(_ bytes: Int) -> String {
    if bytes >= 1_048_576 { return String(format: "%.1f MB", Double(bytes) / 1_048_576) }
    if bytes >= 1024 { return String(format: "%.0f KB", Double(bytes) / 1024) }
    return "\(bytes) B"
}

let argv = Array(CommandLine.arguments.dropFirst())
guard let command = argv.first else {
    print("""
    openplaud — open-plaud Mac sync CLI

    Commands:
      health    --url <base>
      scan      --folder <path>
      sync      --folder <path> [--url <base>] [--token <t>] [--concurrency N] [--state <file>]
      make-wav  --out <path.wav>
      selftest                                  (run pure-logic unit checks)

    Env: OPEN_PLAUD_URL, OPEN_PLAUD_TOKEN
    """)
    exit(0)
}
let args = Args(Array(argv.dropFirst()))

switch command {

case "selftest":
    let (results, ok) = SelfTest.runAll()
    for r in results {
        let mark = r.passed ? "PASS" : "FAIL"
        print("[\(mark)] \(r.name)" + (r.detail.map { " — \($0)" } ?? ""))
    }
    let passed = results.filter { $0.passed }.count
    print("\n\(passed)/\(results.count) checks passed.")
    exit(ok ? 0 : 1)

case "make-wav":
    guard let out = args.string("out") else { fail("make-wav requires --out <path.wav>") }
    let data = WAVWriter.makePCM() // 16 kHz / 16-bit / stereo, 0.1s
    do {
        try data.write(to: URL(fileURLWithPath: out))
        print("Wrote \(data.count) bytes (16 kHz/16-bit/stereo WAV) to \(out)")
    } catch { fail("write failed: \(error.localizedDescription)") }

case "health":
    let base = parseBase(args)
    let client = UploadClient(config: ServerConfig(baseURL: base))
    let sem = DispatchSemaphore(value: 0)
    var ok = false
    Task {
        ok = (try? await client.health()) ?? false
        sem.signal()
    }
    sem.wait()
    print("health \(base.appendingPathComponent("healthz").absoluteString): \(ok ? "ok" : "FAILED")")
    exit(ok ? 0 : 1)

case "scan":
    guard let folder = args.string("folder") else { fail("scan requires --folder <path>") }
    let store = SyncStateStore(fileURL: stateURL(args))
    let clips = ClipScanner.scan(root: URL(fileURLWithPath: folder), store: store)
    if clips.isEmpty {
        print("No rec_*.wav files found under \(folder)")
        exit(0)
    }
    func pad(_ s: String, _ w: Int, right: Bool = false) -> String {
        if s.count >= w { return s }
        let fill = String(repeating: " ", count: w - s.count)
        return right ? fill + s : s + fill
    }
    print("\(pad("FILE", 24)) \(pad("SIZE", 9, right: true)) \(pad("DUR", 6, right: true))  STATE")
    for c in clips {
        let line = "\(pad(c.filename, 24)) \(pad(fmtSize(c.sizeBytes), 9, right: true)) "
            + "\(pad(fmtDuration(c.duration), 6, right: true))  \(c.state.rawValue)"
        print(line)
    }
    print("\n\(clips.count) clip(s).")

case "sync":
    guard let folder = args.string("folder") else { fail("sync requires --folder <path>") }
    let base = parseBase(args)
    let token = args.string("token") ?? env("OPEN_PLAUD_TOKEN")
    let concurrency = Int(args.string("concurrency") ?? "3") ?? 3

    let store = SyncStateStore(fileURL: stateURL(args))
    let clips = ClipScanner.scan(root: URL(fileURLWithPath: folder), store: store)
    if clips.isEmpty { print("No rec_*.wav files found under \(folder)"); exit(0) }

    let client = UploadClient(config: ServerConfig(baseURL: base, token: token))
    let engine = SyncEngine(client: client, store: store, maxConcurrent: concurrency)

    if token == nil {
        FileHandle.standardError.write(Data("warning: no token (--token / $OPEN_PLAUD_TOKEN); server requires auth and will 401\n".utf8))
    }

    let sem = DispatchSemaphore(value: 0)
    var result = SyncSummary()
    Task {
        result = await engine.sync(clips: clips) { p in
            // Never prints token. Status line per transition.
            switch p.state {
            case .syncing: print("→ \(p.filename): uploading…")
            case .synced:  print("✓ \(p.filename): synced\(p.remoteID.map { " (\($0))" } ?? "")")
            case .failed:  print("✗ \(p.filename): \(p.error ?? "failed")")
            case .queued:  break
            }
        }
        sem.signal()
    }
    sem.wait()
    print("\nDone — uploaded \(result.uploaded), skipped \(result.skipped) (already synced), failed \(result.failed).")
    exit(result.failed == 0 ? 0 : 2)

default:
    fail("Unknown command: \(command)")
}

// State file: --state overrides; else the shared default location.
func stateURL(_ args: Args) -> URL {
    if let s = args.string("state") { return URL(fileURLWithPath: s) }
    return SyncStateStore.defaultURL()
}

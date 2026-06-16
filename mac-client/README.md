# open-plaud — Mac client

A native macOS app that syncs your **open-plaud** voice-recorder clips to your
cloud server and manages settings.

The recorder (a Waveshare ESP32-S3) writes `rec_NNNN.wav` clips (16 kHz / 16-bit
/ stereo PCM) to a microSD card and can upload them over Wi-Fi. This client is
the companion for **everything else**: when the device isn't on Wi-Fi, you pull
the clips off it and upload them yourself — idempotently, with retries — and you
manage the server URL and auth token in one place.

## v1 = folder / SD-card sync (no firmware changes)

v1 reads clips as files from a **folder or a mounted volume**:

- Insert the device's microSD card → it mounts as a volume → the app
  **auto-detects it** and shows "Device connected", no folder-picking needed.
- Or point the app at any folder that contains `rec_*.wav` clips.

This deliberately needs **zero firmware changes** and works whether clips arrive
via SD removal today, or via a future "device exposes its SD as a USB drive"
(USB-MSC) firmware step — same code path either way (a USB-exposed SD is just
another mounted volume, and the same content-based detection lights up).

### Why folder/SD sync for v1 (premise check)

The alternatives — a USB-serial protocol or BLE — are both *more* work **and**
firmware-coupled (they'd require new device code, and this repo's firmware is
under active development by someone else). Folder/SD sync is the only path with
no firmware dependency, and it already serves both the "pull the SD" workflow now
and the USB-MSC workflow later without changes. The one tradeoff: until USB-MSC
lands, you physically move the microSD card. Verdict: **folder/SD sync is the
correct v1.** See the roadmap below for the wired-in / wireless options.

## What it does

- **Auto-detect the device (two signals)**: the app recognizes the open-plaud
  device from *either* of two independent watchers:
  - **SD volume (content)** — via the **DiskArbitration** framework (with a 2 s
    poll fallback), it recognizes the card by **content**: an external volume
    whose root (or `sent/` subdir) holds `rec_*.wav`. Content-based, not name- or
    USB-ID-based, so the *same code* handles an SD in a card reader today and a
    future USB-MSC drive. → **"Device connected — <volume>, N clips (M to sync)"**,
    auto-selected as the sync source; sync enabled.
  - **USB cable (vendor ID)** — via **IOKit USB**, it notices the device the
    moment it's plugged in, *even in normal recorder mode when no drive is
    mounted* (today the recorder just exposes a serial port). It matches
    Espressif's vendor ID `0x303A` and reads the product ID to tell recorder mode
    (`0x1001`, USB-Serial/JTAG) from a future drive mode (`0x4002`/`0x4003`,
    USB-MSC). → **"open-plaud connected (recorder mode)"** with guidance to insert
    the microSD or use Wi-Fi; sync stays disabled (clips aren't readable over the
    cable yet).

  Precedence is deliberate and non-regressing: a **mounted volume always wins**
  (a microSD in a generic card reader enumerates under the reader's USB vendor,
  not Espressif's, so the USB signal only *adds* the recorder-mode state — it
  never gates the volume path). A folder you pick by hand is never clobbered by
  auto-detect (a "Use Device" button lets you switch). VID `0x303A` is generic to
  all Espressif boards, so the label is honest ("open-plaud device (ESP32)")
  unless the USB product string literally says `open-plaud`.
- **Scan**: recursively finds `rec_*.wav` (case-insensitive) under the chosen
  folder/volume. For each clip it shows filename, size, **duration** (parsed from
  the WAV header), and **sync state**: `queued` / `syncing` / `synced` / `failed`.
  Clips the device already uploaded over Wi-Fi (which it moves into a top-level
  `sent/` directory) are **skipped**, so plugging in a card never re-uploads them.
- **Sync**: uploads queued clips to `POST {base}/v1/recordings` as
  `multipart/form-data`, field name **`file`**, part `Content-Type: audio/wav` —
  byte-for-byte the same contract the device uses. Uploads are:
  - **concurrency-limited** (default 3 in flight),
  - **retried** on transient failures (network errors, 5xx, 429) with backoff;
    **not** retried on permanent ones (400/401/403/413),
  - **idempotent** — a local synced-state store keyed by
    `filename + size + mtime` means re-runs never re-upload. State is flushed to
    disk immediately after each success, so even a crash mid-sync won't cause a
    duplicate on the next run.
- **Never destructive**: the app never deletes or moves your source WAVs.
- **Settings**: server base URL (stored in `UserDefaults`) and auth token
  (stored **only** in the macOS **Keychain** — never in source, never in
  preferences, never logged).

## Layout

```
mac-client/
├── Package.swift                         # SwiftPM: builds headlessly with `swift build`
├── README.md
├── .gitignore                            # .build/, DerivedData, *.xcuserstate, local state
└── Sources/
    ├── OpenPlaudCore/                     # testable core library (no SwiftUI, no Keychain, no IOKit)
    │   ├── WAV.swift                      #   WAV header parse (duration) + minimal WAV writer
    │   ├── Clip.swift                     #   Clip model + ClipIdentity (idempotency key)
    │   ├── ClipScanner.swift              #   recursive rec_*.wav scan (skips sent/) + state hydration
    │   ├── DeviceDetector.swift           #   pure, shallow "is this an open-plaud volume?" + volume enum
    │   ├── USBDevice.swift                #   pure USB model + VID/PID→mode interpretation (no IOKit)
    │   ├── SyncStateStore.swift           #   durable JSON synced-state store (shared CLI+GUI)
    │   ├── Multipart.swift                #   multipart/form-data body (field "file", audio/wav)
    │   ├── ServerModels.swift             #   tolerant decode of the server's Recording JSON
    │   ├── UploadClient.swift             #   POST upload + health + retry classification
    │   ├── SyncEngine.swift               #   concurrency-limited, retrying, idempotent sync
    │   └── SelfTest.swift                 #   framework-free unit checks (see Testing below)
    ├── OpenPlaudUSB/                       # IOKit USB enumeration + watcher (shared by CLI + app)
    │   ├── USBEnumerator.swift            #   one-shot IOKit enumeration of attached USB devices
    │   └── USBWatcher.swift               #   live attach/detach notifications (IOServiceAddMatchingNotification)
    ├── OpenPlaudCLI/                       # headless CLI for verification + scripted syncs
    │   └── main.swift                      #   openplaud {health,detect,usb,scan,sync,make-wav,selftest}
    └── OpenPlaudApp/                       # SwiftUI app — UI + wiring only
        ├── OpenPlaudApp.swift             #   @main App + Settings scene
        ├── ContentView.swift              #   "Device connected" / recorder-mode banner, clip list, states
        ├── DeviceWatcher.swift            #   DiskArbitration mount/unmount watcher (App-only)
        ├── SettingsView.swift             #   server URL + token (write-only) UI
        ├── AppModel.swift                 #   ObservableObject: clips, volume + USB detection, ConnectionState
        ├── AppSettings.swift              #   URL in UserDefaults, token via Keychain
        └── Keychain.swift                 #   generic-password Keychain wrapper for the token
```

The split is deliberate: every verifiable piece lives in **`OpenPlaudCore`**; the
**App** target is UI + wiring only. So the whole package compiles headlessly and
the logic is testable without a GUI, a network, or the Keychain.

## Build

```bash
cd mac-client
swift build            # builds OpenPlaudCore, the CLI (openplaud), and the app
```

> Requires the Swift toolchain (Swift 5.9+ / macOS 13+). Apple's Command Line
> Tools are enough for `swift build` and the CLI. A signed/notarized `.app` with
> reliable Keychain needs **Xcode** (see Roadmap).

## Run the GUI

```bash
swift run OpenPlaudApp
```

1. **Insert the device's microSD** (or plug in the device) — it mounts, the app
   auto-detects it, and the **"Device connected"** banner appears with the live
   clip count. No folder-picking required. (You can still **Choose a folder…**
   for arbitrary clip folders.)
   - If you plug in the device over **USB in recorder mode** (no microSD inserted),
     the app recognizes it and shows **"open-plaud connected (recorder mode)"** —
     a distinct state that does *not* offer Sync, since clips aren't readable over
     the cable yet. Insert the microSD (or use Wi-Fi sync) to upload.
2. Open **Settings** (gear / ⌘,) → set the **Server base URL** and paste your
   **auth token** (saved to the Keychain). Use **Test Connection** to confirm.
3. Click **Sync N clips** (⌘S). Watch each row go `queued → syncing → synced`.

## Run the CLI (headless / scriptable)

The CLI is driven entirely by flags/env — it never touches the Keychain or
UserDefaults, which keeps it safe for CI and headless verification.

```bash
BIN=.build/debug/openplaud           # after `swift build`

# Liveness probe
$BIN health --url https://openplaud.guzus.xyz

# Which mounted volumes look like an open-plaud device? (content-based detection)
$BIN detect

# Which USB devices are attached? Flags the open-plaud / Espressif ones + mode.
# (Filesystem-independent: works even in recorder mode with no volume mounted.)
$BIN usb

# Generate a device-format test WAV (16 kHz/16-bit/stereo)
$BIN make-wav --out /tmp/rec_0001.wav

# List clips in a folder/volume (sent/ is skipped — see "Auto-detect" above)
$BIN scan --folder /Volumes/SD16G

# Upload queued clips (idempotent; re-runs skip already-synced)
$BIN sync --folder /Volumes/SD16G \
          --url https://openplaud.guzus.xyz \
          --token "$OPEN_PLAUD_TOKEN" \
          --concurrency 3

# Run the pure-logic unit checks
$BIN selftest
```

Environment variables `OPEN_PLAUD_URL` and `OPEN_PLAUD_TOKEN` are honored if the
matching flags are omitted. **The token is never printed or logged.**

### Setting the token

- **GUI**: Settings → Auth token. Stored in the macOS Keychain.
- **CLI**: pass `--token …` or export `OPEN_PLAUD_TOKEN=…`. Nothing is written to
  disk by the CLI; the token lives only in the process environment for that run.

An authenticated upload **requires your token at runtime** — this repo never
contains one. Without a token the server returns `401` and the client surfaces
"Unauthorized — set your token in Settings" (and does not retry).

## How it maps to the device + server

| Concern | Device (firmware) | This Mac client |
|---|---|---|
| Clip files | writes `rec_NNNN.wav` to SD root | finds `rec_*.wav` recursively |
| Audio format | 16 kHz / 16-bit / **stereo** PCM | parses + generates the same |
| Upload endpoint | `POST {base}/v1/recordings` | identical |
| Multipart field | `file`, part `Content-Type: audio/wav` | identical |
| Auth | `Authorization: Bearer <token>` | identical (token from Keychain/flag) |
| Health | — | `GET {base}/healthz` → `{"status":"ok"}` |
| Response | device only checks for 2xx | decodes `Recording` tolerantly (`id, status, filename, size_bytes, created_at, transcript, summary, error`) |

The server contract is documented in [`../server-go/README.md`](../server-go/README.md).

## Connection-method roadmap

| Stage | How clips reach the Mac | Firmware change | Status |
|---|---|---|---|
| **v1 (now)** | Remove microSD → mount → auto-detected by content | none | **shipped** |
| **v1 (now)** | Plug in over USB → recognized by vendor ID (recorder mode shown) | none | **shipped** (recognition only; can't pull clips over the serial cable yet) |
| **v1.x** | Device exposes its SD as a USB drive (USB-MSC) | yes (device side) | same client code path; auto-detect already maps PID `0x4002`/`0x4003` → drive mode |
| **v2** | USB-serial pull protocol | yes | future — direct cable transfer without ejecting SD |
| **v2** | BLE wireless pull | yes (more work) | future — cable-free |

v1 is built so the USB-MSC step is **free** on the app side: a USB-exposed SD is
just another mounted volume, and the USB watcher already recognizes the drive-mode
product IDs.

## Testing / verification

This package targets the **macOS Command Line Tools** environment, which ships
`swift build` but **not** the XCTest / swift-testing modules — so `swift test`
can't run here. Rather than ship a test target that errors, the unit assertions
live in `OpenPlaudCore/SelfTest.swift` and run headlessly:

```bash
swift run openplaud selftest     # exits non-zero if any check fails
```

These cover the load-bearing pure logic: WAV-header duration parsing (including
extra chunks + truncated files), the multipart byte layout (field name `file`,
boundary, `Content-Type: audio/wav`), idempotency (same identity ⇒ skipped;
persists across runs), retry classification (transient vs permanent), the
recursive scanner (including the `sent/`-skip so device-uploaded clips aren't
re-listed), **content-based device detection** (root or `sent/` clips ⇒ detected;
non-clip folders ⇒ not; detection is shallow so a deeply-buried `rec_*.wav` does
not false-trigger), **USB VID/PID interpretation** (VID `0x303A` ⇒ Espressif/
open-plaud; PID `0x1001` ⇒ recorder mode, no volume; `0x4002`/`0x4003` ⇒ drive
mode; unknown PID ⇒ present-but-mode-unknown; honest display name), and tolerant
server-response decoding. With Xcode installed, a standard `.testTarget` can be
re-added trivially and these same checks moved into `@Test`/`XCTest`.

The pure predicates live in `OpenPlaudCore` (`DeviceDetector`, `USBInterpreter`),
which is why they're selftestable headlessly. The framework-bound *watchers* live
outside Core so it stays Foundation-only: the SD mount/unmount watcher
(`DeviceWatcher`, DiskArbitration) is App-side, and the USB attach/detach watcher
+ IOKit enumeration (`OpenPlaudUSB`) is a small shared library used by both the
CLI and the app. To see detection against **real** hardware:

```bash
openplaud detect   # mounted volumes that look like the device (content-based)
openplaud usb      # attached USB devices; flags open-plaud / Espressif + mode
```

`openplaud usb` is filesystem-independent — it works in recorder mode when no
volume is mounted. (On a machine with the device asleep or unplugged it lists the
other USB devices, or none; that's expected.)

For a live end-to-end check against a server (proves multipart + auth header +
response decode), run the Go server locally with a throwaway token and sync a
generated WAV:

```bash
# terminal 1 — throwaway local server (writes nothing into your real data)
cd ../server-go && OP_STORAGE_DIR=$(mktemp -d) OP_AUTH_TOKEN=localtest PORT=8000 go run .

# terminal 2
WORK=$(mktemp -d); BIN=.build/debug/openplaud
$BIN make-wav --out "$WORK/rec_0001.wav"
$BIN sync --folder "$WORK" --url http://127.0.0.1:8000 --token localtest   # → synced
$BIN sync --folder "$WORK" --url http://127.0.0.1:8000 --token localtest   # → skipped (idempotent)
```

## Security notes

- The auth token is stored **only** in the macOS Keychain (GUI) or held in
  memory for one run (CLI). It is **never** written to UserDefaults, **never**
  committed, and **never** printed or logged — not even in error messages.
- The app never deletes or moves your source WAV files.

## Known limitations / risks

- **Server-side de-dup is the client's job.** The server stores every upload as a
  new blob (no content-dedup endpoint). This client's local state prevents *its
  own* re-uploads, but if the **device also uploads the same clip over Wi-Fi**,
  the server will hold both copies. De-duping across the device + Mac is a server
  concern, out of scope for v1.
- **Idempotency key** is `filename + size + mtime`. Effectively collision-proof
  for `rec_NNNN.wav` clips from one device; a content hash would be the
  belt-and-suspenders upgrade (at the cost of hashing every file each scan).
- **Keychain reliability** needs a signed/bundled app; an unsigned `swift run`
  binary may prompt or fail to access the login keychain. That's why the CLI
  takes the token by flag/env instead.
- **USB recognition is presence-only; you can't pull clips over the cable yet.**
  In recorder mode the device exposes a USB-Serial/JTAG port, not a drive, so the
  app can *tell it's plugged in* (and say so) but can't read clips from it — that
  needs either the microSD inserted, Wi-Fi sync, or a future device-side USB-MSC /
  serial-pull protocol. The watcher already maps the drive-mode PIDs so the SD
  shows up automatically once the firmware supports USB-MSC.
- **VID `0x303A` is Espressif-generic, not open-plaud-specific.** Any ESP32 board
  shares it, so the USB match means "an Espressif device" — labeled honestly as
  "open-plaud device (ESP32)" unless the USB product string says `open-plaud`. A
  firmware that sets a distinctive product string (or a `device.json` on the SD)
  would let the app assert identity more strongly; out of scope here.
- The bundled-app distribution (`.app`, code-signing, notarization, sandbox
  entitlements) is a packaging step beyond `swift build`; the code is structured
  for it (security-scoped bookmarks are already wired) but the Xcode wrapper is
  future work. Three entitlements matter for a *sandboxed* build: a hand-picked
  folder needs `com.apple.security.files.user-selected.read-only`; reading a
  mounted SD/USB volume's contents needs
  `com.apple.security.files.removable-volumes.read-only` (or the content scan
  silently returns nothing); and — new for USB recognition — IOKit USB device
  matching needs **`com.apple.security.device.usb`**. An unsandboxed `swift run`
  binary (how you run it today) is unaffected.
- **Richer device card (volume name + MAC + firmware) needs a firmware marker.**
  The prototype's card shows the SD label, the device MAC, and a firmware hash.
  Only the **volume name** is derivable from a bare mounted card; MAC + firmware
  aren't on the SD. Surfacing them would mean the firmware dropping a small
  `device.json` identity file in the card root, which detection could then read —
  a clean future enhancement, out of scope here.
```

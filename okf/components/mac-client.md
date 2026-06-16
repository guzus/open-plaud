---
type: Component
title: Mac Client
description: Swift macOS app and CLI code for local client-side Open Plaud workflows.
resource: ../../mac-client
tags: [macos, swift, client]
timestamp: 2026-06-16T00:00:00+09:00
---
# Role

The mac client area contains a SwiftUI app, CLI target, USB/device watching code,
sync state, upload client, WAV handling, and reusable core models for desktop
workflows around Open Plaud recordings.

# Key Areas

| Path | Role |
|---|---|
| `mac-client/Sources/OpenPlaudApp/` | SwiftUI app, settings, keychain, and device watcher. |
| `mac-client/Sources/OpenPlaudCLI/` | CLI entrypoint. |
| `mac-client/Sources/OpenPlaudCore/` | Core upload, scanning, sync, server models, and WAV helpers. |
| `mac-client/Sources/OpenPlaudUSB/` | USB enumeration and watching. |

# Related Concepts

The mac client consumes the [recordings API](/apis/recordings-api.md) and should
preserve the same [recording data model](/contracts/recording-data-model.md)
shape as the server.

# Citations

[1] [Mac client README](../../mac-client/README.md)
[2] [Repository layout](../../README.md)

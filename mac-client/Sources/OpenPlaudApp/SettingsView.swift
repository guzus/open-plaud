import SwiftUI

/// Settings: server base URL (UserDefaults) and auth token (Keychain only).
/// The token field is write-only from the UI's perspective — we show whether a
/// token exists, never the token itself, and never log it.
struct SettingsView: View {
    @EnvironmentObject var settings: AppSettings
    @EnvironmentObject var model: AppModel

    var asSheet: Bool = false
    var onClose: (() -> Void)? = nil

    @State private var urlText: String = ""
    @State private var tokenText: String = ""
    @State private var savedConfirmation = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Settings")
                .font(.title2.weight(.semibold))

            VStack(alignment: .leading, spacing: 6) {
                Text("Server base URL").font(.headline)
                TextField("https://…", text: $urlText)
                    .textFieldStyle(.roundedBorder)
                Text("Where clips are uploaded (POST /v1/recordings). Stored in preferences.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button("Test Connection") {
                        settings.serverURLString = urlText.trimmingCharacters(in: .whitespacesAndNewlines)
                        model.checkHealth()
                    }
                    if let ok = model.serverReachable {
                        Label(ok ? "Reachable" : "Unreachable",
                              systemImage: ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                            .foregroundStyle(ok ? .green : .red)
                            .font(.caption)
                    }
                }
            }

            Divider()

            VStack(alignment: .leading, spacing: 6) {
                Text("Auth token").font(.headline)
                HStack {
                    SecureField(settings.hasToken ? "•••••••• (stored)" : "Bearer token", text: $tokenText)
                        .textFieldStyle(.roundedBorder)
                    if settings.hasToken {
                        Button(role: .destructive) {
                            settings.clearToken()
                            tokenText = ""
                        } label: { Text("Remove") }
                    }
                }
                Text("Sent as `Authorization: Bearer …`. Stored in the macOS Keychain — never in preferences, never logged.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Divider()

            HStack {
                if savedConfirmation {
                    Label("Saved", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green).font(.caption)
                }
                Spacer()
                if asSheet {
                    Button("Done") { save(); onClose?() }
                        .keyboardShortcut(.defaultAction)
                } else {
                    Button("Save") { save() }
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
        .padding(20)
        .onAppear {
            urlText = settings.serverURLString
        }
    }

    private func save() {
        settings.serverURLString = urlText.trimmingCharacters(in: .whitespacesAndNewlines)
        let t = tokenText.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty {
            settings.setToken(t)
            tokenText = "" // never keep the secret in view state after saving
        }
        savedConfirmation = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { savedConfirmation = false }
    }
}

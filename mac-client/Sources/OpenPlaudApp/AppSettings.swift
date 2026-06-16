import Foundation
import Combine

/// User-facing settings. The server URL lives in UserDefaults; the token lives
/// ONLY in the Keychain (never UserDefaults, never logged).
final class AppSettings: ObservableObject {
    private enum Keys {
        static let serverURL = "serverURL"
        static let folderBookmark = "folderBookmark" // security-scoped bookmark data
    }

    static let defaultServerURL = "https://openplaud.guzus.xyz"

    @Published var serverURLString: String {
        didSet { UserDefaults.standard.set(serverURLString, forKey: Keys.serverURL) }
    }

    /// Mirror of "is a token stored in Keychain"; the token value itself is never
    /// held in a published property to minimize its exposure.
    @Published var hasToken: Bool = Keychain.hasToken

    init() {
        self.serverURLString = UserDefaults.standard.string(forKey: Keys.serverURL) ?? Self.defaultServerURL
    }

    var serverURL: URL? { URL(string: serverURLString.trimmingCharacters(in: .whitespacesAndNewlines)) }

    func setToken(_ token: String) {
        let trimmed = token.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            Keychain.deleteToken()
        } else {
            Keychain.saveToken(trimmed)
        }
        hasToken = Keychain.hasToken
    }

    func clearToken() {
        Keychain.deleteToken()
        hasToken = false
    }

    /// Read the token from Keychain only at the moment it's needed for a request.
    func currentToken() -> String? { Keychain.loadToken() }

    // MARK: - Security-scoped folder bookmark (so a sandboxed app keeps access)

    func saveFolderBookmark(_ url: URL) {
        if let data = try? url.bookmarkData(options: [.withSecurityScope], includingResourceValuesForKeys: nil, relativeTo: nil) {
            UserDefaults.standard.set(data, forKey: Keys.folderBookmark)
        }
    }

    func resolveFolderBookmark() -> URL? {
        guard let data = UserDefaults.standard.data(forKey: Keys.folderBookmark) else { return nil }
        var stale = false
        let url = try? URL(resolvingBookmarkData: data, options: [.withSecurityScope], relativeTo: nil, bookmarkDataIsStale: &stale)
        return url
    }
}

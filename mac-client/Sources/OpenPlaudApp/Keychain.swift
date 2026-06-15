import Foundation
import Security

/// Minimal Keychain wrapper for the auth token. The token is stored ONLY here —
/// never in UserDefaults, never in source, never logged. Generic-password item.
///
/// Note: reliable Keychain access requires a signed/bundled app; an unsigned
/// `swift run` binary may be unable to read/write the login keychain. The GUI
/// uses this; the CLI deliberately reads the token from a flag/env instead.
enum Keychain {
    private static let service = "xyz.openplaud.mac-client"
    private static let account = "ingest-auth-token"

    static func saveToken(_ token: String) {
        let data = Data(token.utf8)
        // Delete any existing item first, then add fresh.
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)

        var add = query
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlocked
        SecItemAdd(add as CFDictionary, nil)
    }

    static func loadToken() -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func deleteToken() {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)
    }

    static var hasToken: Bool { loadToken()?.isEmpty == false }
}

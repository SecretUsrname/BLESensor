import CryptoKit
import Foundation
import Security

/// The phone's lock key: P-256 in the Secure Enclave, so the private key never leaves the chip.
///
/// Created with "after first unlock" access, NOT CryptoKit's default "when unlocked". With the
/// default, signing fails while the phone is locked in a pocket, which would break arrival unlocks.
/// The simulator has no Secure Enclave, so it falls back to a software key there.
final class KeyManager {
    static let shared = KeyManager()

    enum KeyError: Error {
        case accessControl
        case keychain(OSStatus)
    }

    private let service = "dev.slock.app"
    private let account = "lock-signing-key"
    private var seKey: SecureEnclave.P256.Signing.PrivateKey?
    private var swKey: P256.Signing.PrivateKey?

    var usesSecureEnclave: Bool { SecureEnclave.isAvailable }

    /// 64 bytes: x | y, the format the lock stores.
    func publicKeyRaw() throws -> Data {
        try loadOrCreate()
        if let k = seKey { return k.publicKey.rawRepresentation }
        return swKey!.publicKey.rawRepresentation
    }

    /// ECDSA P-256 over SHA-256(message), 64 bytes: r | s.
    func sign(_ message: Data) throws -> Data {
        try loadOrCreate()
        if let k = seKey { return try k.signature(for: message).rawRepresentation }
        return try swKey!.signature(for: message).rawRepresentation
    }

    func reset() {
        deleteFromKeychain()
        seKey = nil
        swKey = nil
    }

    private func loadOrCreate() throws {
        if seKey != nil || swKey != nil { return }
        if let blob = readFromKeychain() {
            if SecureEnclave.isAvailable {
                seKey = try SecureEnclave.P256.Signing.PrivateKey(dataRepresentation: blob)
            } else {
                swKey = try P256.Signing.PrivateKey(rawRepresentation: blob)
            }
            return
        }
        if SecureEnclave.isAvailable {
            var error: Unmanaged<CFError>?
            guard let access = SecAccessControlCreateWithFlags(
                nil, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly, .privateKeyUsage, &error
            ) else { throw KeyError.accessControl }
            let key = try SecureEnclave.P256.Signing.PrivateKey(accessControl: access)
            try writeToKeychain(key.dataRepresentation)  // an encrypted handle, only usable on this device
            seKey = key
        } else {
            let key = P256.Signing.PrivateKey()
            try writeToKeychain(key.rawRepresentation)
            swKey = key
        }
    }

    // MARK: Keychain

    private var baseQuery: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    private func readFromKeychain() -> Data? {
        var query = baseQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: AnyObject?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        return result as? Data
    }

    private func writeToKeychain(_ data: Data) throws {
        deleteFromKeychain()
        var query = baseQuery
        query[kSecValueData as String] = data
        query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(query as CFDictionary, nil)
        guard status == errSecSuccess else { throw KeyError.keychain(status) }
    }

    private func deleteFromKeychain() {
        SecItemDelete(baseQuery as CFDictionary)
    }
}

import Foundation

enum GuestSwap {
    static let enabledPreferenceKey = "guestSwapEnabled"
    private static var didClearStaleFiles = false

    // Cemu keeps the guest address-space pointer for the life of the app.
    private static var backing: FileBackedGuestMemory?

    static func clearOnLaunch() {
        do {
            try FileBackedGuestMemory.removeStaleFiles()
            didClearStaleFiles = true
        } catch {
            print("Could not clear stale swap files. \(error.localizedDescription)")
        }
    }

    static func prepare() async -> Bool {
        guard didClearStaleFiles, UserDefaults.standard.bool(forKey: enabledPreferenceKey) else { return false }
        if backing != nil { return true }

        do {
            let mapping = try await Task.detached(priority: .userInitiated) {
                try FileBackedGuestMemory(size: 4 * 1024 * 1024 * 1024)
            }.value
            guard offer_donated_memory(mapping.base, UInt64(mapping.size)) else {
                print("Cemu rejected the file-backed guest memory.")
                return false
            }
            backing = mapping
            print("File-backed guest RAM: 4 GiB")
            return true
        } catch {
            print("Could not set up file-backed guest memory. \(error.localizedDescription)")
            return false
        }
    }
}

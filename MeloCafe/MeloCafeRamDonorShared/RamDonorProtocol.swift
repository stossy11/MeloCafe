import Foundation

enum RamDonorProtocol {
    static let chunkSize = 256 * 1024 * 1024
}

struct RamDonorError: LocalizedError {
    let message: String

    init(_ message: String) {
        self.message = message
    }

    var errorDescription: String? { message }
}

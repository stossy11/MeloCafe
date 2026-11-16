import Darwin
import Foundation

nonisolated final class FileBackedGuestMemory: Sendable {
    private static let filePrefix = "MeloCafe-swap."
    private let address: UInt
    let size: Int

    var base: UnsafeMutableRawPointer {
        UnsafeMutableRawPointer(bitPattern: address)!
    }

    init(size: Int, directory: URL = FileManager.default.temporaryDirectory) throws {
        guard size > 0 else { throw POSIXError(.EINVAL) }

        var path = Array(directory.appendingPathComponent(Self.filePrefix + "XXXXXX").path.utf8CString)
        let fd = mkstemp(&path)
        guard fd >= 0 else { throw Self.systemError() }
        defer { close(fd) }

        // The descriptor and mapping keep the file alive after unlinking it.
        guard unlink(path) == 0 else { throw Self.systemError() }

        // Reserve disk blocks up front so a sparse file cannot run out of space during play.
        var allocation = fstore_t(
            fst_flags: UInt32(F_ALLOCATEALL),
            fst_posmode: F_PEOFPOSMODE,
            fst_offset: 0,
            fst_length: off_t(size),
            fst_bytesalloc: 0
        )
        guard fcntl(fd, F_PREALLOCATE, &allocation) == 0 else { throw Self.systemError() }
        guard allocation.fst_bytesalloc >= off_t(size) else { throw POSIXError(.ENOSPC) }
        guard ftruncate(fd, off_t(size)) == 0 else { throw Self.systemError() }

        let mapping = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
        guard let mapping, mapping != MAP_FAILED else { throw Self.systemError() }
        address = UInt(bitPattern: mapping)
        self.size = size
    }

    deinit {
        munmap(base, size)
    }

    static func removeStaleFiles(in directory: URL = FileManager.default.temporaryDirectory) throws {
        let names = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        for name in names where name.hasPrefix(filePrefix) {
            let suffix = name.dropFirst(filePrefix.count)
            guard suffix.count == 6, suffix.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber) }) else {
                continue
            }
            let path = directory.appendingPathComponent(name).path
            if unlink(path) != 0, errno != ENOENT {
                throw systemError()
            }
        }
    }

    private static func systemError() -> POSIXError {
        POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
}

import Darwin
import Foundation
import XCTest

#if os(iOS)
@testable import MeloCafe
#endif

final class FileBackedGuestMemoryTests: XCTestCase {
    func testLaunchCleanupRemovesOnlyStaleSwapFiles() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }

        let remaining = ["unrelated.bin", "MeloCafe-swap.backup.bin"]
        for name in ["MeloCafe-swap.A1b2C3"] + remaining {
            try Data([0xA5]).write(to: directory.appendingPathComponent(name))
        }

        try FileBackedGuestMemory.removeStaleFiles(in: directory)
        try FileBackedGuestMemory.removeStaleFiles(in: directory)
        XCTAssertEqual(Set(try FileManager.default.contentsOfDirectory(atPath: directory.path)), Set(remaining))
    }

    func testWritableFileMappingDoesNotChargeItsPagesToFootprint() throws {
        let mebibyte = 1024 * 1024
        let size = 64 * mebibyte
        // Page tables and test-process bookkeeping can still contribute to the footprint.
        let allowance: Int64 = Int64(2 * mebibyte)

        let fileBaseline = try footprint()
        let file = try FileBackedGuestMemory(size: size)
        writeEveryPage(file.base, size: size)
        // Measure before flushing or unmapping. The emulator writes to the live mapping.
        let fileIncrease = try footprint() - fileBaseline
        XCTAssertTrue(everyPageHasMarker(file.base, size: size))
        XCTAssertLessThanOrEqual(fileIncrease, allowance, "File-backed footprint increased by \(fileIncrease) bytes")

        let anonymous = try XCTUnwrap(mmap(nil, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0))
        guard anonymous != MAP_FAILED else { throw POSIXError(.ENOMEM) }
        defer { munmap(anonymous, size) }

        let anonymousBaseline = try footprint()
        writeEveryPage(anonymous, size: size)
        let anonymousIncrease = try footprint() - anonymousBaseline
        XCTAssertTrue(everyPageHasMarker(anonymous, size: size))
        XCTAssertGreaterThanOrEqual(anonymousIncrease, Int64(48 * mebibyte), "Anonymous control increased by \(anonymousIncrease) bytes")
        print("phys_footprint deltas: file-backed \(fileIncrease) bytes, anonymous \(anonymousIncrease) bytes")
    }

    func testMappingIsZeroInitializedAndRetainsWritesAcrossProtectionChanges() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }

        let size = Int(vm_page_size)
        let file = try FileBackedGuestMemory(size: size, directory: directory)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty)
        XCTAssertTrue(UnsafeRawBufferPointer(start: file.base, count: size).allSatisfy { $0 == 0 })

        writeEveryPage(file.base, size: size)
        guard mprotect(file.base, size, PROT_NONE) == 0,
              mprotect(file.base, size, PROT_READ | PROT_WRITE) == 0 else {
            return XCTFail("Could not change protection on the file mapping")
        }
        XCTAssertTrue(everyPageHasMarker(file.base, size: size))
    }

    private func writeEveryPage(_ base: UnsafeMutableRawPointer, size: Int) {
        for offset in stride(from: 0, to: size, by: Int(vm_page_size)) {
            base.storeBytes(of: UInt8(0xA5), toByteOffset: offset, as: UInt8.self)
        }
    }

    private func everyPageHasMarker(_ base: UnsafeRawPointer, size: Int) -> Bool {
        stride(from: 0, to: size, by: Int(vm_page_size)).allSatisfy {
            base.load(fromByteOffset: $0, as: UInt8.self) == 0xA5
        }
    }

    private func footprint() throws -> Int64 {
        var info = task_vm_info_data_t()
        let capacity = MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size
        var count = mach_msg_type_number_t(capacity)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: capacity) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS else {
            throw NSError(domain: NSMachErrorDomain, code: Int(result))
        }
        return Int64(info.phys_footprint)
    }
}

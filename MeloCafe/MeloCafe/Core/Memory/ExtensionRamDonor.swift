import Darwin
import ExtensionFoundation
import Foundation
import XPC

@available(iOS 26.0, *)
extension AppExtensionPoint {
    @Definition
    static var ramDonor: AppExtensionPoint {
        Name("RamDonor")
        EnhancedSecurity()
    }
}

@available(iOS 26.0, *)
enum ExtensionRamDonor {
    static func acquire(size: Int) async throws -> DonatedRam {
        let chunkSize = RamDonorProtocol.chunkSize
        guard size > 0, size.isMultiple(of: chunkSize) else {
            throw RamDonorError("The requested size must be a multiple of the donor chunk size.")
        }

        let monitor = try await AppExtensionPoint.Monitor(appExtensionPoint: .ramDonor)
        guard let identity = monitor.identities.first else {
            throw RamDonorError("The memory helper is unavailable.")
        }

        let donation = try DonatedRam(size: size)
        var usedPIDs = Set<pid_t>()
        let chunkCount = size / chunkSize
        for index in 0..<chunkCount {
            let target = donation.address + vm_address_t(index * chunkSize)
            do {
                let (pid, region) = try await acquireChunk(at: target, identity: identity, usedPIDs: usedPIDs)
                usedPIDs.insert(pid)
                donation.regions.append(region)
            } catch {
                throw RamDonorError("Chunk \(index + 1) of \(chunkCount) failed. \(error.localizedDescription)")
            }
        }
        return donation
    }

    private static func acquireChunk(
        at target: vm_address_t,
        identity: AppExtensionIdentity,
        usedPIDs: Set<pid_t>
    ) async throws -> (pid_t, xpc_object_t) {
        for attempt in 0..<10 {
            if attempt > 0 {
                try await Task.sleep(for: .milliseconds(600))
            }

            let process = try await AppExtensionProcess(configuration: .init(appExtensionIdentity: identity))
            defer { process.invalidate() }

            let session = try process.makeXPCSession()
            try session.activate()
            defer { session.cancel(reason: "RAM donation complete") }

            let info = try await request("identify", on: session)
            guard let pid = info["pid", as: pid_t.self], pid > 0 else {
                throw RamDonorError("The memory helper did not report its PID.")
            }
            guard !usedPIDs.contains(pid) else { continue }

            let reply = try await request("allocate", on: session)
            guard let region = reply["region", as: XPC_TYPE_SHMEM] else {
                throw RamDonorError("The memory helper returned no shared memory.")
            }
            try map(region, at: target, size: RamDonorProtocol.chunkSize)

            do {
                _ = try await request("exit", on: session)
                throw RamDonorError("The memory helper replied without exiting.")
            } catch is XPCRichError {
                // The helper exits without replying. The next cycle requires a fresh PID.
            }
            return (pid, region)
        }
        throw RamDonorError("The system kept reusing a previous memory helper PID.")
    }

    private static func request(_ operation: String, on session: XPCSession) async throws -> XPCDictionary {
        var message = XPCDictionary()
        message["operation"] = operation

        let replies = AsyncThrowingStream<XPCDictionary, Error> { continuation in
            let timeout = Task {
                do {
                    try await Task.sleep(for: .seconds(5))
                } catch {
                    return
                }
                continuation.finish(throwing: RamDonorError("The helper timed out during \(operation)."))
            }
            continuation.onTermination = { _ in timeout.cancel() }
            session.send(message: message) { result in
                switch result {
                case .success(let reply):
                    continuation.yield(reply)
                    continuation.finish()
                case .failure(let error):
                    continuation.finish(throwing: error)
                }
            }
        }
        var repliesIterator = replies.makeAsyncIterator()
        guard let reply = try await repliesIterator.next() else {
            throw RamDonorError("The helper returned no reply.")
        }
        if let error = reply["error", as: String.self] {
            throw RamDonorError(error)
        }
        return reply
    }

    private static func map(_ region: xpc_object_t, at target: vm_address_t, size: Int) throws {
        var scratch: UnsafeMutableRawPointer?
        let mapped = xpc_shmem_map(region, &scratch)
        guard let scratch, mapped > 0 else {
            throw RamDonorError("xpc_shmem_map failed.")
        }
        defer { munmap(scratch, mapped) }
        guard mapped == size else {
            throw RamDonorError("The helper mapped \(mapped) bytes instead of \(size).")
        }

        var address = target
        var protection: vm_prot_t = 0
        var maximum: vm_prot_t = 0
        let result = vm_remap(
            mach_task_self_, &address, vm_size_t(size), 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
            mach_task_self_, vm_address_t(UInt(bitPattern: scratch)), 0,
            &protection, &maximum, VM_INHERIT_SHARE
        )
        guard result == KERN_SUCCESS else {
            throw RamDonorError("vm_remap failed (\(result)).")
        }
        guard address == target, protection & VM_PROT_WRITE != 0, protection & VM_PROT_READ != 0 else {
            throw RamDonorError("The remapped chunk isn't readable and writable at the requested address.")
        }
    }
}

final class DonatedRam {
    let address: vm_address_t
    private let size: vm_size_t
    fileprivate var regions: [xpc_object_t] = []

    var base: UnsafeMutableRawPointer {
        UnsafeMutableRawPointer(bitPattern: UInt(address))!
    }

    init(size: Int) throws {
        self.size = vm_size_t(size)
        var base: vm_address_t = 0
        let result = vm_allocate(mach_task_self_, &base, self.size, VM_FLAGS_ANYWHERE)
        guard result == KERN_SUCCESS else {
            throw RamDonorError("Could not reserve guest address space (\(result)).")
        }
        address = base
    }

    deinit {
        vm_deallocate(mach_task_self_, address, size)
    }
}

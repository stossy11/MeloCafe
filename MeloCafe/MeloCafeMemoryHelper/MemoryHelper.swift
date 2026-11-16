import Darwin
import ExtensionFoundation
import Synchronization
import XPC

@main
struct MemoryHelper: AppExtension {
    @AppExtensionPoint.Bind
    var boundExtensionPoint: AppExtensionPoint {
        AppExtensionPoint.Identifier(host: "com.stossy11.MeloCafe", name: "RamDonor")
    }

    var configuration: some AppExtensionConfiguration {
        ConnectionHandler(onSessionRequest: { request in
            request.accept { _ in RamDonorPeer() }
        })
    }
}

private final class RamDonorPeer: XPCPeerHandler {
    private let allocation = Mutex<vm_address_t?>(nil)

    deinit {
        allocation.withLock { address in
            if let address {
                vm_deallocate(mach_task_self_, address, vm_size_t(RamDonorProtocol.chunkSize))
            }
        }
    }

    func handleIncomingRequest(_ message: XPCDictionary) -> XPCDictionary? {
        var reply = XPCDictionary()
        do {
            switch message["operation", as: String.self] {
            case "identify":
                reply["pid"] = getpid()
            case "allocate":
                reply["region"] = try allocateRegion()
            case "exit":
                guard allocation.withLock({ $0 != nil }) else {
                    throw RamDonorError("The helper has no donated chunk to hand off.")
                }
                exit(0)
            default:
                throw RamDonorError("Unknown helper operation.")
            }
        } catch {
            reply["error"] = error.localizedDescription
        }
        return reply
    }

    private func allocateRegion() throws -> xpc_object_t {
        try allocation.withLock { address in
            guard address == nil else {
                throw RamDonorError("This helper session already donated a chunk.")
            }
            let size = vm_size_t(RamDonorProtocol.chunkSize)
            var base: vm_address_t = 0
            let result = vm_allocate(mach_task_self_, &base, size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE)
            guard result == KERN_SUCCESS else {
                throw RamDonorError("The helper could not allocate memory (\(result)).")
            }
            guard let region = mlx_create_shared_memory(UnsafeMutableRawPointer(bitPattern: UInt(base))!, Int(size)) else {
                vm_deallocate(mach_task_self_, base, size)
                throw RamDonorError("xpc_shmem_create rejected the purgeable allocation.")
            }
            address = base
            return region
        }
    }
}

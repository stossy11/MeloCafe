import Foundation

@objc(JITRequestHandler)
final class JITRequestHandler: NSObject, NSExtensionRequestHandling, MCJITService {
    private let queue = DispatchQueue(label: "cafe.melo.jit", qos: .userInitiated)
    private var connection: NSXPCConnection?
    private var context: NSExtensionContext?

    func beginRequest(with context: NSExtensionContext) {
        guard let item = context.inputItems.first as? NSExtensionItem,
              let endpoint = item.userInfo?[MCJITEndpointKey] as? NSXPCListenerEndpoint else {
            context.cancelRequest(withError: NSError(domain: "MeloCafe.JIT", code: 1))
            return
        }
        self.context = context
        let connection = NSXPCConnection(listenerEndpoint: endpoint)
        connection.exportedInterface = NSXPCInterface(with: MCJITService.self)
        connection.exportedObject = self
        connection.remoteObjectInterface = NSXPCInterface(with: MCJITProgress.self)
        connection.resume()
        self.connection = connection
        // XPC connects lazily on the first message. The host cannot send its
        // request until its listener accepts us, so initiate that handshake here.
        let host = connection.remoteObjectProxyWithErrorHandler { error in
            print("[JIT] Could not connect to host: \(error.localizedDescription)")
        } as? MCJITProgress
        host?.updateStatus("JIT helper connected")
    }

    func enableJIT(forPID pid: Int32, pairingData data: Data, forceScript: Bool) {
        guard let host = connection?.remoteObjectProxy as? MCJITProgress else {
            return
        }
        queue.async {
            let pairingURL = FileManager.default.temporaryDirectory.appendingPathComponent("pairing-\(UUID()).plist")
            defer {
                try? FileManager.default.removeItem(at: pairingURL)
            }
            do {
                try data.write(to: pairingURL, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
                let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
                let directory = library.appendingPathComponent("StikJIT", isDirectory: true)
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                var cache = directory
                var values = URLResourceValues()
                values.isExcludedFromBackup = true
                try cache.setResourceValues(values)
                try StikJIT.enableJIT(
                    targetPID: pid,
                    pairingFile: pairingURL,
                    ddiPaths: .default(in: directory),
                    script: .universal,
                    forceScript: forceScript,
                    preparationProgress: { stage in
                        switch stage {
                        case .checkingReachability:
                            host.updateStatus("Connecting to device…")
                        case .checkingDDI:
                            host.updateStatus("Checking developer disk image…")
                        case .downloadingDDI(let fraction, _):
                            host.updateStatus("Downloading developer disk image: \(Int(fraction * 100))%")
                        case .mountingDDI(let fraction):
                            host.updateStatus("Mounting developer disk image: \(Int(fraction * 100))%")
                        case .verifyingDDI:
                            host.updateStatus("Verifying developer disk image…")
                        case .ready:
                            host.updateStatus("Attaching debugger…")
                        }
                    },
                    progress: { line in
                        if line.hasPrefix("Running ") {
                            print("[JIT helper] Starting debugger script")
                        }
                        // The universal script remains in its breakpoint loop until
                        // Cemu initializes its arena and sends CMD_DETACH.
                        if line.hasPrefix("attach_response = T") {
                            print("[JIT helper] Debugger attached")
                            host.debuggerAttached()
                        }
                    })
                host.finishedWithError(nil)
            } catch {
                host.finishedWithError(error.localizedDescription)
            }
        }
    }
}

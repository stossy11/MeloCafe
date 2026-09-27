//
//  CemuManager.swift
//  MeloCafe
//
//  Created by Stossy11 on 5/3/2026.
//

import Foundation
import Combine

@MainActor
class CemuManager: ObservableObject {
    static let shared = CemuManager()
    private static let fileManager = FileManager.default

    @Published var autoEnableJIT: Bool {
        didSet {
            UserDefaults.standard.set(autoEnableJIT, forKey: "autoEnableJIT")
        }
    }
    @Published private(set) var pairingFileName: String?
    @Published private(set) var jitStatus = "Not enabled"
    @Published private(set) var isEnablingJIT = false
    @Published private(set) var jitError: String?

    private let pairingDirectory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("JIT", isDirectory: true)
    private var pairingFile: URL { pairingDirectory.appendingPathComponent("pairing.plist") }
    private var pairingNameFile: URL { pairingDirectory.appendingPathComponent("filename.txt") }
    private var jitOperation: Task<Void, Error>?
    private var jitHelper: JITHelperClient?
    private var jitCompletion: CheckedContinuation<Void, Error>?
    private var jitTimeout: Task<Void, Never>?
    private var initializingMemory = false
    private var memoryReady = false
    private var helperFinished = false
    private var lastHelperStop: Date?

    private init() {
        autoEnableJIT = UserDefaults.standard.bool(forKey: "autoEnableJIT")
        if Self.fileManager.fileExists(atPath: pairingFile.path) {
            pairingFileName = (try? String(contentsOf: pairingNameFile, encoding: .utf8)) ?? "Pairing File"
        }
    }

    static func initialize() {
        if !fileManager.fileExists(atPath: URL.romsURL.path) {
            do {
                try fileManager.createDirectory(at: .romsURL, withIntermediateDirectories: true, attributes: nil)
            } catch {
                print("Failed to create directory: \(URL.romsURL.path), error: \(error)")
            }
        }

        let documents = URL.documentsDirectory

        let executable = Bundle.main.executablePath

        documents.path.withCString { dataPtr in
            executable!.withCString { execPtr in
                CemuInitialize(execPtr, dataPtr, dataPtr, dataPtr, dataPtr)
            }
        }
    }

    static func loadGameIcon(_ titleId: UInt64) -> Data? {
        var iconResult = CemuGetGameIcon(titleId)

        guard let iconData = iconResult.data, iconResult.size > 0 else {
            return nil
        }

        let data = Data(bytes: iconData, count: Int(iconResult.size))

        Cemu_FreeGameIcon(&iconResult)

        return data
    }

    static func loadTitle(titleId: UInt64) -> Bool {
        return CemuLoadTitle(titleId)
    }

    static func load(path: String, load: Bool = false) -> UInt64 {
        return path.withCString { ptr in
            return CemuLoadFile(ptr, load)
        }
    }

    static func run() {
        ControllerManager.shared.isRunning = true
        ControllerManager.shared.attachAllToCore()


        Thread.detachNewThread {
            CemuRun()
        }
    }

    static func shutdown() {
        ControllerManager.shared.isRunning = false
        ControllerManager.shared.detachAllFromCore()
        CemuShutdown()
    }

    var jitUnavailableReason: String? {
        #if targetEnvironment(simulator)
        return "Automatic JIT requires a physical iPhone or iPad."
        #else
        guard #available(iOS 17.4, *) else {
            return "Automatic JIT requires iOS 17.4 or later."
        }
        if ProcessInfo.processInfo.isiOSAppOnMac {
            return "Automatic JIT is unavailable on Mac."
        }
        if getenv("LC_HOME_PATH") != nil {
            return "Automatic JIT is unavailable inside LiveContainer. Use an external debugger."
        }
        if !checkAppEntitlement("get-task-allow") {
            return "Reinstall MeloCafe with a signing method that enables debugging (get-task-allow)."
        }
        return nil
        #endif
    }

    func importPairingFile(_ url: URL) throws {
        let access = url.startAccessingSecurityScopedResource()
        defer {
            if access {
                url.stopAccessingSecurityScopedResource()
            }
        }

        // Validate before replacing a working pairing record.
        let data = try validatePairingData(Data(contentsOf: url))
        try Self.fileManager.createDirectory(at: pairingDirectory, withIntermediateDirectories: true)
        var directory = pairingDirectory
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try directory.setResourceValues(values)
        try data.write(to: pairingFile, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        try url.lastPathComponent.write(to: pairingNameFile, atomically: true, encoding: .utf8)
        pairingFileName = url.lastPathComponent
        jitError = nil
        jitStatus = "Pairing file imported"
    }

    private func validatePairingData(_ data: Data) throws -> Data {
        guard !data.isEmpty, data.count <= 1_048_576,
              let record = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let identifier = record["identifier"] as? String, !identifier.isEmpty,
              let publicKey = record["public_key"] as? Data, publicKey.count == 32,
              let privateKey = record["private_key"] as? Data, privateKey.count == 32 else {
            throw jitFailure("Select a StikJIT-compatible remote pairing .plist file for this device. This file is not a valid remote pairing record.")
        }
        return data
    }

    func enableJITOnLaunchIfNeeded() async {
        guard autoEnableJIT else {
            return
        }
        // enableJIT keeps errors visible in Settings.
        try? await enableJIT()
    }

    func prepareJITForGame() async throws {
        if let jitOperation {
            try await jitOperation.value
        }
        if autoEnableJIT {
            try await enableJIT()
        }
    }

    func enableJIT() async throws {
        if let jitOperation {
            return try await jitOperation.value
        }
        guard !initializingMemory else {
            throw jitFailure("The previous JIT memory request is still finishing. Wait before retrying, or restart MeloCafe.")
        }
        guard !GamesManager.shared.startedEmulation else {
            throw jitFailure("Stop the game before enabling JIT.")
        }
        jitError = nil
        isEnablingJIT = true
        let task = Task { try await self.performEnableJIT() }
        jitOperation = task
        defer {
            jitOperation = nil
            isEnablingJIT = false
        }
        do {
            try await task.value
            jitStatus = "JIT enabled"
            print("[JIT] JIT enabled; Cemu memory initialization completed")
        } catch {
            jitError = error.localizedDescription
            jitStatus = "JIT could not be enabled"
            print("[JIT] Activation failed: \(error.localizedDescription)")
            throw error
        }
    }

    private var needsScript: Bool {
        guard let value = getenv("HAS_TXM") else {
            return false
        }
        return String(cString: value) == "1"
    }

    private func performEnableJIT() async throws {
        // Do not attach a second debugger over Xcode/StikDebug. An existing Cemu
        // arena remains usable after the universal script has detached.
        if CemuIsJITInitialized() {
            return
        }
        if MCJITIsTraced() || (MCJITIsDebugged() && !needsScript) {
            jitStatus = "Initializing JIT memory…"
            guard await Task.detached(operation: { CemuInitJIT() }).value else {
                throw jitFailure("The debugger is attached, but JIT memory could not be initialized.")
            }
            return
        }
        if let reason = jitUnavailableReason {
            throw jitFailure(reason)
        }
        guard pairingFileName != nil else {
            throw jitFailure("Select a pairing file in Settings first.")
        }
        let data = try validatePairingData(Data(contentsOf: pairingFile))
        // iOS needs a short interval between terminating and relaunching an extension.
        if let lastHelperStop {
            let remaining = 1 - Date().timeIntervalSince(lastHelperStop)
            if remaining > 0 {
                try await Task.sleep(nanoseconds: UInt64(remaining * 1_000_000_000))
            }
        }
        jitStatus = "Starting JIT helper…"
        memoryReady = false
        helperFinished = false
        initializingMemory = false
        try await withCheckedThrowingContinuation { continuation in
            jitCompletion = continuation
            let client = JITHelperClient()
            jitHelper = client
            client.start(withPairingData: data, forceScript: needsScript, status: { [weak self] status in
                self?.jitStatus = status
                print("[JIT] \(status)")
            }, attached: { [weak self] in
                // The helper's device probe can detect TXM even when the host's
                // launch-time heuristic did not. Match the active universal script.
                setenv("DUAL_MAPPED_JIT", "1", 1)
                setenv("HAS_TXM", "1", 1)
                self?.initializeJITMemory()
            }, finished: { [weak self] error in
                guard let self, self.jitCompletion != nil else {
                    return
                }
                if let error {
                    self.finishJIT(error: self.jitFailure(error))
                } else {
                    self.helperFinished = true
                    self.initializeJITMemory()
                    self.finishJITIfReady()
                }
            })
            jitTimeout = Task { [weak self] in
                do {
                    try await Task.sleep(nanoseconds: 300_000_000_000)
                } catch {
                    return
                }
                guard let self else {
                    return
                }
                self.finishJIT(error: self.jitFailure("JIT activation timed out. Check LocalDevVPN and your pairing file, then try again."))
            }
        }
    }

    private func initializeJITMemory() {
        guard jitCompletion != nil, !initializingMemory, !memoryReady else {
            return
        }
        guard MCJITIsDebugged() else {
            finishJIT(error: jitFailure("The helper finished without enabling debugging for MeloCafe."))
            return
        }
        initializingMemory = true
        jitStatus = "Initializing JIT memory…"
        Task {
            let success = await Task.detached { CemuInitJIT() }.value
            initializingMemory = false
            guard jitCompletion != nil else {
                return
            }
            if success {
                memoryReady = true
                finishJITIfReady()
            } else {
                finishJIT(error: jitFailure("JIT memory initialization failed. Reconnect LocalDevVPN and try again."))
            }
        }
    }

    private func finishJITIfReady() {
        if memoryReady && helperFinished {
            finishJIT(error: nil)
        }
    }

    private func finishJIT(error: Error?) {
        guard let continuation = jitCompletion else {
            return
        }
        jitCompletion = nil
        jitTimeout?.cancel()
        jitTimeout = nil
        jitHelper?.invalidate()
        jitHelper = nil
        lastHelperStop = Date()
        if let error {
            continuation.resume(throwing: error)
        } else {
            continuation.resume()
        }
    }

    private func jitFailure(_ message: String) -> NSError {
        NSError(domain: "MeloCafe.JIT", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }
}

extension FileManager {
    func filePath(atPath path: String, withLength length: Int) -> String? {
        guard let file = try? contentsOfDirectory(atPath: path).filter({ $0.count == length }).first else { return nil }
        return "\(path)/\(file)"
    }
}

extension URL {
    @available(iOS, introduced: 14.0, deprecated: 16.0, message: "Use URL.documentsDirectory on iOS 16 and above")
    static var documentsDirectory: URL {
        let documentDirectory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        return documentDirectory
    }
}

enum DeviceCpu {
    case mseries
    case aseries
}

struct ChipInfo {
    let series: DeviceCpu
    let number: Int
}

public extension ProcessInfo {
    var hasTXMClassic: Bool {
        ProcessInfo.processInfo.isiOSAppOnMac ? false :
        { if let boot = FileManager.default.filePath(atPath: "/System/Volumes/Preboot", withLength: 36), let file = FileManager.default.filePath(atPath: "\(boot)/boot", withLength: 96) { return access("\(file)/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4", F_OK) == 0 } else { return (FileManager.default.filePath(atPath: "/private/preboot", withLength: 96).map { access("\($0)/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4", F_OK) == 0 }) ?? false } }()
    }
    
    var hasTXM: Bool {
        if #available(iOS 27, *) {
            let lastNonTXM = 12 // A12
            let chipInfo = parseChipInfo()
            
            if let info = chipInfo, info.series == .aseries {
                return info.number > lastNonTXM
            }
            
            return true
        }
        
        if #available(iOS 26.6, *), !hasTXMClassic {
            let firstTXM = 15 // A15
            let iPadTXM = 2 // M2
            let chipInfo = parseChipInfo()
            
            if let info = chipInfo {
                if info.series == .mseries {
                    return info.number >= iPadTXM
                } else {
                    return info.number >= firstTXM
                }
            }
            
            return false
        }
        
        return hasTXMClassic
    }
    
    private func parseChipInfo() -> ChipInfo? {
        guard let device = MTLCreateSystemDefaultDevice() else { return nil }
        
        let name = device.name.uppercased()
        
        let pattern = "APPLE\\s+([MA])(\\d+)"
        guard let regex = try? NSRegularExpression(pattern: pattern),
            let match = regex.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)),
            let letterRange = Range(match.range(at: 1), in: name),
            let numberRange = Range(match.range(at: 2), in: name),
            let number = Int(name[numberRange])
        else { return nil }
        
        switch name[letterRange] {
        case "M":
            return ChipInfo(series: .mseries, number: number)
        case "A":
            return ChipInfo(series: .aseries, number: number)
        default:
            return nil
        }
    }
}


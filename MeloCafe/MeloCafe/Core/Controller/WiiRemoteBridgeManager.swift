import Combine
import CoreBluetooth
import Foundation
import NintendoBLE
import UIKit

/// The BLE connection outlives Settings and games. Cemu owns Wii initialization,
/// decoding and output; this object only transports complete HID reports.
@MainActor
final class WiiRemoteBridgeManager: ObservableObject {
    static let shared = WiiRemoteBridgeManager()

    struct SavedBridge: Codable, Identifiable, Equatable {
        let id: UUID
        var name: String
    }
    @Published private(set) var candidates: [ControllerCandidate] = []
    @Published private(set) var selected: ControllerCandidate?
    @Published private(set) var connection: ControllerConnectionState = .disconnected
    @Published private(set) var bluetoothState: CBManagerState = .unknown
    @Published private(set) var isSearching = false
    @Published private(set) var pairingOpen = false
    @Published private(set) var connectedSlots: [UInt8] = []
    @Published private(set) var features: [UInt8: UInt8] = [:]
    @Published private(set) var errorMessage: String?
    @Published private(set) var savedBridges: [SavedBridge] = []
    @Published private(set) var remoteInfo: [UInt8: BridgeDeviceInfo] = [:]
    @Published private(set) var managementSupported = false
    @Published private(set) var remoteControlSupported = false
    @Published private(set) var playerOrder: [UInt8] = [0, 1, 2, 3]
    @Published private(set) var disconnectedRemoteSlots = Set<UInt8>()
    private var pausedOutputSlots = Set<UInt8>()
    @Published private(set) var isManaging = false
    @Published private(set) var connectionDetail = "Not connected"
    @Published private(set) var diagnostics: [String] = []
    @Published var autoReconnect: Bool = true {
        didSet {
            UserDefaults.standard.set(autoReconnect, forKey: "WiiBridgeAutoReconnect")
            if autoReconnect { reconnectPaused = false; scheduleReconnect() }
            else { retryTask?.cancel(); client.stopDiscovery(); isSearching = false }
        }
    }

    private let client = NintendoControllerManager(options: .init(sources: [.wiiBridge]))
    private var handles: [UInt8: UInt64] = [:]
    private var outputTimer: Timer?
    private var observers: [NSObjectProtocol] = []
    private var retryTask: Task<Void, Never>?
    private var managementTask: Task<Void, Never>?
    private lazy var metadata = WiiBridgeMetadataRefresh(read: { [weak self] item in
        guard let self else { throw CancellationError() }
        try await self.readMetadata(item)
    }, failed: { [weak self] item, error in
        self?.log("Background metadata \(item): \(error.localizedDescription)")
    })
    private var retryCount = 0
    private var reconnectPaused = false
    private var inForeground = true
    private var manualSearch = false
    private var intentionalDisconnect = false
    private var connectionGeneration = 0

    private init() {
        let defaults = UserDefaults.standard
        if let data = defaults.data(forKey: "WiiRememberedBridges"),
           let saved = try? JSONDecoder().decode([SavedBridge].self, from: data) { savedBridges = saved }
        autoReconnect = defaults.object(forKey: "WiiBridgeAutoReconnect") as? Bool ?? true
        observers.append(NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.inForeground = false
                self.retryTask?.cancel()
                self.stopSearching()
                self.disconnectTransport()
                self.connectionDetail = "Paused while MeloCafe is in the background"
                self.log(self.connectionDetail)
            }
        })
        observers.append(NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.inForeground = true
                self.scheduleReconnect(immediate: true)
            }
        })
        client.onDiscovered = { [weak self] candidate in
            guard let self else { return }
            self.candidates.removeAll { $0.id == candidate.id }
            self.candidates.append(candidate)
            self.candidates.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
            if !self.manualSearch, (self.autoReconnect || self.selected != nil), !self.reconnectPaused,
               self.connection != .ready, self.connection != .connecting,
               (self.savedBridges.contains(where: { $0.id == candidate.id }) || self.selected?.id == candidate.id),
               self.selected == nil || self.selected?.id == candidate.id {
                self.attempt(candidate)
            }
        }
        client.onDiagnostic = { [weak self] _, message in self?.log(message) }
        client.onBluetoothStateChanged = { [weak self] state in
            guard let self else { return }
            self.bluetoothState = state
            if state == .poweredOn {
                if let selected = self.selected, !self.reconnectPaused, self.connection == .disconnected {
                    self.attempt(selected)
                } else { self.scheduleReconnect(immediate: true) }
            }
        }
        client.onConnectionChanged = { [weak self] id, state in
            guard let self, self.selected?.id == id else { return }
            self.connection = state
            switch state {
            case .ready:
                self.retryCount = 0
                self.retryTask?.cancel()
                self.errorMessage = nil
                self.connectionDetail = "Connected"
                if let selected = self.selected { self.remember(id: id, name: selected.name) }
                self.managementSupported = self.client.supportsBridgeManagement(id)
                self.remoteControlSupported = self.client.supportsRemoteControl(id)
                self.startOutputPump()
                self.refreshNames()
            case .disconnected, .failed:
                self.connectionGeneration += 1
                self.managementTask?.cancel()
                self.managementTask = nil
                self.isManaging = false
                self.metadata.reset()
                self.clearRemotes()
                self.pairingOpen = false
                self.managementSupported = false
                self.remoteControlSupported = false
                self.pausedOutputSlots = []
                if case .failed(let message) = state {
                    self.errorMessage = message
                    self.connectionDetail = message
                } else { self.connectionDetail = "Disconnected" }
                if !self.intentionalDisconnect { self.scheduleReconnect() }
            case .connecting:
                self.connectionDetail = "Connecting…"
            }
        }
        client.onBridgeStatus = { [weak self] id, status in
            guard let self, self.selected?.id == id else { return }
            self.pairingOpen = status.pairingOpen
            let added = status.connectedSlots.subtracting(self.connectedSlots)
            self.synchronizeSlots(status.connectedSlots)
            // Known remotes need no metadata round-trip when they wake. For a new
            // remote, let the native extension handshake run before its name read.
            let missing = added.filter { self.remoteInfo[$0] == nil }
            if self.managementSupported && !missing.isEmpty {
                self.metadata.request(Set(missing.map { .remote($0) }), delay: 3)
            }
        }
        client.onWiiReport = { [weak self] id, report in
            guard let self, self.selected?.id == id, let handle = self.handles[report.slot] else { return }
            let accepted = report.data.withUnsafeBytes { bytes in
                if let timestamp = report.timestampMicroseconds {
                    return WiiRemoteBridge_receiveTimed(handle, bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count, timestamp)
                }
                return WiiRemoteBridge_receive(handle, bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count)
            }
            if !accepted {
                // A bad/unsupported packet must not tear down every controller.
                self.log("Ignored Wii report \(report.data.first ?? 0), slot \(report.slot), bytes \(report.data.count)")
            }
        }
        // Starting the manager at app launch permits reconnect without visiting Settings.
        if autoReconnect && !savedBridges.isEmpty { client.prepareBluetooth() }
    }

    private func log(_ message: String) {
        let line = "\(Date().formatted(date: .omitted, time: .standard)) \(message)"
        diagnostics.append(line)
        if diagnostics.count > 30 { diagnostics.removeFirst(diagnostics.count - 30) }
        NSLog("Wii bridge: %@", message)
    }

    private func remember(id: UUID, name: String) {
        savedBridges.removeAll { $0.id == id }
        savedBridges.insert(SavedBridge(id: id, name: name), at: 0)
        saveBridges()
    }
    private func saveBridges() {
        if let data = try? JSONEncoder().encode(savedBridges) {
            UserDefaults.standard.set(data, forKey: "WiiRememberedBridges")
        }
    }
    func forget(_ bridge: SavedBridge) {
        if selected?.id == bridge.id { disconnect(); selected = nil }
        savedBridges.removeAll { $0.id == bridge.id }
        saveBridges()
    }
    func connectSaved(_ bridge: SavedBridge) {
        connect(ControllerCandidate(id: bridge.id, name: bridge.name, source: .wiiBridge))
    }
    func search() {
        retryTask?.cancel()
        manualSearch = true
        errorMessage = nil
        candidates.removeAll()
        isSearching = true
        client.startDiscovery()
    }
    func stopManualSearch() {
        guard manualSearch else { return }
        stopSearching()
        scheduleReconnect()
    }
    func stopSearching() {
        manualSearch = false
        isSearching = false
        client.stopDiscovery()
    }
    func connect(_ candidate: ControllerCandidate) {
        retryTask?.cancel()
        stopSearching()
        disconnectTransport()
        selected = candidate
        remoteInfo = [:]
        reconnectPaused = false
        retryCount = 0
        errorMessage = nil
        attempt(candidate)
    }
    private func attempt(_ candidate: ControllerCandidate) {
        guard inForeground else { return }
        selected = candidate
        client.prepareBluetooth()
        do {
            if client.candidates[candidate.id] != nil { try client.connect(to: candidate.id) }
            else { try client.connectToKnownBridge(id: candidate.id, name: candidate.name) }
            isSearching = false
            client.stopDiscovery()
        } catch {
            connection = .disconnected
            connectionDetail = "Waiting for \(candidate.name)…"
            log("Connect deferred: \(error)")
            client.startDiscovery()
            scheduleReconnect()
        }
    }
    func disconnect() {
        reconnectPaused = true
        retryTask?.cancel()
        stopSearching()
        disconnectTransport()
        connectionDetail = "Disconnected; automatic reconnection paused"
        log(connectionDetail)
    }
    private func disconnectTransport() {
        intentionalDisconnect = true
        if let selected { client.disconnect(selected.id) }
        intentionalDisconnect = false
        connectionGeneration += 1
        managementTask?.cancel()
        managementTask = nil
        isManaging = false
        metadata.reset()
        managementSupported = false
        remoteControlSupported = false
        pausedOutputSlots = []
        clearRemotes()
        connection = .disconnected
        pairingOpen = false
    }
    private func scheduleReconnect(immediate: Bool = false) {
        guard autoReconnect, !reconnectPaused, inForeground, !manualSearch,
              connection != .ready, connection != .connecting,
              selected != nil || !savedBridges.isEmpty else { return }
        retryTask?.cancel()
        let delay = immediate ? 0 : min(15, 1 << min(retryCount, 4))
        retryCount += 1
        connectionDetail = "Reconnecting in \(delay)s…"
        retryTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay) * 1_000_000_000)
            guard !Task.isCancelled, let self, self.inForeground, !self.reconnectPaused else { return }
            if let selected = self.selected { self.attempt(selected) }
            else {
                // Scan remembered bridges so an unavailable favorite cannot block another saved bridge.
                self.client.startDiscovery()
                self.connectionDetail = "Looking for remembered bridges…"
            }
        }
    }
    private func fail(_ message: String) {
        log(message)
        disconnectTransport()
        errorMessage = message
        scheduleReconnect()
    }

    func remoteName(_ slot: UInt8) -> String { remoteInfo[slot]?.name ?? "Wii Remote \(Int(slot) + 1)" }

    private func manage(_ operation: @escaping (UUID) async throws -> Void) {
        guard let selected, connection == .ready, managementSupported, !isManaging else { return }
        isManaging = true
        errorMessage = nil
        let generation = connectionGeneration
        managementTask = Task { [weak self] in
            guard let self else { return }
            do { try await self.metadata.performUserOperation { try await operation(selected.id) } }
            catch {
                if !Task.isCancelled, self.connectionGeneration == generation {
                    self.errorMessage = error.localizedDescription
                    self.log(error.localizedDescription)
                }
            }
            if self.connectionGeneration == generation { self.isManaging = false }
        }
    }
    private func readMetadata(_ item: WiiBridgeMetadataRefresh.Item) async throws {
        guard let selected, connection == .ready else { throw CancellationError() }
        let id = selected.id
        let generation = connectionGeneration
        switch item {
        case .bridge:
            let info = try await client.bridgeInfo(via: id)
            try Task.checkCancellation()
            guard generation == connectionGeneration else { throw CancellationError() }
            self.selected = ControllerCandidate(id: id, name: info.name, source: .wiiBridge)
            remember(id: id, name: info.name)
        case .remote(let slot):
            do {
                let info = try await client.remoteInfo(slot: slot, via: id)
                try Task.checkCancellation()
                guard generation == connectionGeneration else { throw CancellationError() }
                remoteInfo[slot] = info // Publish each successful read; retain the others.
            } catch BridgeManagementError.rejected(2) {
                try Task.checkCancellation()
                if generation == connectionGeneration { remoteInfo[slot] = nil }
            }
        case .players:
            guard remoteControlSupported else { return }
            let state = try await client.playerState(via: id)
            try Task.checkCancellation()
            guard generation == connectionGeneration else { throw CancellationError() }
            applyPlayerState(state)
        }
    }
    func refreshNames() {
        guard connection == .ready, managementSupported else { return }
        metadata.request([.bridge, .remote(0), .remote(1), .remote(2), .remote(3), .players])
    }
    func openPairing() {
        manage { [weak self] id in
            guard let self else { return }
            try await self.client.openPairingWindow(via: id)
            self.errorMessage = nil
        }
    }
    func renameBridge(_ name: String) {
        manage { [weak self] id in
            guard let self else { return }
            try await self.client.renameBridge(name, via: id)
            try Task.checkCancellation()
            self.selected = ControllerCandidate(id: id, name: name, source: .wiiBridge)
            self.remember(id: id, name: name)
        }
    }
    func renameRemote(slot: UInt8, name: String) {
        guard let remote = remoteInfo[slot] else { return }
        manage { [weak self] id in
            guard let self else { return }
            try await self.client.renameRemote(remote, name: name, via: id)
            try Task.checkCancellation()
            self.remoteInfo[slot] = try remote.renamed(name)
        }
    }

    private func applyPlayerState(_ state: BridgePlayerState) {
        let changed = playerOrder != state.order
        playerOrder = state.order
        disconnectedRemoteSlots = state.disconnectedSlots
        if changed { updateNativeOrder() }
    }
    private func updateNativeOrder() {
        connectedSlots = playerOrder.filter { handles[$0] != nil }
        let orderedHandles = connectedSlots.compactMap { handles[$0] }
        if orderedHandles.withUnsafeBufferPointer({ WiiRemoteBridge_setPlayerOrder($0.baseAddress, $0.count) }) {
            WiiRemoteBridge_notifyChanged()
        }
    }
    func reorderRemotes(_ orderedSlots: [UInt8]) {
        guard orderedSlots.count == connectedSlots.count, Set(orderedSlots) == Set(connectedSlots),
              remoteControlSupported, !isManaging else { return }
        let order = orderedSlots + playerOrder.filter { !orderedSlots.contains($0) }
        manage { [weak self] id in
            guard let self else { return }
            do {
                let state = try await self.client.setPlayerOrder(order, via: id)
                try Task.checkCancellation()
                self.applyPlayerState(state)
            }
            catch {
                // A lost acknowledgement may still mean the firmware applied the order.
                if !Task.isCancelled, let current = try? await self.client.playerState(via: id), !Task.isCancelled {
                    self.applyPlayerState(current)
                }
                throw error
            }
        }
    }
    func disconnectRemote(_ slot: UInt8) {
        guard let remote = remoteInfo[slot], remoteControlSupported, !isManaging else { return }
        manage { [weak self] id in
            guard let self else { return }
            self.pausedOutputSlots.insert(slot)
            defer { self.pausedOutputSlots.remove(slot) }
            try await self.client.disconnectRemote(remote, via: id)
            try Task.checkCancellation()
            self.disconnectedRemoteSlots.insert(slot)
        }
    }
    func connectRemote(_ slot: UInt8) {
        guard let remote = remoteInfo[slot], remoteControlSupported else { return }
        manage { [weak self] id in
            guard let self else { return }
            self.pausedOutputSlots.insert(slot)
            defer { self.pausedOutputSlots.remove(slot) }
            try await self.client.connectRemote(remote, via: id)
            try Task.checkCancellation()
            self.disconnectedRemoteSlots.remove(slot)
        }
    }

    private func synchronizeSlots(_ slots: Set<UInt8>) {
        var changed = false
        for slot in Array(handles.keys) where !slots.contains(slot) {
            WiiRemoteBridge_remove(handles.removeValue(forKey: slot)!)
            features[slot] = nil
            changed = true
        }
        for slot in slots.sorted() where handles[slot] == nil {
            let handle = WiiRemoteBridge_add()
            guard handle != 0 else {
                fail("MeloCafe could not register the Wii Remote. Reconnect the bridge.")
                return
            }
            handles[slot] = handle
            changed = true
        }
        connectedSlots = playerOrder.filter { handles[$0] != nil }
        if changed { updateNativeOrder() }
        if handles.isEmpty {
            outputTimer?.invalidate()
            outputTimer = nil
        } else if connection == .ready && outputTimer == nil {
            startOutputPump()
        }
    }

    private func clearRemotes() {
        outputTimer?.invalidate()
        outputTimer = nil
        let changed = !handles.isEmpty
        for handle in handles.values { WiiRemoteBridge_remove(handle) }
        handles.removeAll()
        connectedSlots = []
        features = [:]
        if changed { WiiRemoteBridge_notifyChanged() }
    }

    private func startOutputPump() {
        guard !handles.isEmpty else { return }
        outputTimer?.invalidate()
        // Native Wii commands are paced by Cemu at 25 ms. Drain on the main run
        // loop to keep CoreBluetooth and all connection state on one actor.
        let timer = Timer(timeInterval: 0.01, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in self?.drainOutput() }
        }
        outputTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func drainOutput() {
        guard connection == .ready, let selected else { return }
        var bytes = [UInt8](repeating: 0, count: 22)
        for slot in handles.keys.sorted() {
            guard let handle = handles[slot], !pausedOutputSlots.contains(slot) else { continue }
            let detected = WiiRemoteBridge_features(handle)
            if features[slot] != detected { features[slot] = detected }
            // Bound work per tick so input and UI are never starved.
            for _ in 0..<8 {
                let count = bytes.withUnsafeMutableBufferPointer {
                    WiiRemoteBridge_peekOutput(handle, $0.baseAddress, $0.count)
                }
                guard count > 0 else { break }
                do {
                    try client.sendWiiReport(Data(bytes.prefix(count)), slot: slot, via: selected.id)
                    WiiRemoteBridge_consumeOutput(handle)
                } catch NintendoBLEError.notReady {
                    break // Slot transitions are reconciled by the next bridge status.
                } catch NintendoBLEError.writeQueueFull {
                    return // Preserve the command and retry next tick.
                } catch {
                    fail("Could not send a Wii Remote command: \(error.localizedDescription)")
                    return
                }
            }
        }
    }
}

import CoreBluetooth
import SwiftUI
import NintendoBLE

struct WiiRemoteSettingsView: View {
    @ObservedObject private var bridge = WiiRemoteBridgeManager.shared
    @ObservedObject private var controllers = ControllerManager.shared
    private struct RenameTarget: Identifiable {
        let id = UUID()
        let slot: UInt8?
        let name: String
    }
    @State private var renameTarget: RenameTarget?

    private var otherWPADCount: Int {
        controllers.controllers.filter { $0.controllerType != .VPAD }.count
    }
    private var hasEnoughPorts: Bool {
        otherWPADCount + bridge.connectedSlots.count <= 7 &&
            controllers.controllers.count + bridge.connectedSlots.count <= 8
    }
    private var canConnectBridge: Bool { bridge.connection != .ready && bridge.connection != .connecting }
    private var sleepingSlots: [UInt8] {
        bridge.playerOrder.filter { bridge.remoteInfo[$0] != nil && !bridge.connectedSlots.contains($0) }
    }
    private var otherBridges: [WiiRemoteBridgeManager.SavedBridge] {
        bridge.savedBridges.filter { $0.id != bridge.selected?.id }
    }
    private var newCandidates: [ControllerCandidate] {
        bridge.candidates.filter { candidate in
            candidate.id != bridge.selected?.id && !bridge.savedBridges.contains { $0.id == candidate.id }
        }
    }

    var body: some View {
        Form {
            Section {
                if let selected = bridge.selected {
                    HStack(spacing: 12) {
                        Button {
                            renameTarget = RenameTarget(slot: nil, name: selected.name)
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(selected.name).foregroundStyle(.primary)
                                Text(bridge.connectionDetail).font(.caption).foregroundStyle(.secondary)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .disabled(bridge.connection != .ready || !bridge.managementSupported || bridge.isManaging)
                        .accessibilityHint("Rename bridge")
                        if bridge.connection == .connecting { ProgressView() }
                        if bridge.connection == .ready || bridge.connection == .connecting {
                            Button(bridge.connection == .connecting ? "Cancel" : "Disconnect", role: .destructive) {
                                bridge.disconnect()
                            }.buttonStyle(.borderless)
                        } else {
                            Button("Connect") { bridge.connect(selected) }.buttonStyle(.borderless)
                        }
                    }
                    .contextMenu {
                        if let saved = bridge.savedBridges.first(where: { $0.id == selected.id }) {
                            Button("Forget Bridge", role: .destructive) { bridge.forget(saved) }
                        }
                    }
                }
                Toggle("Automatically Reconnect", isOn: $bridge.autoReconnect)
            } header: {
                Text("Bridge")
            } footer: {
                if let error = bridge.errorMessage { Text(error).foregroundStyle(.red) }
            }

            if bridge.connection == .ready {
                Section {
                    if bridge.connectedSlots.isEmpty {
                        Text("No Wii Remotes connected").foregroundStyle(.secondary)
                    }
                    ForEach(bridge.connectedSlots, id: \.self) { slot in
                        remoteRow(slot, connected: true)
                            .moveDisabled(!bridge.remoteControlSupported || bridge.isManaging || bridge.connectedSlots.count < 2)
                    }
                    .onMove { source, destination in
                        var order = bridge.connectedSlots
                        order.move(fromOffsets: source, toOffset: destination)
                        bridge.reorderRemotes(order)
                    }
                } header: {
                    Text("Wii Remotes")
                } footer: {
                    if !hasEnoughPorts {
                        Text("Some Wii Remotes cannot be assigned. Remove another controller to free a port.")
                            .foregroundStyle(.orange)
                    } else if bridge.connectedSlots.count > 1 && bridge.remoteControlSupported {
                        Text("Drag to change player order.")
                    }
                }

                if !sleepingSlots.isEmpty {
                    Section("Other Remotes") {
                        ForEach(sleepingSlots, id: \.self) { slot in remoteRow(slot, connected: false) }
                    }
                }

                Section {
                    HStack {
                        Button(bridge.pairingOpen ? "Extend Pairing Window" : "Pair a Wii Remote") { bridge.openPairing() }
                            .disabled(!bridge.managementSupported || bridge.isManaging)
                        Spacer()
                        if bridge.pairingOpen { Text("Pairing…").font(.subheadline).foregroundStyle(.secondary) }
                    }
                } footer: {
                    Text(bridge.managementSupported
                         ? "Press the red SYNC button inside the remote’s battery cover within 30 seconds. Press A to wake a remembered remote."
                         : "Update the ESP32 firmware to pair from Settings. With older firmware, reset the bridge and press the remote’s red SYNC button.")
                }
            }

            if !otherBridges.isEmpty {
                Section("Remembered Bridges") {
                    ForEach(otherBridges) { saved in
                        HStack {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(saved.name)
                                Text(String(saved.id.uuidString.suffix(8))).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Connect") { bridge.connectSaved(saved) }
                                .buttonStyle(.borderless).disabled(!canConnectBridge)
                        }
                        .swipeActions {
                            Button("Forget", role: .destructive) { bridge.forget(saved) }
                        }
                    }
                }
            }

            if canConnectBridge {
                Section("Available Bridges") {
                    if bridge.bluetoothState == .poweredOff {
                        Text("Turn on Bluetooth to find your ESP32.")
                    } else if bridge.bluetoothState == .unauthorized {
                        Text("Allow Bluetooth access for MeloCafe in the iOS Settings app.")
                    } else if bridge.bluetoothState == .unsupported {
                        Text("Bluetooth is unavailable on this device.")
                    }
                    HStack {
                        Button(bridge.isSearching ? "Stop Searching" : "Find ESP32 Bridge") {
                            if bridge.isSearching { bridge.stopSearching() } else { bridge.search() }
                        }
                        Spacer()
                        if bridge.isSearching { ProgressView() }
                    }
                    ForEach(newCandidates) { candidate in
                        HStack {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(candidate.name)
                                Text(String(candidate.id.uuidString.suffix(8))).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Connect") { bridge.connect(candidate) }.buttonStyle(.borderless)
                        }
                    }
                }
            }
            if !bridge.diagnostics.isEmpty {
                Section {
                    DisclosureGroup("Connection Diagnostics") {
                        Text(bridge.diagnostics.joined(separator: "\n"))
                            .font(.caption.monospaced()).textSelection(.enabled)
                    }
                }
            }
        }
        .environment(\.editMode, .constant(bridge.remoteControlSupported && bridge.connectedSlots.count > 1 ? .active : .inactive))
        .navigationTitle("Wii Remotes")
        .onDisappear { bridge.stopManualSearch() }
        .sheet(item: $renameTarget) { target in
            WiiBridgeRenameView(name: target.name) { name in
                if let slot = target.slot { bridge.renameRemote(slot: slot, name: name) }
                else { bridge.renameBridge(name) }
            }
        }
    }

    private func remoteRow(_ slot: UInt8, connected: Bool) -> some View {
        HStack(spacing: 12) {
            Button {
                renameTarget = RenameTarget(slot: slot, name: bridge.remoteName(slot))
            } label: {
                VStack(alignment: .leading, spacing: 4) {
                    Text(bridge.remoteName(slot)).foregroundStyle(.primary)
                    Text(remoteDetail(slot, connected: connected)).font(.caption).foregroundStyle(.secondary)
                    if connected, let features = bridge.features[slot], features & 1 != 0, features & 8 == 0 {
                        Text("Keep still for gyro calibration.").font(.caption).foregroundStyle(.secondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!bridge.managementSupported || bridge.remoteInfo[slot] == nil || bridge.isManaging)
            .accessibilityHint("Rename Wii Remote")
            if bridge.remoteControlSupported {
                if connected {
                    Button("Disconnect", role: .destructive) { bridge.disconnectRemote(slot) }
                        .buttonStyle(.borderless).disabled(bridge.isManaging)
                } else {
                    Button("Connect") { bridge.connectRemote(slot) }
                        .buttonStyle(.borderless).disabled(bridge.isManaging)
                }
            }
        }
    }
    private func remoteDetail(_ slot: UInt8, connected: Bool) -> String {
        guard connected else { return bridge.disconnectedRemoteSlots.contains(slot) ? "Disconnected" : "Asleep" }
        let player = (bridge.connectedSlots.firstIndex(of: slot) ?? 0) + otherWPADCount + 1
        let features = bridge.features[slot] ?? 0
        var parts = ["Player \(player)"]
        if features & 1 != 0 { parts.append("MotionPlus") }
        if features & 2 != 0 { parts.append("Nunchuck") }
        if features & 4 != 0 { parts.append("Classic Controller") }
        return parts.joined(separator: " · ")
    }
}

private struct WiiBridgeRenameView: View {
    @Environment(\.dismiss) private var dismiss
    @State var name: String
    let save: (String) -> Void
    var body: some View {
        NavigationView {
            Form {
                TextField("Name", text: $name)
                if !BridgeDeviceInfo.validName(name) {
                    Text("Enter a shorter, nonempty name without control characters.").font(.caption).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Rename")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save(name); dismiss() }.disabled(!BridgeDeviceInfo.validName(name))
                }
            }
        }
    }
}

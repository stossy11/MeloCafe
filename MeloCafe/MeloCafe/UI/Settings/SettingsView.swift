//
//  SettingsView.swift
//  MeloCafe
//
//  Created by Stossy11 on 7/4/2026.
//


import SwiftUI
import UniformTypeIdentifiers

extension CemuConfigWrapper {
    func cast<T>(_ value: Any) -> T? {
        value as? T
    }
}

extension Binding {
    func map<U>(
        get: @escaping (Value) -> U,
        set: @escaping (U) -> Value
    ) -> Binding<U> {
        Binding<U>(
            get: { get(self.wrappedValue) },
            set: { self.wrappedValue = set($0) }
        )
    }
}

struct NavigationStack<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        if #available(iOS 16, *) {
            SwiftUI.NavigationStack(root: content)
        } else {
            NavigationView(content: content)
                .navigationViewStyle(.stack)
        }
    }
}

struct AppIconPosition: Identifiable {
    var id: String { creator }
    var creator: String
    var icons: [AppIcon]
}

struct AppIcon: Identifiable {
    var id: String
    var name: String
    var def: Bool = false
}

private enum SettingsTab: String, CaseIterable, Identifiable {
    case general
    case graphics
    case audio
    case overlay
    case notifications
    case input

    var id: String { rawValue }

    var title: String {
        switch self {
        case .general: return "General"
        case .graphics: return "Graphics"
        case .audio: return "Audio"
        case .overlay: return "Overlay"
        case .notifications: return "Alerts"
        case .input: return "Input"
        }
    }

    var icon: String {
        switch self {
        case .general: return "gearshape"
        case .graphics: return "display"
        case .audio: return "speaker.wave.2"
        case .overlay: return "gauge"
        case .notifications: return "bell"
        case .input: return "gamecontroller"
        }
    }
}

private struct SettingsTabBar: View {
    @Binding var selection: SettingsTab

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach(SettingsTab.allCases) { tab in
                    Button {
                        selection = tab
                    } label: {
                        VStack(spacing: 4) {
                            Image(systemName: tab.icon)
                                .font(.system(size: 20))
                            Text(tab.title)
                                .font(.caption2)
                        }
                        .frame(minWidth: 64)
                        .padding(.vertical, 8)
                        .padding(.horizontal, 6)
                        .foregroundColor(selection == tab ? .accentColor : .secondary)
                        .background(
                            RoundedRectangle(cornerRadius: 10, style: .continuous)
                                .fill(selection == tab ? Color.accentColor.opacity(0.12) : Color.clear)
                        )
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal)
            .padding(.vertical, 6)
        }
        .background(Color(.systemGroupedBackground))
        .animation(.easeInOut(duration: 0.15), value: selection)
    }
}

private struct SettingsSlider<V: BinaryFloatingPoint>: View where V.Stride: BinaryFloatingPoint {
    let title: String
    @Binding var value: V
    let range: ClosedRange<V>
    let step: V.Stride
    var unit: String = ""
    var decimals: Int = 0
    var defaultValue: V? = nil

    private var formatted: String {
        String(format: "%.\(decimals)f", Double(value)) + unit
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Text(formatted)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)

                if let defaultValue, value != defaultValue {
                    Button("Reset") {
                        withAnimation { value = defaultValue }
                    }
                    .buttonStyle(.borderless)
                    .font(.footnote)
                }
            }
            Slider(value: $value, in: range, step: step)
        }
        .padding(.vertical, 2)
    }
}

private struct HexColorRow: View {
    let title: String
    @Binding var hex: String

    private var colorBinding: Binding<Color> {
        Binding<Color>(
            get: { Self.hexToColor(hex) ?? .white },
            set: { hex = Self.colorToHex($0, keepAlpha: hex.filter(\.isHexDigit).count == 8) }
        )
    }

    var body: some View {
        ColorPicker(title, selection: colorBinding, supportsOpacity: false)
    }

    private static func hexToColor(_ string: String) -> Color? {
        let s = string.trimmingCharacters(in: CharacterSet(charactersIn: "#")).filter(\.isHexDigit)
        guard s.count == 6 || s.count == 8, let v = UInt64(s, radix: 16) else { return nil }
        let hasAlpha = s.count == 8
        let r = Double((v >> (hasAlpha ? 24 : 16)) & 0xFF) / 255
        let g = Double((v >> (hasAlpha ? 16 : 8)) & 0xFF) / 255
        let b = Double((v >> (hasAlpha ? 8 : 0)) & 0xFF) / 255
        return Color(red: r, green: g, blue: b)
    }

    private static func colorToHex(_ color: Color, keepAlpha: Bool) -> String {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        UIColor(color).getRed(&r, green: &g, blue: &b, alpha: &a)
        let base = String(format: "%02X%02X%02X", Int(round(r * 255)), Int(round(g * 255)), Int(round(b * 255)))
        return keepAlpha ? base + "FF" : base
    }
}

struct SettingsView: View {
    @ObservedObject private var controllerManager: ControllerManager = .shared
    @StateObject private var configManager: ConfigManager = .shared
    @EnvironmentObject private var gameManager: GamesManager

    @AppStorage("cardType") var cardTypeRawValue: String = CardType.card.rawValue
    var cardType: Binding<CardType> {
        .init {
            CardType(rawValue: cardTypeRawValue) ?? .card
        } set: { type in
            cardTypeRawValue = type.rawValue
        }
    }

    @AppStorage(RamDonor.enabledPreferenceKey) private var ramDonorsEnabled = true
    @AppStorage("breakpoint") var stikDebugbreakpoint = false

    @AppStorage("showSwapButton") var showSwapButton: Bool = true
    @AppStorage("screenLayout") private var screenLayout = ScreenLayout.initialValue

    @State private var selectedTab: SettingsTab = .general
    @State private var showingEmulatedDevices = false

    private var hasJITEntitlement: Bool { checkAppEntitlement("get-task-allow") }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                SettingsTabBar(selection: $selectedTab)
                Divider()
                tabContent
            }
            .navigationTitle("Settings") // iOS 15 seems to expect a navigation title, so we'll put this here. -stossy11
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    @ViewBuilder
    private var tabContent: some View {
        switch selectedTab {
        case .general: generalTab
        case .graphics: GraphicsSettingsView()
        case .audio: AudioSettingsView()
        case .overlay: OverlaySettingsView()
        case .notifications: NotificationSettingsView()
        case .input: inputTab
        }
    }

    private var generalTab: some View {
        Form {
            cpuSection
            memorySection
            appSection
            generalSection
            accountSection
            loadGameSection
            memorialSection
        }
    }

    private var inputTab: some View {
        Form {
            controllersSection
            inputServicesSection
            emulatedDevicesSection
        }
        .sheet(isPresented: $showingEmulatedDevices) {
            EmulatedDevicesView()
        }
    }

    private var cpuSection: some View {
        Section {
            Picker("CPU Mode", selection: configManager.interpreter) {
                ForEach(hasJITEntitlement ? CPUMode.allCases : CPUMode.allCasesAppStore, id: \.self) {
                    Text($0.string)
                }
            }
            .pickerStyle(.menu)
            .disabled(!hasJITEntitlement)
        } header: {
            Text("CPU")
        } footer: {
            VStack(alignment: .leading, spacing: 4) {
                Text("\"Single Core\" CPU Modes are slower but may offer more stable performance / better battery life depending on the title.")
                if !hasJITEntitlement {
                    Text("Other CPU modes are unavailable because this build doesn't have the debugging entitlement.")
                }
            }
        }
    }

    private var memorySection: some View {
        Section {
            Toggle("Use RAM Donors", isOn: $ramDonorsEnabled)
        } header: {
            Text("Memory")
        } footer: {
            Text("Use helper processes to supply guest RAM. Restart MeloCafe for changes to take effect.")
        }
    }

    private var appSection: some View {
        Section("App") {
            NavigationLink("App Icon Switcher") {
                AppIconSwitcher()
            }

            Picker("Library View", selection: cardType) {
                ForEach(CardType.allCases, id: \.self) {
                    Text($0.displayName)
                }
            }
            .pickerStyle(.menu)
        }
    }

    private var generalSection: some View {
        Section("General") {
            Picker("Console Language", selection: configManager.consoleLanguage) {
                ForEach(ConsoleLanguage.allCases, id: \.self) {
                    Text($0.string)
                }
            }
            .pickerStyle(.menu)

            HStack {
                Text("Screen Layout")

                Button {
                    AppAlerts.showSyncAlert(title: "Screen Layout", message: screenLayout.description)
                } label: {
                    Image(systemName: "info.circle")
                }
                .buttonStyle(.borderless)
                .foregroundStyle(.secondary)
                .accessibilityLabel("About screen layouts")

                Spacer()

                Picker("", selection: $screenLayout) {
                    ForEach(ScreenLayout.allCases, id: \.self) { layout in
                        Text(layout.string).tag(layout)
                    }
                }
                .pickerStyle(.menu)
            }

            if screenLayout == .singleScreen {
                Toggle("Show Swap Button (TV <-> Pad)", isOn: $showSwapButton)
                    .transition(.opacity)
            }

            Toggle("Disable Screensaver", isOn: configManager.disableScreensaver)
            Toggle("Play Boot Sound", isOn: configManager.playBootSound)
        }
        .animation(.default, value: screenLayout)
    }

    private var accountSection: some View {
        Section("Account") {
            NavigationLink {
                AccountSettingsView()
            } label: {
                HStack {
                    Text("Account Settings")
                    Spacer()
                    Text(configManager.activeAccount?.displayName ?? "None")
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
        }
    }

    private var loadGameSection: some View {
        Section {
            Button("Load from Folder") {
                importGame(types: [.folder])
            }

            Button("Load from File") {
                importGame(types: [.item])
            }
        } header: {
            Text("Load Game")
        } footer: {
            Text("Launch a title that isn't in your library.")
        }
    }

    private var memorialSection: some View {
        Section {
            HStack(spacing: 6) {
                Text("In memoriam of 'Lily'")
                    .font(.system(.footnote, design: .monospaced))
                    .foregroundColor(.secondary)
                Image(systemName: "heart")
                    .foregroundColor(.purple)
                    .font(.footnote)
            }
            .frame(maxWidth: .infinity, alignment: .center)
            .listRowBackground(Color.clear)
        }
    }

    private var controllersSection: some View {
        Section {
            if controllerManager.controllers.isEmpty {
                Text("No controllers connected")
                    .foregroundStyle(.secondary)
            }

            ForEach(controllerManager.controllers) { entry in
                ControllerRow(entry: entry)
                    .contextMenu {
                        ForEach(ControllerType.allCases) { type in
                            if !type.name.isEmpty {
                                Button(type.name) {
                                    controllerManager.setControllerType(id: entry.id, to: type)
                                }
                                .disabled(!controllerManager.canSelectType(type, for: entry.id))
                            }
                        }
                    }
            }
            .onMove { source, destination in
                controllerManager.move(from: source, to: destination)
            }
            .onDelete { offsets in
                let ids = offsets.map { controllerManager.controllers[$0].id }
                for id in ids { controllerManager.remove(id: id) }
            }

            let missing = controllerManager.missingControllers()

            if !missing.isEmpty {
                Menu("Add Controller") {
                    ForEach(missing) { entry in
                        Button(entry.name) {
                            controllerManager.addFromAll(id: entry.id)
                        }
                        .disabled(!controllerManager.canAdd(id: entry.id))
                    }
                }
            }
        } header: {
            Text("Controllers")
        } footer: {
            Text("Drag to set player order. Touch and hold a controller to change its type.")
        }
        .environment(\.editMode, .constant(.active))
    }

    private var inputServicesSection: some View {
        Section("Input Services") {
            Toggle("Disable Motion", isOn: configManager.disableMotion)

            //TextField("DSU Host", text: configManager.dsuHost)
            //    .textInputAutocapitalization(.never)
            //    .autocorrectionDisabled()

            //TextField("DSU Port", text: configManager.dsuPortString)
            //    .keyboardType(.numberPad)
        }
    }

    private var emulatedDevicesSection: some View {
        Section {
            Toggle("Skylanders Portal", isOn: configManager.emulateSkylanderPortal)
            Toggle("Disney Infinity Base", isOn: configManager.emulateInfinityBase)
            Toggle("LEGO Dimensions Toypad", isOn: configManager.emulateDimensionsToypad)

            Button("Manage Figures") {
                showingEmulatedDevices = true
            }
        } header: {
            Text("Emulated Devices")
        } footer: {
            Text("Enable a device here, then use Manage Figures to load your figures onto it.")
        }
    }

    private func importGame(types: [UTType]) {
        FileImporterManager.shared.importFiles(
            types: types,
            allowMultiple: false,
            stopAccessingSecurityScopedResources: false
        ) { result in
            handleImportResult(result)
        }
    }

    private func handleImportResult(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            guard url.startAccessingSecurityScopedResource() else {
                AppAlerts.showSyncAlert(title: "Couldn't Open", message: "Permission to access that location was denied.")
                return
            }
            gameManager.loadGame(url.path, true)
        case .failure(let error):
            AppAlerts.showSyncAlert(title: "Import Failed", message: error.localizedDescription)
        }
    }
}

private struct GraphicsSettingsView: View {
    @ObservedObject private var configManager: ConfigManager = .shared

    var body: some View {
        Form {
            Section("Renderer") {
                Picker("Renderer", selection: configManager.renderer) {
                    ForEach(Renderer.allCases, id: \.self) {
                        if !$0.string.isEmpty {
                            Text($0.string)
                        }
                    }
                }

                if configManager.renderer.wrappedValue == .vulkan {
                    Toggle("Accurate Barriers", isOn: configManager.vkAccurateBarriers)
                }

                if configManager.renderer.wrappedValue == .metal {
                    Toggle("Force Mesh Shaders", isOn: configManager.forceMeshShaders)
                    Toggle("Framebuffer Fetch", isOn: configManager.framebufferFetch)
                }
            }
            .animation(.default, value: configManager.renderer.wrappedValue)

            Section("Sync & Shaders") {
                Toggle("VSync", isOn: configManager.vsync)
                Toggle("GX2 DrawDone Sync", isOn: configManager.gx2DrawDoneSync)
                Toggle("Async Shader Compile", isOn: configManager.asyncCompile)
                Toggle("Shader Cache", isOn: configManager.precompiledShaders)
            }

            Section("Scaling") {
                Picker("Upscale Filter", selection: configManager.upscaleFilter) {
                    ForEach(UpscalingFilter.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                Picker("Downscale Filter", selection: configManager.downscaleFilter) {
                    ForEach(UpscalingFilter.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                Picker("Fullscreen Scaling", selection: configManager.fullscreenScaling) {
                    ForEach(FullscreenScaling.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                Toggle("Render Upside Down", isOn: configManager.renderUpsideDown)
            }

            Section("Gamma") {
                Toggle("Override App Gamma", isOn: configManager.overrideAppGammaPreference)

                if configManager.overrideAppGammaPreference.wrappedValue {
                    SettingsSlider(
                        title: "Override Gamma",
                        value: configManager.overrideGammaValue,
                        range: 1.0...3.0,
                        step: 0.05,
                        decimals: 2,
                        defaultValue: 2.2
                    )
                }

                SettingsSlider(
                    title: "Display Gamma",
                    value: configManager.userDisplayGamma,
                    range: 1.0...3.0,
                    step: 0.05,
                    decimals: 2,
                    defaultValue: 2.2
                )
            }
            .animation(.default, value: configManager.overrideAppGammaPreference.wrappedValue)

            Section {
                Toggle("Dump Textures and Shaders", isOn: configManager.dumpGraphicsAssets)
            } header: {
                Text("Graphics Diagnostics")
            } footer: {
                Text("For timing, enable Overlay → Debug; summaries are written to log.txt once per second. For visual glitches, enable dumping before launching a game, then find the dump folder in MeloCafe’s files. Dumping slows gameplay and uses storage, so keep it off for timing runs. It turns off when the app closes.")
            }

            Section {
                NavigationLink("All Graphic Packs") {
                    GraphicPacksView()
                }
            }
        }
    }
}

private struct AudioSettingsView: View {
    @ObservedObject private var configManager: ConfigManager = .shared

    var body: some View {
        Form {
            Section("TV") {
                Toggle("TV Audio", isOn: configManager.tvAudioEnabled)

                Picker("TV Channels", selection: configManager.tvChannels) {
                    ForEach(AudioChannels.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                SettingsSlider(title: "TV Volume", value: configManager.tvVolume, range: 0...100, step: 1, unit: "%")
            }

            Section("Gamepad") {
                Toggle("Pad Audio", isOn: configManager.padAudioEnabled)

                Picker("Pad Channels", selection: configManager.padChannels) {
                    ForEach(AudioChannels.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                SettingsSlider(title: "Pad Volume", value: configManager.padVolume, range: 0...100, step: 1, unit: "%")
            }

            Section("Input") {
                Toggle("Microphone", isOn: configManager.microphoneEnabled)

                Picker("Input Channels", selection: configManager.inputChannels) {
                    ForEach(AudioChannels.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                SettingsSlider(title: "Input Volume", value: configManager.inputVolume, range: 0...100, step: 1, unit: "%")
            }

            Section("Toys-to-Life") {
                SettingsSlider(title: "Portal Volume", value: configManager.portalVolume, range: 0...100, step: 1, unit: "%")
            }
        }
    }
}

private struct OverlaySettingsView: View {
    @ObservedObject private var configManager: ConfigManager = .shared

    var body: some View {
        Form {
            Section("Appearance") {
                Picker("Position", selection: configManager.overlayPosition) {
                    ForEach(ScreenPosition.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                HexColorRow(title: "Text Color", hex: configManager.overlayTextColorHex)

                SettingsSlider(
                    title: "Text Scale",
                    value: configManager.overlayTextScale,
                    range: 50...200,
                    step: 25,
                    unit: "%",
                    defaultValue: 100
                )
            }

            Section("Statistics") {
                Toggle("FPS", isOn: configManager.overlayFPS)
                Toggle("CPU Mode", isOn: configManager.overlayCPUMode)
                Toggle("Draw Calls", isOn: configManager.overlayDrawcalls)
                Toggle("CPU Usage", isOn: configManager.overlayCPUUsage)
                Toggle("CPU Per Core Usage", isOn: configManager.overlayCPUPerCoreUsage)
                Toggle("RAM Usage", isOn: configManager.overlayRAMUsage)
                Toggle("VRAM Usage", isOn: configManager.overlayVRAMUsage)
                Toggle("Debug", isOn: configManager.overlayDebug)
            }
        }
    }
}

private struct NotificationSettingsView: View {
    @ObservedObject private var configManager: ConfigManager = .shared

    var body: some View {
        Form {
            Section("Appearance") {
                Picker("Position", selection: configManager.notificationPosition) {
                    ForEach(ScreenPosition.allCases, id: \.self) {
                        Text($0.string)
                    }
                }

                HexColorRow(title: "Text Color", hex: configManager.notificationTextColorHex)

                SettingsSlider(
                    title: "Text Scale",
                    value: configManager.notificationTextScale,
                    range: 50...200,
                    step: 25,
                    unit: "%",
                    defaultValue: 100
                )
            }

            Section("Show Notifications For") {
                Toggle("Controller Profiles", isOn: configManager.notificationControllerProfiles)
                Toggle("Low Battery", isOn: configManager.notificationControllerBattery)
                Toggle("Shader Compiling", isOn: configManager.notificationShaderCompiling)
                Toggle("Friends", isOn: configManager.notificationFriends)
            }
        }
    }
}

private struct ControllerRow: View {
    let entry: ControllerEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(entry.name)
                .font(.body)
            Text(entry.controllerType.name)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }
}

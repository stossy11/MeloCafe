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


struct SettingsView: View {
    @ObservedObject private var controllerManager: ControllerManager = .shared
    @StateObject private var configManager: ConfigManager = .shared
    @EnvironmentObject private var gameManager: GamesManager
    
    @AppStorage("cardType") var cardTypeRawValue: String = CardType.list.rawValue
    var cardType: Binding<CardType> {
        .init {
            CardType(rawValue: cardTypeRawValue) ?? .card
        } set: { type in
            cardTypeRawValue = type.rawValue
        }
    }
    
    @AppStorage("breakpoint") var stikDebugbreakpoint = false
    
    @AppStorage("showSwapButton") var showSwapButton: Bool = true
    @AppStorage("screenLayout") private var screenLayout = ScreenLayout.initialValue
    
    @State private var showingEmulatedDevices = false
    
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    if checkAppEntitlement("get-task-allow") {
                        Picker("CPU Mode", selection: configManager.interpreter) {
                            ForEach(CPUMode.allCases, id: \.self) {
                                Text($0.string)
                            }
                        }
                        .pickerStyle(.menu)
                    } else {
                        Picker("CPU Mode", selection: .constant(CPUMode.interpreter)) {
                            ForEach(CPUMode.allCases, id: \.self) {
                                Text($0.string)
                            }
                        }
                        .pickerStyle(.menu)
                        .disabled(true)
                    }
                } header: {
                    Text("CPU")
                }
                
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
                        .foregroundStyle(.secondary)
                        
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
                    }
                    
                    Toggle("Disable Screensaver", isOn: configManager.disableScreensaver)
                    Toggle("Play Boot Sound", isOn: configManager.playBootSound)
                }
                
                Section("Account") {
                    NavigationLink {
                        AccountSettingsView()
                    } label: {
                        HStack {
                            Text("Account settings")
                            Spacer()
                            Text(configManager.activeAccount?.displayName ?? "No account selected")
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                
                Section("Controllers") {
                    ForEach(controllerManager.controllers) { entry in
                        ControllerRow(entry: entry)
                            .contextMenu {
                                ForEach(ControllerType.allCases) { type in
                                    if !type.name.isEmpty {
                                        Button {
                                            controllerManager.setControllerType(id: entry.id, to: type)
                                        } label: {
                                            if entry.controllerType == type {
                                                Label(type.name, systemImage: "checkmark")
                                            } else {
                                                Text(type.name)
                                            }
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
                    
                    let cont = controllerManager.missingControllers()
                    
                    if !cont.isEmpty {
                        Menu {
                            ForEach(cont) { entry in
                                Button {
                                    controllerManager.addFromAll(id: entry.id)
                                } label: {
                                    Text(entry.name)
                                }
                                .disabled(!controllerManager.canAdd(id: entry.id))
                            }
                        } label: {
                            Label("Controllers", systemImage: "chevron.down")
                        }
                    }
                    
                }
                .environment(\.editMode, .constant(.active))
                
                Section("Graphics") {
                    Picker("Renderer", selection: configManager.renderer) {
                        ForEach(Renderer.allCases, id: \.self) {
                            if !$0.string.isEmpty {
                                Text($0.string)
                            }
                        }
                    }
                    
                    Toggle("VSync", isOn: configManager.vsync)
                    Toggle("GX2 DrawDone Sync", isOn: configManager.gx2DrawDoneSync)
                    Toggle("Render Upside Down", isOn: configManager.renderUpsideDown)
                    Toggle("Async Shader Compile", isOn: configManager.asyncCompile)
                    Toggle("Shader Cache", isOn: configManager.precompiledShaders)
                    
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
                    
                    if configManager.renderer.wrappedValue == .vulkan {
                        Toggle("Accurate Barriers", isOn: configManager.vkAccurateBarriers)
                    }
                    
                    if configManager.renderer.wrappedValue == .metal {
                        Toggle("Force Mesh Shaders", isOn: configManager.forceMeshShaders)
                        Toggle("Framebuffer Fetch", isOn: configManager.framebufferFetch)
                    }
                    
                    Toggle("Override App Gamma", isOn: configManager.overrideAppGammaPreference)
                    
                    if configManager.overrideAppGammaPreference.wrappedValue {
                        VStack(alignment: .leading) {
                            HStack {
                                Text("Override Gamma")
                                Spacer()
                                Text(String(format: "%.2f", configManager.overrideGammaValue.wrappedValue))
                                    .foregroundStyle(.secondary)
                            }
                            Slider(value: configManager.overrideGammaValue, in: 1.0...3.0, step: 0.05)
                        }
                    }
                    
                    VStack(alignment: .leading) {
                        HStack {
                            Text("Display Gamma")
                            Spacer()
                            Text(String(format: "%.2f", configManager.userDisplayGamma.wrappedValue))
                                .foregroundStyle(.secondary)
                        }
                        Slider(value: configManager.userDisplayGamma, in: 1.0...3.0, step: 0.05)
                    }
                    
                    
                    NavigationLink("All Graphic Packs") {
                        GraphicPacksView()
                    }
                }
                
                Section("Audio") {
                    Toggle("TV Audio", isOn: configManager.tvAudioEnabled)
                    Toggle("Pad Audio", isOn: configManager.padAudioEnabled)
                    
                    Picker("TV Channels", selection: configManager.tvChannels) {
                        ForEach(AudioChannels.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Picker("Pad Channels", selection: configManager.padChannels) {
                        ForEach(AudioChannels.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Picker("Input Channels", selection: configManager.inputChannels) {
                        ForEach(AudioChannels.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Toggle("Microphone", isOn: configManager.microphoneEnabled)
                    
                    VStack(alignment: .leading) {
                        Text("TV Volume: \(Int(configManager.tvVolume.wrappedValue))%")
                        Slider(value: configManager.tvVolume, in: 0...100, step: 1)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Pad Volume: \(Int(configManager.padVolume.wrappedValue))%")
                        Slider(value: configManager.padVolume, in: 0...100, step: 1)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Input Volume: \(Int(configManager.inputVolume.wrappedValue))%")
                        Slider(value: configManager.inputVolume, in: 0...100, step: 1)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Portal Volume: \(Int(configManager.portalVolume.wrappedValue))%")
                        Slider(value: configManager.portalVolume, in: 0...100, step: 1)
                    }
                }
                
                Section("Overlay") {
                    Picker("Position", selection: configManager.overlayPosition) {
                        ForEach(ScreenPosition.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    TextField("Text Color", text: configManager.overlayTextColorHex)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    
                    VStack(alignment: .leading) {
                        Text("Text Scale: \(Int(configManager.overlayTextScale.wrappedValue))%")
                        Slider(value: configManager.overlayTextScale, in: 50...200, step: 25)
                    }
                    
                    Toggle("FPS", isOn: configManager.overlayFPS)
                    Toggle("CPU Mode", isOn: configManager.overlayCPUMode)
                    Toggle("Draw Calls", isOn: configManager.overlayDrawcalls)
                    Toggle("CPU Usage", isOn: configManager.overlayCPUUsage)
                    Toggle("CPU Per Core Usage", isOn: configManager.overlayCPUPerCoreUsage)
                    Toggle("RAM Usage", isOn: configManager.overlayRAMUsage)
                    Toggle("VRAM Usage", isOn: configManager.overlayVRAMUsage)
                    Toggle("Debug", isOn: configManager.overlayDebug)
                }
                
                Section("Notifications") {
                    Picker("Position", selection: configManager.notificationPosition) {
                        ForEach(ScreenPosition.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    TextField("Text Color", text: configManager.notificationTextColorHex)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    
                    VStack(alignment: .leading) {
                        Text("Text Scale: \(Int(configManager.notificationTextScale.wrappedValue))%")
                        Slider(value: configManager.notificationTextScale, in: 50...200, step: 25)
                    }
                    
                    Toggle("Controller Profiles", isOn: configManager.notificationControllerProfiles)
                    Toggle("Low Battery", isOn: configManager.notificationControllerBattery)
                    Toggle("Shader Compiling", isOn: configManager.notificationShaderCompiling)
                    Toggle("Friends", isOn: configManager.notificationFriends)
                }
                
                Section("Input Services") {
                    Toggle("Disable Motion", isOn: configManager.disableMotion)
                    
                    //TextField("DSU Host", text: configManager.dsuHost)
                    //    .textInputAutocapitalization(.never)
                    //    .autocorrectionDisabled()
                    
                    //TextField("DSU Port", text: configManager.dsuPortString)
                    //    .keyboardType(.numberPad)
                }
                
                Section("Emulated Devices") {
                    Toggle("Skylanders Portal", isOn: configManager.emulateSkylanderPortal)
                    Toggle("Disney Infinity Base", isOn: configManager.emulateInfinityBase)
                    Toggle("LEGO Dimensions Toypad", isOn: configManager.emulateDimensionsToypad)
                    Button {
                        showingEmulatedDevices = true
                    } label: {
                        Label("Manage Figures", systemImage: "externaldrive.connected.to.line.below")
                    }
                    .sheet(isPresented: $showingEmulatedDevices) {
                        EmulatedDevicesView()
                    }
                }
                
                Section("Load Game") {
                    Button("Load from Folder") {
                        FileImporterManager.shared.importFiles(
                            types: [.folder],
                            allowMultiple: false,
                            stopAccessingSecurityScopedResources: false
                        ) { result in
                            handleImportResult(result)
                        }
                    }
                    
                    Button("Load from File") {
                        FileImporterManager.shared.importFiles(
                            types: [.item],
                            allowMultiple: false,
                            stopAccessingSecurityScopedResources: false
                        ) { result in
                            handleImportResult(result)
                        }
                    }
                }
                
                Section {
                    HStack {
                        Text("In memoriam of 'Lily'")
                            .font(.system(.footnote, design: .monospaced))
                            .foregroundColor(.secondary)
                        Image(systemName: "heart")
                            .foregroundColor(.purple)
                            .font(.footnote)
                    }
                    .frame(maxWidth: .infinity, alignment: .center)
                } 
            }
            .navigationTitle("Settings") // iOS 15 seems to expect a navigation title, so we'll put this here. -stossy11
        }
    }
    
    private func handleImportResult(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            _ = url.startAccessingSecurityScopedResource()
            gameManager.loadGame(url.path, true)
        case .failure(let error):
            print("Failed to import: \(error)")
        }
    }
}

private struct ControllerRow: View {
    let entry: ControllerEntry

    var body: some View {
        HStack {
            Image(systemName: entry.isVirtual ? "iphone" : "gamecontroller.fill")
                .foregroundStyle(entry.isVirtual ? .blue : .primary)
                .frame(width: 28)

            VStack(alignment: .leading, spacing: 2) {
                Text(entry.name)
                    .font(.body)
                Text(entry.controllerType.name)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }
}

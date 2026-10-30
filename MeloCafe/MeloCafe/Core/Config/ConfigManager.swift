//
//  ConfigBindings.swift
//  MeloCafe
//
//  Created by Stossy11 on 5/4/2026.
//

import SwiftUI
import Combine

@MainActor
class ConfigManager: ObservableObject {
    let config = CemuConfigWrapper.shared()
    static var shared: ConfigManager = .init()

    @Published var accounts: [Account] = []
    @Published var accountCountries: [AccountCountry] = []
    @Published var activeAccountPersistentId: UInt32 = 0
    @Published var accountControlsLocked = false
    @Published var customNetworkServiceAvailable = false

    private init() {
        reloadAccounts()
    }


    var cpuModeBinding: Binding<ObjCCPUMode> {
        Binding {
            self.config.cpuMode
        } set: {
            self.config.cpuMode = $0
            self.objectWillChange.send()
        }
    }

    var interpreter: Binding<CPUMode> {
        cpuModeBinding.map(
            get: { .init($0) },
            set: { $0.config }
        )
    }

    var renderer: Binding<Renderer> {
        Binding {
            Renderer(self.config.graphicAPI)
        } set: { val in
            self.config.graphicAPI = val.config
            self.objectWillChange.send()
        }
    }

    var disableScreensaver: Binding<Bool> {
        Binding {
            self.config.disableScreensaver
        } set: {
            self.config.disableScreensaver = $0
            self.objectWillChange.send()
        }
    }

    var playBootSound: Binding<Bool> {
        Binding {
            self.config.playBootSound
        } set: {
            self.config.playBootSound = $0
            self.objectWillChange.send()
        }
    }
    
    var precompiledShadersBinding: Binding<ObjCPrecompiledShaderOption> {
        Binding {
            self.config.precompiledShaders
        } set: {
            self.config.precompiledShaders = $0
            self.objectWillChange.send()
        }
    }

    var precompiledShaders: Binding<Bool> {
        precompiledShadersBinding.map(
            get: { $0 != .disable },
            set: { $0 ? .auto : .disable }
        )
    }

    var vsync: Binding<Bool> {
        Binding {
            self.config.vsync != 0
        } set: {
            self.config.vsync = $0 ? 1 : 0
            self.objectWillChange.send()
        }
    }

    var gx2DrawDoneSync: Binding<Bool> {
        Binding {
            self.config.gx2DrawDoneSync
        } set: {
            self.config.gx2DrawDoneSync = $0
            self.objectWillChange.send()
        }
    }

    var renderUpsideDown: Binding<Bool> {
        Binding {
            self.config.renderUpsideDown
        } set: {
            self.config.renderUpsideDown = $0
            self.objectWillChange.send()
        }
    }

    var asyncCompile: Binding<Bool> {
        Binding {
            self.config.asyncCompile
        } set: {
            self.config.asyncCompile = $0
            self.objectWillChange.send()
        }
    }

    var upscaleFilter: Binding<UpscalingFilter> {
        Binding {
            UpscalingFilter(self.config.upscaleFilter)
        } set: {
            self.config.upscaleFilter = $0.config
            self.objectWillChange.send()
        }
    }

    var downscaleFilter: Binding<UpscalingFilter> {
        Binding {
            UpscalingFilter(self.config.downscaleFilter)
        } set: {
            self.config.downscaleFilter = $0.config
            self.objectWillChange.send()
        }
    }

    var fullscreenScaling: Binding<FullscreenScaling> {
        Binding {
            FullscreenScaling(self.config.fullscreenScaling)
        } set: {
            self.config.fullscreenScaling = $0.config
            self.objectWillChange.send()
        }
    }

    var textureResolutionScale: Binding<Double> {
        Binding {
            Double(self.config.textureResolutionScale)
        } set: {
            self.config.textureResolutionScale = Float($0)
            self.objectWillChange.send()
        }
    }

    var vkAccurateBarriers: Binding<Bool> {
        Binding {
            self.config.vkAccurateBarriers
        } set: {
            self.config.vkAccurateBarriers = $0
            self.objectWillChange.send()
        }
    }

    var disableStreamout: Binding<Bool> {
        Binding {
            self.config.disableStreamout
        } set: {
            self.config.disableStreamout = $0
            self.objectWillChange.send()
        }
    }

    var overrideAppGammaPreference: Binding<Bool> {
        Binding {
            self.config.overrideAppGammaPreference
        } set: {
            self.config.overrideAppGammaPreference = $0
            self.objectWillChange.send()
        }
    }

    var overrideGammaValue: Binding<Double> {
        Binding {
            Double(self.config.overrideGammaValue)
        } set: {
            self.config.overrideGammaValue = Float($0)
            self.objectWillChange.send()
        }
    }

    var userDisplayGamma: Binding<Double> {
        Binding {
            Double(self.config.userDisplayGamma)
        } set: {
            self.config.userDisplayGamma = Float($0)
            self.objectWillChange.send()
        }
    }

    var forceMeshShaders: Binding<Bool> {
        Binding {
            self.config.forceMeshShaders
        } set: {
            self.config.forceMeshShaders = $0
            self.objectWillChange.send()
        }
    }

    var framebufferFetch: Binding<Bool> {
        Binding {
            self.config.framebufferFetch
        } set: {
            self.config.framebufferFetch = $0
            self.objectWillChange.send()
        }
    }

    var gpuCaptureDir: Binding<String> {
        Binding {
            self.config.gpuCaptureDir
        } set: {
            self.config.gpuCaptureDir = $0
            self.objectWillChange.send()
        }
    }

    var consoleLanguage: Binding<ConsoleLanguage> {
        Binding {
            ConsoleLanguage(self.config.consoleLanguage)
        } set: {
            self.config.consoleLanguage = $0.config
            self.objectWillChange.send()
        }
    }

    var audioDelayString: Binding<String> {
        intStringBinding(
            get: { Int(self.config.audioDelay) },
            set: { self.config.audioDelay = Int32(max(0, $0)) }
        )
    }

    var audioDelay: Binding<Int> {
        Binding {
            Int(self.config.audioDelay)
        } set: {
            self.config.audioDelay = Int32($0)
            self.objectWillChange.send()
        }
    }

    var microphoneEnabled: Binding<Bool> {
        Binding {
            self.config.microphoneEnabled
        } set: {
            self.config.microphoneEnabled = $0
            self.objectWillChange.send()
        }
    }

    var tvAudioEnabled: Binding<Bool> {
        Binding {
            self.config.tvAudioEnabled
        } set: {
            self.config.tvAudioEnabled = $0
            self.objectWillChange.send()
        }
    }

    var padAudioEnabled: Binding<Bool> {
        Binding {
            self.config.padAudioEnabled
        } set: {
            self.config.padAudioEnabled = $0
            self.objectWillChange.send()
        }
    }

    var tvChannels: Binding<AudioChannels> {
        Binding {
            AudioChannels(self.config.tvChannels)
        } set: {
            self.config.tvChannels = $0.config
            self.objectWillChange.send()
        }
    }

    var padChannels: Binding<AudioChannels> {
        Binding {
            AudioChannels(self.config.padChannels)
        } set: {
            self.config.padChannels = $0.config
            self.objectWillChange.send()
        }
    }

    var inputChannels: Binding<AudioChannels> {
        Binding {
            AudioChannels(self.config.inputChannels)
        } set: {
            self.config.inputChannels = $0.config
            self.objectWillChange.send()
        }
    }

    var tvVolume: Binding<Double> {
        Binding {
            Double(self.config.tvVolume)
        } set: {
            self.config.tvVolume = Int32($0)
            self.objectWillChange.send()
        }
    }

    var padVolume: Binding<Double> {
        Binding {
            Double(self.config.padVolume)
        } set: {
            self.config.padVolume = Int32($0)
            self.objectWillChange.send()
        }
    }

    var inputVolume: Binding<Double> {
        Binding {
            Double(self.config.inputVolume)
        } set: {
            self.config.inputVolume = Int32($0)
            self.objectWillChange.send()
        }
    }

    var portalVolume: Binding<Double> {
        Binding {
            Double(self.config.portalVolume)
        } set: {
            self.config.portalVolume = Int32($0)
            self.objectWillChange.send()
        }
    }

    var tvDevice: Binding<String> {
        Binding {
            self.config.tvDevice
        } set: {
            self.config.tvDevice = $0
            self.objectWillChange.send()
        }
    }

    var padDevice: Binding<String> {
        Binding {
            self.config.padDevice
        } set: {
            self.config.padDevice = $0
            self.objectWillChange.send()
        }
    }

    var inputDevice: Binding<String> {
        Binding {
            self.config.inputDevice
        } set: {
            self.config.inputDevice = $0
            self.objectWillChange.send()
        }
    }

    var portalDevice: Binding<String> {
        Binding {
            self.config.portalDevice
        } set: {
            self.config.portalDevice = $0
            self.objectWillChange.send()
        }
    }

    var overlayPosition: Binding<ScreenPosition> {
        Binding {
            ScreenPosition(self.config.overlayPosition)
        } set: {
            self.config.overlayPosition = $0.config
            self.objectWillChange.send()
        }
    }

    var overlayTextColorHex: Binding<String> {
        colorHexBinding(
            get: { self.config.overlayTextColor },
            set: { self.config.overlayTextColor = $0 }
        )
    }

    var overlayTextScale: Binding<Double> {
        Binding {
            Double(self.config.overlayTextScale)
        } set: {
            self.config.overlayTextScale = Int32($0)
            self.objectWillChange.send()
        }
    }

    var overlayFPS: Binding<Bool> {
        Binding {
            self.config.overlayFPS
        } set: {
            self.config.overlayFPS = $0
            self.objectWillChange.send()
        }
    }

    var overlayCPUMode: Binding<Bool> {
        Binding {
            self.config.overlayCPUMode
        } set: {
            self.config.overlayCPUMode = $0
            self.objectWillChange.send()
        }
    }

    var overlayDrawcalls: Binding<Bool> {
        Binding {
            self.config.overlayDrawcalls
        } set: {
            self.config.overlayDrawcalls = $0
            self.objectWillChange.send()
        }
    }

    var overlayCPUUsage: Binding<Bool> {
        Binding {
            self.config.overlayCPUUsage
        } set: {
            self.config.overlayCPUUsage = $0
            self.objectWillChange.send()
        }
    }

    var overlayCPUPerCoreUsage: Binding<Bool> {
        Binding {
            self.config.overlayCPUPerCoreUsage
        } set: {
            self.config.overlayCPUPerCoreUsage = $0
            self.objectWillChange.send()
        }
    }

    var overlayRAMUsage: Binding<Bool> {
        Binding {
            self.config.overlayRAMUsage
        } set: {
            self.config.overlayRAMUsage = $0
            self.objectWillChange.send()
        }
    }

    var overlayVRAMUsage: Binding<Bool> {
        Binding {
            self.config.overlayVRAMUsage
        } set: {
            self.config.overlayVRAMUsage = $0
            self.objectWillChange.send()
        }
    }

    var overlayDebug: Binding<Bool> {
        Binding {
            self.config.overlayDebug
        } set: {
            self.config.overlayDebug = $0
            self.objectWillChange.send()
        }
    }

    var dumpGraphicsAssets: Binding<Bool> {
        Binding {
            self.config.dumpGraphicsAssets
        } set: {
            self.config.dumpGraphicsAssets = $0
            self.objectWillChange.send()
        }
    }

    var notificationPosition: Binding<ScreenPosition> {
        Binding {
            ScreenPosition(self.config.notificationPosition)
        } set: {
            self.config.notificationPosition = $0.config
            self.objectWillChange.send()
        }
    }

    var notificationTextColorHex: Binding<String> {
        colorHexBinding(
            get: { self.config.notificationTextColor },
            set: { self.config.notificationTextColor = $0 }
        )
    }

    var notificationTextScale: Binding<Double> {
        Binding {
            Double(self.config.notificationTextScale)
        } set: {
            self.config.notificationTextScale = Int32($0)
            self.objectWillChange.send()
        }
    }

    var notificationControllerProfiles: Binding<Bool> {
        Binding {
            self.config.notificationControllerProfiles
        } set: {
            self.config.notificationControllerProfiles = $0
            self.objectWillChange.send()
        }
    }

    var notificationControllerBattery: Binding<Bool> {
        Binding {
            self.config.notificationControllerBattery
        } set: {
            self.config.notificationControllerBattery = $0
            self.objectWillChange.send()
        }
    }

    var notificationShaderCompiling: Binding<Bool> {
        Binding {
            self.config.notificationShaderCompiling
        } set: {
            self.config.notificationShaderCompiling = $0
            self.objectWillChange.send()
        }
    }

    var notificationFriends: Binding<Bool> {
        Binding {
            self.config.notificationFriends
        } set: {
            self.config.notificationFriends = $0
            self.objectWillChange.send()
        }
    }

    var dsuHost: Binding<String> {
        Binding {
            self.config.dsuHost
        } set: {
            self.config.dsuHost = $0
            self.objectWillChange.send()
        }
    }

    var dsuPortString: Binding<String> {
        intStringBinding(
            get: { Int(self.config.dsuPort) },
            set: { self.config.dsuPort = UInt16(min(max(0, $0), Int(UInt16.max))) }
        )
    }

    var disableMotion: Binding<Bool> {
        Binding {
            self.config.disableMotion
        } set: {
            self.config.disableMotion = $0
            self.objectWillChange.send()
        }
    }

    var crashDump: Binding<CrashDump> {
        Binding {
            CrashDump(self.config.crashDump)
        } set: {
            self.config.crashDump = $0.config
            self.objectWillChange.send()
        }
    }

    var gdbPortString: Binding<String> {
        intStringBinding(
            get: { Int(self.config.gdbPort) },
            set: { self.config.gdbPort = UInt16(min(max(0, $0), Int(UInt16.max))) }
        )
    }

    var legacyOnlineEnabled: Binding<Bool> {
        Binding {
            self.config.legacyOnlineEnabled
        } set: {
            self.config.legacyOnlineEnabled = $0
            self.objectWillChange.send()
        }
    }

    var onlinePlayEnabled: Binding<Bool> {
        Binding {
            self.networkService.wrappedValue != .offline
        } set: { enabled in
            if enabled {
                if self.networkService.wrappedValue == .offline {
                    self.networkService.wrappedValue = .pretendo
                } else {
                    self.config.legacyOnlineEnabled = true
                    self.objectWillChange.send()
                }
            } else {
                self.networkService.wrappedValue = .offline
            }
        }
    }

    var networkService: Binding<NetworkService> {
        Binding {
            if !self.isOnlineFullyValid(for: self.currentAccountPersistentId) {
                return .offline
            }

            return NetworkService(self.config.networkService(forPersistentId: self.currentAccountPersistentId))
        } set: { service in
            guard self.isOnlineFullyValid(for: self.currentAccountPersistentId) else {
                self.objectWillChange.send()
                return
            }

            self.config.setNetworkService(service.config, forPersistentId: self.currentAccountPersistentId)
            self.objectWillChange.send()
        }
    }

    private var currentAccountPersistentId: UInt32 {
        activeAccountPersistentId == 0 ? config.activeAccountPersistentId : activeAccountPersistentId
    }

    private func colorHexBinding(get: @escaping () -> UInt32, set: @escaping (UInt32) -> Void) -> Binding<String> {
        Binding {
            String(format: "#%08X", get())
        } set: { value in
            let cleaned = value
                .trimmingCharacters(in: .whitespacesAndNewlines)
                .replacingOccurrences(of: "#", with: "")

            guard let color = UInt32(cleaned, radix: 16) else {
                return
            }

            if cleaned.count == 6 {
                set(0xFF000000 | color)
            } else if cleaned.count == 8 {
                set(color)
            } else {
                return
            }

            self.objectWillChange.send()
        }
    }

    private func intStringBinding(get: @escaping () -> Int, set: @escaping (Int) -> Void) -> Binding<String> {
        Binding {
            String(get())
        } set: { value in
            guard let intValue = Int(value.trimmingCharacters(in: .whitespacesAndNewlines)) else {
                return
            }

            set(intValue)
            self.objectWillChange.send()
        }
    }

    var emulateSkylanderPortal: Binding<Bool> {
        Binding {
            self.config.emulateSkylanderPortal
        } set: {
            self.config.emulateSkylanderPortal = $0
            self.objectWillChange.send()
        }
    }

    var emulateInfinityBase: Binding<Bool> {
        Binding {
            self.config.emulateInfinityBase
        } set: {
            self.config.emulateInfinityBase = $0
            self.objectWillChange.send()
        }
    }

    var emulateDimensionsToypad: Binding<Bool> {
        Binding {
            self.config.emulateDimensionsToypad
        } set: {
            self.config.emulateDimensionsToypad = $0
            self.objectWillChange.send()
        }
    }
}

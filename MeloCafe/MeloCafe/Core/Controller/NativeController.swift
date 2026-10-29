//
//  NativeController.swift
//  MeloCafe
//
//  Created by Stossy11 on 9/3/2026.
//

import Foundation
import GameController
import CoreHaptics

final class NativeController {
    let gc: GCController
    private(set) var coreHandle: UnsafeMutableRawPointer?
    nonisolated private let deviceMotion: DeviceMotionSource
    private let motionConsumerID = UUID()
    private var motionSource: ControllerMotionSource = .controller
    private var hapticEngine: CHHapticEngine?
    private var hapticPlayer: CHHapticAdvancedPatternPlayer?
    nonisolated(unsafe) private var attachmentGeneration: UInt64 = 0
    
    nonisolated private let stateLock = NSLock()
    nonisolated private let motionLock = NSLock()
    nonisolated(unsafe) private var _state  = GCBridgeControllerState()
    nonisolated(unsafe) private var _motion = GCBridgeMotionState()
    
    public var controllerType: ControllerType = .Pro
    
    init(gc: GCController) {
        self.gc = gc
        deviceMotion = .shared
        gc.handlerQueue = .main
        setupHaptics()
    }
    
    deinit {
        detach()
    }
    
    public func detach() {
        deviceMotion.stop(for: motionConsumerID)
        gc.extendedGamepad?.valueChangedHandler = nil
        gc.motion?.valueChangedHandler = nil
        
        if let motion = gc.motion, motion.sensorsRequireManualActivation {
            motion.sensorsActive = false
        }
        
        stateLock.lock()
        
        attachmentGeneration &+= 1
        _state = GCBridgeControllerState()
        
        stateLock.unlock()
        
        
        motionLock.lock()
        _motion = GCBridgeMotionState()
        motionLock.unlock()
        
        
        let handle = coreHandle
        coreHandle = nil
        if let handle { GCControllerBridge_remove(handle) }
        
        stopHaptic()
    }
    
    public func registerWithCore(_ controllerType: ControllerType, motionSource: ControllerMotionSource) {
        guard controllerType != .MAX else { return }
        self.controllerType = controllerType
        if let handle = coreHandle, self.motionSource == motionSource {
            GCControllerBridge_configure(handle, controllerType.rawValue)
            return
        }
        
        detach()
        self.motionSource = motionSource
        if let pad = gc.extendedGamepad { updateState(from: pad) }
        
        let ctx = Unmanaged.passRetained(self).toOpaque()
        
        var desc = GCBridgeControllerDesc()
        desc.context = ctx
        
        self.controllerType = controllerType
        desc.controllerType = controllerType.rawValue
        
        desc.poll_state = { ctx in
            let me = Unmanaged<NativeController>.fromOpaque(ctx!).takeUnretainedValue()
            me.stateLock.lock()
            defer { me.stateLock.unlock() }
            return me._state
        }
        
        if motionSource == .device {
            if deviceMotion.isAvailable {
                desc.poll_motion = { ctx in
                    let me = Unmanaged<NativeController>.fromOpaque(ctx!).takeUnretainedValue()
                    return me.deviceMotion.sample()
                }
            }
        } else if gc.motion?.hasRotationRate == true {
            desc.poll_motion = { ctx in NativeController.pollMotion(ctx) }
        }
        if gc.haptics != nil {
            desc.rumble = { ctx, start in NativeController.rumble(ctx, start) }
        }
        
        desc.release = { ctx in
            Unmanaged<NativeController>.fromOpaque(ctx!).release()
        }
        
        let name = gc.vendorName ?? "MFi Controller"
        name.withCString { ptr in
            desc.display_name = ptr
            coreHandle = GCControllerBridge_add(&desc)
        }
        
        if coreHandle == nil {
            Unmanaged<NativeController>.fromOpaque(ctx).release()
            return
        }
        
        let generation = attachmentGeneration
        gc.extendedGamepad?.valueChangedHandler = { [weak self] pad, _ in
            guard let self, self.coreHandle != nil,
                  self.attachmentGeneration == generation else { return }
            self.updateState(from: pad)
        }
        
        if motionSource == .device {
            deviceMotion.start(for: motionConsumerID)
        } else if let motion = gc.motion, motion.hasRotationRate {
            motion.valueChangedHandler = { [weak self] m in
                guard let self, self.coreHandle != nil,
                      self.attachmentGeneration == generation else { return }
                self.updateMotion(from: m)
            }
            
            if motion.sensorsRequireManualActivation {
                motion.sensorsActive = true
            }
        }
    }
    
    private nonisolated static func pollMotion(_ ctx: UnsafeMutableRawPointer?) -> GCBridgeMotionState {
        let me = Unmanaged<NativeController>.fromOpaque(ctx!).takeUnretainedValue()
        
        me.motionLock.lock()
        defer { me.motionLock.unlock() }
        
        return me._motion
    }
    
    private nonisolated static func rumble(
        _ ctx: UnsafeMutableRawPointer?,
        _ start: Bool
    ) {
        let me = Unmanaged<NativeController>.fromOpaque(ctx!).takeUnretainedValue()
        
        me.stateLock.lock()
        let generation = me.attachmentGeneration
        me.stateLock.unlock()
        
        Task { @MainActor in
            guard me.coreHandle != nil, me.attachmentGeneration == generation,
                  me.hapticEngine != nil else { return }
            
            if start {
                me.startHaptic()
            } else {
                me.stopHaptic()
            }
        }
    }
    
    private func updateState(from pad: GCExtendedGamepad) {
        var s = GCBridgeControllerState()
        
        func bit(_ pressed: Bool, _ bit: Int) { if pressed { s.buttons |= 1 << bit } }
        bit(pad.buttonA.isPressed, 0)
        bit(pad.buttonB.isPressed, 1)
        bit(pad.buttonX.isPressed, 2)
        bit(pad.buttonY.isPressed, 3)
        bit(pad.leftShoulder.isPressed, 4)
        bit(pad.rightShoulder.isPressed, 5)
        bit(pad.leftTrigger.isPressed, 6)
        bit(pad.rightTrigger.isPressed, 7)
        bit(pad.buttonOptions?.isPressed ?? false, 8)
        bit(pad.buttonMenu.isPressed, 9)
        bit(pad.leftThumbstickButton?.isPressed  ?? false, 10)
        bit(pad.rightThumbstickButton?.isPressed ?? false, 11)
        bit(pad.dpad.up.isPressed,    16)
        bit(pad.dpad.down.isPressed,  17)
        bit(pad.dpad.left.isPressed,  18)
        bit(pad.dpad.right.isPressed, 19)
        
        s.leftStick = GCBridgeVec2(x: pad.leftThumbstick.xAxis.value, y: pad.leftThumbstick.yAxis.value)
        
        s.rightStick = GCBridgeVec2(x: pad.rightThumbstick.xAxis.value, y: pad.rightThumbstick.yAxis.value)
        s.leftTrigger = pad.leftTrigger.value
        s.rightTrigger = pad.rightTrigger.value
        
        stateLock.lock()
        _state = s
        stateLock.unlock()
    }
    
    private func updateMotion(from m: GCMotion) {
        var mo = GCBridgeMotionState()
        
        let converted = DeviceMotionCoordinates.convert(
            gyro: SIMD3(Float(m.rotationRate.x), Float(m.rotationRate.y), Float(m.rotationRate.z)),
            acceleration: SIMD3(Float(m.acceleration.x), Float(m.acceleration.y), Float(m.acceleration.z)),
            quarterTurns: 0)
        let a = converted.acceleration, g = converted.gyro

        mo.accelerometer = GCBridgeVec3(x: a.x, y: a.y, z: a.z)
        mo.gyroscope = GCBridgeVec3(x: g.x, y: g.y, z: g.z)
        
        mo.orientation = GCBridgeVec3(x: 0, y: 0, z: 0)
        mo.quaternion  = GCBridgeQuat(w: 1, x: 0, y: 0, z: 0)
        
        mo.timestamp = ProcessInfo.processInfo.systemUptime
        
        motionLock.lock()
        _motion = mo
        motionLock.unlock()
    }
    
    private func setupHaptics() {
        guard let engine = gc.haptics?.createEngine(withLocality: .default) else { return }
        engine.resetHandler = { [weak self] in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.hapticPlayer = nil
                try? self.hapticEngine?.start()
            }
        }
        
        try? engine.start()
        hapticEngine = engine
    }
    
    private func startHaptic() {
        guard let engine = hapticEngine else { return }
        
        try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        hapticPlayer = nil
        
        let params: [CHHapticEventParameter] = [
            .init(parameterID: .hapticIntensity, value: 1.0),
            .init(parameterID: .hapticSharpness, value: 0.5),
        ]
        
        let event = CHHapticEvent(eventType: .hapticContinuous,
                                  parameters: params,
                                  relativeTime: 0,
                                  duration: 1.0)
        
        guard let pattern = try? CHHapticPattern(events: [event], parameters: []),
              let player  = try? engine.makeAdvancedPlayer(with: pattern) else { return }
        
        player.loopEnabled = true
        try? player.start(atTime: CHHapticTimeImmediate)
        hapticPlayer = player
    }
    
    private func stopHaptic() {
        try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        hapticPlayer = nil
    }
}

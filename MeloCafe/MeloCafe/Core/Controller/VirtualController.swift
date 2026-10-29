//
//  VirtualController.swift
//  MeloCafe
//
//  Created by Stossy11 on 9/3/2026.
//

import Melo_Controller
import Foundation
import Combine
import CoreMotion
import UIKit

class VirtualControllerHandler: Melo_Controller.Controller {
    let virtualController: VirtualController = ControllerManager.shared.virtualController

    func buttonPressed(_ button: Melo_Controller.VirtualControllerButton) {
        virtualController.pressButton(button)
    }

    func buttonReleased(_ button: Melo_Controller.VirtualControllerButton) {
        virtualController.releaseButton(button)
    }

    func joystickMoved(position: CGPoint, right: Bool) {
        virtualController.setThumbstick(position, right: right)
    }
}

final class VirtualController {
    private(set) var coreHandle: UnsafeMutableRawPointer?
    
    private var pressedButtons: Set<VirtualControllerButton> = []
    private var leftThumbstick:  CGPoint = .zero
    private var rightThumbstick: CGPoint = .zero
    
    var attached: Bool = false
    
    nonisolated private let stateLock = NSLock()
    nonisolated private let motionLock = NSLock()
    
    nonisolated(unsafe) private var _state  = GCBridgeControllerState()
    nonisolated(unsafe) private var _motion = GCBridgeMotionState()
    
    private let motionManager = CMMotionManager()
    private let motionQueue = OperationQueue.main
    private var motionGeneration: UInt64 = 0
    
    private var deviceOrientation: UIDeviceOrientation = .portrait
    private var orientationObserver: NSObjectProtocol?
    
    public var controllerType: ControllerType = .VPAD
    
    deinit {
        detach()
    }
    
    func detach() {
        attached = false
        stopMotion()
        let handle = coreHandle
        coreHandle = nil
        if let handle { GCControllerBridge_remove(handle) }
        pressedButtons.removeAll()
        leftThumbstick = .zero
        rightThumbstick = .zero
        updateState()
        motionLock.lock()
        _motion = GCBridgeMotionState()
        motionLock.unlock()
    }
    
    public func registerWithCore(_ controllerType: ControllerType) {
        guard controllerType != .MAX else { return }
        self.controllerType = controllerType
        if let handle = coreHandle {
            GCControllerBridge_configure(handle, controllerType.rawValue)
            return
        }
        detach()
        
        let ctx = Unmanaged.passRetained(self).toOpaque()
        
        var desc = GCBridgeControllerDesc()
        desc.context = ctx
        
        self.controllerType = controllerType
        desc.controllerType = controllerType.rawValue
        
        desc.poll_state = { ctx in
            let me = Unmanaged<VirtualController>.fromOpaque(ctx!).takeUnretainedValue()
            me.stateLock.lock()
            defer { me.stateLock.unlock() }
            return me._state
        }
        
        if motionManager.isDeviceMotionAvailable {
            desc.poll_motion = { ctx in
                let me = Unmanaged<VirtualController>.fromOpaque(ctx!).takeUnretainedValue()
                me.motionLock.lock()
                defer { me.motionLock.unlock() }
                return me._motion
            }
        }
        
        desc.rumble = nil
        
        desc.release = { ctx in
            Unmanaged<VirtualController>.fromOpaque(ctx!).release()
        }
        
        "MeloCafe Virtual Controller".withCString { ptr in
            desc.display_name = ptr
            coreHandle = GCControllerBridge_add(&desc)
        }
        
        guard coreHandle != nil else {
            Unmanaged<VirtualController>.fromOpaque(ctx).release()
            return
        }
        
        attached = true
        startMotion()
    }
    
    private func startMotion() {
        guard motionManager.isDeviceMotionAvailable else { return }
        
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        let initial = UIDevice.current.orientation
        if initial.isLandscape || initial.isPortrait { deviceOrientation = initial }
        orientationObserver = NotificationCenter.default.addObserver(
            forName: UIDevice.orientationDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            let orientation = UIDevice.current.orientation
            if orientation.isLandscape || orientation.isPortrait {
                self?.deviceOrientation = orientation
            }
        }
        let generation = motionGeneration
        motionManager.deviceMotionUpdateInterval = 1.0 / 60.0
        
        motionManager.startDeviceMotionUpdates(
            using: .xArbitraryZVertical,
            to: motionQueue
        ) { [weak self] data, _ in
            guard let self, let data, self.attached,
                  self.motionGeneration == generation else { return }
            self.updateMotion(from: data)
        }
    }
    
    private func stopMotion() {
        motionGeneration &+= 1
        motionManager.stopDeviceMotionUpdates()
        if let observer = orientationObserver {
            NotificationCenter.default.removeObserver(observer)
            orientationObserver = nil
            UIDevice.current.endGeneratingDeviceOrientationNotifications()
        }
    }
    
    private func updateMotion(from m: CMDeviceMotion) {
        var mo = GCBridgeMotionState()
        
        let gx = Float(m.rotationRate.x)
        let gy = Float(m.rotationRate.y)
        let gz = Float(m.rotationRate.z)
        let ax = Float(m.gravity.x + m.userAcceleration.x)
        let ay = Float(m.gravity.y + m.userAcceleration.y)
        let az = Float(m.gravity.z + m.userAcceleration.z)
        
        let quarterTurns: Int
        switch deviceOrientation {
        case .portrait, .unknown, .faceUp, .faceDown: quarterTurns = 0
        case .landscapeLeft: quarterTurns = 1
        case .landscapeRight: quarterTurns = -1
        case .portraitUpsideDown: quarterTurns = 2
        @unknown default: return
        }
        let converted = DeviceMotionCoordinates.convert(
            gyro: SIMD3(gx, gy, gz), acceleration: SIMD3(ax, ay, az), quarterTurns: quarterTurns)
        let g = converted.gyro, a = converted.acceleration
        mo.gyroscope     = GCBridgeVec3(x: g.x, y: g.y, z: g.z)
        mo.accelerometer = GCBridgeVec3(x: a.x, y: a.y, z: a.z)

        mo.orientation = GCBridgeVec3(x: 0, y: 0, z: 0)
        mo.quaternion  = GCBridgeQuat(w: 1, x: 0, y: 0, z: 0)
        mo.timestamp = m.timestamp
        
        motionLock.lock()
        _motion = mo
        motionLock.unlock()
    }
    
    public func setThumbstick(_ value: CGPoint, right: Bool) {
        guard attached else { return }
        right ? (rightThumbstick = value) : (leftThumbstick = value)
        updateState()
    }
    
    public func pressButton(_ button: VirtualControllerButton) {
        guard attached else { return }
        pressedButtons.insert(button)
        updateState()
    }
    
    public func releaseButton(_ button: VirtualControllerButton) {
        guard attached else { return }
        pressedButtons.remove(button)
        updateState()
    }
    
    private func updateState() {
        var s = GCBridgeControllerState()
        
        func pressed(_ b: VirtualControllerButton) -> Bool { pressedButtons.contains(b) }
        func bit(_ p: Bool, _ bit: Int) { if p { s.buttons |= 1 << bit } }
        
        bit(pressed(.A), 0)
        bit(pressed(.B), 1)
        bit(pressed(.X), 2)
        bit(pressed(.Y), 3)
        bit(pressed(.leftShoulder), 4)
        bit(pressed(.rightShoulder), 5)
        bit(pressed(.leftTrigger), 6)
        bit(pressed(.rightTrigger), 7)
        bit(pressed(.back), 8)
        bit(pressed(.start), 9)
        bit(pressed(.leftStick), 10)
        bit(pressed(.rightStick), 11)
        bit(pressed(.dPadUp), 16)
        bit(pressed(.dPadDown), 17)
        bit(pressed(.dPadLeft), 18)
        bit(pressed(.dPadRight), 19)
        
        s.leftStick  = GCBridgeVec2(x:  Float(leftThumbstick.x), y: -Float(leftThumbstick.y))
        s.rightStick = GCBridgeVec2(x:  Float(rightThumbstick.x), y: -Float(rightThumbstick.y))
        s.leftTrigger  = pressed(.leftTrigger)  ? 1 : 0
        s.rightTrigger = pressed(.rightTrigger) ? 1 : 0
        
        stateLock.lock()
        _state = s
        stateLock.unlock()
    }
}

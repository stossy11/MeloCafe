//
//  VirtualController.swift
//  MeloCafe
//
//  Created by Stossy11 on 9/3/2026.
//

import Melo_Controller
import Foundation
import Combine
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
    
    nonisolated(unsafe) private var _state  = GCBridgeControllerState()
    
    nonisolated private let deviceMotion: DeviceMotionSource
    private let motionConsumerID = UUID()

    init() {
        deviceMotion = .shared
    }

    public var controllerType: ControllerType = .VPAD
    
    deinit {
        detach()
    }
    
    func detach() {
        attached = false
        deviceMotion.stop(for: motionConsumerID)
        let handle = coreHandle
        coreHandle = nil
        if let handle { GCControllerBridge_remove(handle) }
        pressedButtons.removeAll()
        leftThumbstick = .zero
        rightThumbstick = .zero
        updateState()
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
        
        if deviceMotion.isAvailable {
            desc.poll_motion = { ctx in
                let me = Unmanaged<VirtualController>.fromOpaque(ctx!).takeUnretainedValue()
                return me.deviceMotion.sample()
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
        deviceMotion.start(for: motionConsumerID)
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

import CoreMotion
import Foundation
import UIKit

// One sensor stream can serve both touch controls and physical controllers.
// Consumers own independent subscriptions so detaching one doesn't stop the others.
final class DeviceMotionSource {
    static let shared = DeviceMotionSource()

    private let motionManager = CMMotionManager()
    private let motionQueue = OperationQueue.main
    private var consumers: Set<UUID> = []
    private var motionGeneration: UInt64 = 0
    private var deviceOrientation: UIDeviceOrientation = .portrait
    private var orientationObserver: NSObjectProtocol?

    nonisolated private let motionLock = NSLock()
    nonisolated(unsafe) private var _motion = GCBridgeMotionState()

    private init() {}

    var isAvailable: Bool { motionManager.isDeviceMotionAvailable }

    nonisolated func sample() -> GCBridgeMotionState {
        motionLock.lock()
        defer { motionLock.unlock() }
        return _motion
    }

    func start(for consumer: UUID) {
        guard isAvailable, consumers.insert(consumer).inserted,
              consumers.count == 1 else { return }
        
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
            guard let self, let data, !self.consumers.isEmpty,
                  self.motionGeneration == generation else { return }
            self.updateMotion(from: data)
        }
    }
    
    func stop(for consumer: UUID) {
        guard consumers.remove(consumer) != nil, consumers.isEmpty else { return }
        motionGeneration &+= 1
        motionManager.stopDeviceMotionUpdates()
        if let observer = orientationObserver {
            NotificationCenter.default.removeObserver(observer)
            orientationObserver = nil
            UIDevice.current.endGeneratingDeviceOrientationNotifications()
        }
        motionLock.lock()
        _motion = GCBridgeMotionState()
        motionLock.unlock()
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
    
}

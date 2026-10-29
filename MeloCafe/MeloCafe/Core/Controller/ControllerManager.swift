//
//  ControllerManager.swift
//  MeloCafe
//
//  Created by Stossy11 on 9/3/2026.
//

import Foundation
import GameController
import Combine
import SwiftUI

enum ControllerMotionSource: String, CaseIterable {
    case controller
    case device

    var name: String {
        switch self {
        case .controller: return "Controller"
        case .device: return "This Device"
        }
    }
}

struct ControllerEntry: Identifiable {
    let id: UUID
    let name: String
    let isVirtual: Bool
    let hasMotion: Bool
    let hasRumble: Bool
    var controllerType: ControllerType = .VPAD

    fileprivate var nativeController: NativeController?

    init(id: UUID = UUID(), name: String, isVirtual: Bool, hasMotion: Bool, hasRumble: Bool, controllerType: ControllerType = .VPAD, nativeController: NativeController? = nil) {
        self.id = id
        self.name = name
        self.isVirtual = isVirtual
        self.hasMotion = hasMotion
        self.hasRumble = hasRumble
        self.controllerType = controllerType
        self.nativeController = nativeController
    }
}

final class ControllerManager: ObservableObject {
    static let shared = ControllerManager()
    
    let virtualController = VirtualController()
    private let virtualControllerID = UUID()
    private var observers: [NSObjectProtocol] = []
    private var registeredControllers: [ControllerEntry] = []
    private var automaticVirtual = false
    
    @Published private(set) var controllers: [ControllerEntry] = []
    @Published private(set) var allControllers: [ControllerEntry] = []
    @Published var motionSource: ControllerMotionSource = ControllerMotionSource(
        rawValue: UserDefaults.standard.string(forKey: "controllerMotionSource") ?? ""
    ) ?? .controller {
        didSet {
            guard motionSource != oldValue else { return }
            UserDefaults.standard.set(motionSource.rawValue, forKey: "controllerMotionSource")
            synchronizeIfRunning()
        }
    }
    var isRunning = false
    
    private init() {
        observers.append(NotificationCenter.default.addObserver(
            forName: .GCControllerDidConnect, object: nil, queue: .main
        ) { [weak self] notification in
            guard let gc = notification.object as? GCController else { return }
            self?.addNative(gc)
            self?.synchronizeIfRunning()
        })
        
        observers.append(NotificationCenter.default.addObserver(
            forName: .GCControllerDidDisconnect, object: nil, queue: .main
        ) { [weak self] notification in
            guard let self, let gc = notification.object as? GCController else { return }
            self.controllers.removeAll { $0.nativeController?.gc === gc }
            self.allControllers.removeAll { $0.nativeController?.gc === gc }
            if self.controllers.isEmpty { self.addVirtualFallback() }
            self.synchronizeIfRunning()
        })
        
        allControllers.append(ControllerEntry(
            id: virtualControllerID, name: "MeloCafe Virtual Controller",
            isVirtual: true, hasMotion: true, hasRumble: false
        ))
        
        for gc in GCController.controllers() { addNative(gc) }
        if controllers.isEmpty { addVirtualFallback() }
    }
    
    func controllerCount() -> Int { controllers.count }
    func hasVirtual() -> Bool { controllers.contains { $0.isVirtual } }
    
    func canSelectType(_ type: ControllerType, for id: UUID) -> Bool {
        guard type != .MAX else { return false }
        
        let others = controllers.filter { $0.id != id }
        let sameFamily = others.filter { ($0.controllerType == .VPAD) == (type == .VPAD) }
        
        return others.count < 8 && sameFamily.count < (type == .VPAD ? 2 : 7)
    }
    
    func canAdd(id: UUID) -> Bool {
        guard let entry = allControllers.first(where: { $0.id == id }) else { return false }
        
        return !controllers.contains { $0.id == id } && canSelectType(entry.controllerType, for: id)
    }
    
    func searchForControllers() {
        let connected = GCController.controllers()
        let disconnected: (ControllerEntry) -> Bool = { entry in
            guard let gc = entry.nativeController?.gc else { return false }
            return !connected.contains { $0 === gc }
        }
        
        let lostActiveController = controllers.contains(where: disconnected)
        
        controllers.removeAll(where: disconnected)
        allControllers.removeAll(where: disconnected)
        
        for gc in connected { addNative(gc) }
        
        if lostActiveController && controllers.isEmpty { addVirtualFallback() }
        synchronizeIfRunning()
    }
    
    func attachAllToCore() {
        for entry in registeredControllers where !controllers.contains(where: { $0.id == entry.id }) {
            if entry.isVirtual { virtualController.detach() }
            else { entry.nativeController?.detach() }
        }
        
        for entry in controllers {
            if entry.isVirtual { virtualController.registerWithCore(entry.controllerType) }
            else { entry.nativeController?.registerWithCore(entry.controllerType, motionSource: motionSource) }
        }
        
        var handles: [UnsafeMutableRawPointer?] = controllers.compactMap { entry in
            entry.isVirtual ? virtualController.coreHandle : entry.nativeController?.coreHandle
        }.map { Optional($0) }
        
        handles.withUnsafeMutableBufferPointer { buffer in
            GCControllerBridge_setOrder(buffer.baseAddress, buffer.count)
        }
        
        registeredControllers = controllers
        GCControllerBridge_notifyChanged()
    }
    
    func detachAllFromCore() {
        virtualController.detach()
        for entry in registeredControllers { entry.nativeController?.detach() }
        registeredControllers.removeAll()
        GCControllerBridge_notifyChanged()
    }
    
    private func synchronizeIfRunning() {
        if isRunning { attachAllToCore() }
    }
    
    private func addVirtualFallback() {
        guard !hasVirtual(), let entry = allControllers.first(where: { $0.isVirtual }),
              canSelectType(entry.controllerType, for: entry.id) else { return }
        
        controllers.insert(entry, at: 0)
        automaticVirtual = true
    }
    
    func attachVirtual() { addFromAll(id: virtualControllerID) }
    func detachVirtual() { remove(id: virtualControllerID) }
    
    func remove(id: UUID) {
        controllers.removeAll { $0.id == id }
        if id == virtualControllerID { automaticVirtual = false }
        synchronizeIfRunning()
    }
    
    func move(from source: IndexSet, to destination: Int) {
        controllers.move(fromOffsets: source, toOffset: destination)
        
        synchronizeIfRunning()
    }
    
    func setControllerType(id: UUID, to type: ControllerType) {
        guard canSelectType(type, for: id),
              let index = controllers.firstIndex(where: { $0.id == id }) else { return }
        
        controllers[index].controllerType = type
        
        if let allIndex = allControllers.firstIndex(where: { $0.id == id }) {
            allControllers[allIndex].controllerType = type
        }
        
        synchronizeIfRunning()
    }
    
    func missingControllers() -> [ControllerEntry] {
        allControllers.filter { entry in !controllers.contains { $0.id == entry.id } }
    }
    
    func addFromAll(id: UUID) {
        guard canAdd(id: id), let entry = allControllers.first(where: { $0.id == id }) else { return }
        controllers.append(entry)
        if entry.isVirtual { automaticVirtual = false }
        synchronizeIfRunning()
    }
    
    private func addNative(_ gc: GCController) {
        guard gc.extendedGamepad != nil,
              !allControllers.contains(where: { $0.nativeController?.gc === gc }) else { return }
        
        let replacingFallback = automaticVirtual && hasVirtual()
        let active = controllers.filter { !replacingFallback || !$0.isVirtual }
        let type: ControllerType = active.contains { $0.controllerType == .VPAD } ? .Pro : .VPAD
        
        let entry = ControllerEntry(
            name: gc.vendorName ?? "Controller", isVirtual: false,
            hasMotion: gc.motion?.hasRotationRate == true, hasRumble: gc.haptics != nil,
            controllerType: type, nativeController: NativeController(gc: gc)
        )
        
        allControllers.append(entry)
        
        if replacingFallback {
            controllers.removeAll { $0.isVirtual }
            automaticVirtual = false
        }
        
        if canSelectType(type, for: entry.id) { controllers.append(entry) }
    }
}

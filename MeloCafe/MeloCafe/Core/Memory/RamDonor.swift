//
//  RamDonor.swift
//  MeloNX
//
//  Created by Stossy11 on 29/9/2026.
//

import Foundation

enum RamDonor {
    private static let mib: UInt64 = 1024 * 1024
    
    static let enabledPreferenceKey = "ramDonorsEnabled"

    static var isEnabled: Bool {
        get {
            UserDefaults.standard.object(forKey: enabledPreferenceKey) as? Bool ?? true
        }
        set {
            UserDefaults.standard.set(newValue, forKey: enabledPreferenceKey)
        }
    }
    
    static var gpuBuffersEnabled: Bool {
        false
    }
    
    static var killDonorsAfterMapping: Bool {
        get { true }
        set { _ = newValue }
    }
    
    private(set) static var region: UnsafeMutableRawPointer?
    private(set) static var regionSize: UInt64 = 0
    private(set) static var lastStatus: String = "Not attempted yet"
    
    private static var nativeError: String? {
        guard let raw = mlx_ram_donor_last_error() else { return nil }
        
        let text = String(cString: raw)
        
        return text.isEmpty ? nil : text
    }
    
    private static func fail(_ reason: String) -> Bool {
        lastStatus = reason
        print(lastStatus)
        
        return false
    }
    
    @discardableResult
    static func prepare(bytes: UInt64) -> Bool {
        guard bytes > 0 else {
            return fail("Asked for no memory, nothing to do")
        }
        
        guard isEnabled || gpuBuffersEnabled else {
            return fail("Memory donation is turned off in Settings")
        }
        
        guard isSupported else {
            return fail("The memory helper is missing from this build")
        }
        
        guard region == nil else {
            lastStatus = "Already holding \(regionSize / mib) MB"
            return true
        }
        
        let requestedGuest: UInt64 = isEnabled ? bytes : 0
        
        mlx_ram_donor_set_kill_after_mapping(killDonorsAfterMapping)
        
        guard let mapped = mlx_ram_donor_acquire(requestedGuest) else {
            let reason = nativeError ?? "No donor memory available"
            mlx_ram_donor_shutdown()
            return fail("Could not source donor memory: \(reason)")
        }
        
        
        region = mapped
        regionSize = requestedGuest
        
        offer_donated_memory(mapped, requestedGuest)
        
        lastStatus = "Guest RAM: \(requestedGuest / mib) MiB"
        print(lastStatus)
        
        return true
    }
    
    static func teardown() {
        guard region != nil else { return }
        
        withdraw_donated_memory()
        
        if let region {
            mlx_ram_donor_release(region)
        }
        
        mlx_ram_donor_shutdown()
        
        region = nil
        regionSize = 0
    }
    
    
    static var isSupported: Bool {
        mlx_ram_donor_is_available()
    }
}

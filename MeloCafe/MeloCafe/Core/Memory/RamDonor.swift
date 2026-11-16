//
//  RamDonor.swift
//  MeloNX
//
//  Created by Stossy11 on 29/9/2026.
//

import Combine
import Foundation

final class RamDonor: ObservableObject {
    static let shared = RamDonor()
    static let enabledPreferenceKey = "ramDonorsEnabled"
    static let maxAttempts = 3

    @Published private(set) var didFail = false
    
    private var region: DonatedRam?

    var isAvailable: Bool {
        if #available(iOS 26.0, *) {
            return !didFail
        }
        return false
    }

    private init() {}

    func prepare() async -> Bool {
        if region != nil { return true }
        let defaults = UserDefaults.standard
        guard #available(iOS 26.0, *),
              defaults.object(forKey: Self.enabledPreferenceKey) == nil || defaults.bool(forKey: Self.enabledPreferenceKey),
              !didFail else { return false }

        let size = 4 * 1024 * 1024 * 1024
        for attempt in 1...Self.maxAttempts {
            if attempt > 1 {
                try? await Task.sleep(for: .milliseconds(600))
            }
            guard !Task.isCancelled else { return false }

            do {
                let donation = try await ExtensionRamDonor.acquire(size: size)
                guard offer_donated_memory(donation.base, UInt64(size)) else {
                    throw RamDonorError("Cemu rejected the donated memory.")
                }
                region = donation
                print("Donated guest RAM: 4 GiB")
                return true
            } catch {
                print("RAM donation attempt \(attempt) of \(Self.maxAttempts) failed. \(error.localizedDescription)")
            }
        }
        didFail = true
        
        return false
    }
}

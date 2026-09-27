//
//  CemuManager.swift
//  MeloCafe
//
//  Created by Stossy11 on 5/3/2026.
//

import Foundation

class CemuManager {
    private static let fileManager = FileManager.default

    static func initialize() {
        if !fileManager.fileExists(atPath: URL.romsURL.path) {
            do {
                try fileManager.createDirectory(at: .romsURL, withIntermediateDirectories: true, attributes: nil)
            } catch {
                print("Failed to create directory: \(URL.romsURL.path), error: \(error)")
            }
        }

        let documents = URL.documentsDirectory

        let executable = Bundle.main.executablePath

        documents.path.withCString { dataPtr in
            executable!.withCString { execPtr in
                CemuInitialize(execPtr, dataPtr, dataPtr, dataPtr, dataPtr)
            }
        }
    }

    nonisolated static func loadGameIcon(_ titleId: UInt64) -> Data? {
        var iconResult = CemuGetGameIcon(titleId)

        guard let iconData = iconResult.data, iconResult.size > 0 else {
            return nil
        }

        let data = Data(bytes: iconData, count: Int(iconResult.size))

        Cemu_FreeGameIcon(&iconResult)

        return data
    }

    static func loadTitle(titleId: UInt64) -> Bool {
        return CemuLoadTitle(titleId)
    }

    static func load(path: String, load: Bool = false) -> UInt64 {
        return path.withCString { ptr in
            return CemuLoadFile(ptr, load)
        }
    }

    static func run() {
        ControllerManager.shared.isRunning = true
        ControllerManager.shared.attachAllToCore()


        Thread.detachNewThread {
            CemuRun()
        }
    }

    static func shutdown() {
        ControllerManager.shared.isRunning = false
        ControllerManager.shared.detachAllFromCore()
        CemuShutdown()
    }
}

extension FileManager {
    func filePath(atPath path: String, withLength length: Int) -> String? {
        guard let file = try? contentsOfDirectory(atPath: path).filter({ $0.count == length }).first else { return nil }
        return "\(path)/\(file)"
    }
}

extension URL {
    @available(iOS, introduced: 14.0, deprecated: 16.0, message: "Use URL.documentsDirectory on iOS 16 and above")
    static var documentsDirectory: URL {
        let documentDirectory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        return documentDirectory
    }
}

enum DeviceCpu {
    case mseries
    case aseries
}

struct ChipInfo {
    let series: DeviceCpu
    let number: Int
}

public extension ProcessInfo {
    var hasTXMClassic: Bool {
        ProcessInfo.processInfo.isiOSAppOnMac ? false :
        { if let boot = FileManager.default.filePath(atPath: "/System/Volumes/Preboot", withLength: 36), let file = FileManager.default.filePath(atPath: "\(boot)/boot", withLength: 96) { return access("\(file)/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4", F_OK) == 0 } else { return (FileManager.default.filePath(atPath: "/private/preboot", withLength: 96).map { access("\($0)/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4", F_OK) == 0 }) ?? false } }()
    }
    
    var hasTXM: Bool {
        if #available(iOS 27, *) {
            let lastNonTXM = 12 // A12
            let chipInfo = parseChipInfo()
            
            if let info = chipInfo, info.series == .aseries {
                return info.number > lastNonTXM
            }
            
            return true
        }
        
        if #available(iOS 26.6, *), !hasTXMClassic {
            let firstTXM = 15 // A15
            let iPadTXM = 2 // M2
            let chipInfo = parseChipInfo()
            
            if let info = chipInfo {
                if info.series == .mseries {
                    return info.number >= iPadTXM
                } else {
                    return info.number >= firstTXM
                }
            }
            
            return false
        }
        
        return hasTXMClassic
    }
    
    private func parseChipInfo() -> ChipInfo? {
        guard let device = MTLCreateSystemDefaultDevice() else { return nil }
        
        let name = device.name.uppercased()
        
        let pattern = "APPLE\\s+([MA])(\\d+)"
        guard let regex = try? NSRegularExpression(pattern: pattern),
            let match = regex.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)),
            let letterRange = Range(match.range(at: 1), in: name),
            let numberRange = Range(match.range(at: 2), in: name),
            let number = Int(name[numberRange])
        else { return nil }
        
        switch name[letterRange] {
        case "M":
            return ChipInfo(series: .mseries, number: number)
        case "A":
            return ChipInfo(series: .aseries, number: number)
        default:
            return nil
        }
    }
}

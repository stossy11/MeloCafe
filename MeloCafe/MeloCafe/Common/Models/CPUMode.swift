//
//  CPUMode.swift
//  MeloCafe
//
//  Created by Stossy11 on 25/4/2026.
//

import Foundation

public enum CPUMode: CaseIterable, Hashable {
    case interpreter(multicore: Bool)
    case recompiler(multicore: Bool)
    case auto
    
    static public var allCases: [CPUMode] {
        [
            .auto,
            .interpreter(multicore: true),
            .recompiler(multicore: true),
            .interpreter(multicore: false),
            .recompiler(multicore: false)
        ]
    }
    
    static public var allCasesAppStore: [CPUMode] {
        [
            .interpreter(multicore: true),
            .interpreter(multicore: false),
        ]
    }

    init(_ cpuMode: ObjCCPUMode) {
        switch cpuMode {
        case .singlecoreInterpreter: self = .interpreter(multicore: false)
        case .multicoreInterpreter: self = .interpreter(multicore: true)
        case .singlecoreRecompiler: self = .recompiler(multicore: false)
        case .multicoreRecompiler: self = .recompiler(multicore: true)
        case .auto: self = .auto
        default: self = .interpreter(multicore: true)
        }
    }

    var config: ObjCCPUMode {
        switch self {
        case .interpreter(let multicore):
            return multicore ? .multicoreInterpreter : .singlecoreInterpreter
        case .recompiler(let multicore):
            return multicore ? .multicoreRecompiler : .singlecoreRecompiler
        case .auto: return .auto
        }
    }
    
    var isInterpreter: Bool {
        switch self {
        case .interpreter:
            return true
        default: return false
        }
    }
    
    var isMulticore: Bool {
        switch self {
        case .interpreter(let multicore):
            return multicore
        case .recompiler(let multicore):
            return multicore
        case .auto: return true
        }
    }

    var string: String {
        switch self {
        case .interpreter(let multicore): return multicore ? "Interpreter" : "Interpreter (Single Core)"
        case .recompiler(let multicore): return multicore ? "Recompiler (JIT)" : "Recompiler (JIT, Single Core)"
        case .auto: return "Auto"
        }
    }
}

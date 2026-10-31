//
//  ContentView.swift
//  MeloCafe
//
//  Created by Stossy11 on 5/3/2026.
//

import SwiftUI
import GameController
import MetalKit
import Melo_Controller
import UniformTypeIdentifiers


struct ContentView: View {
    let cemuView: MetalView
    let cemuPadView: MetalView
    @EnvironmentObject private var gameManager: GamesManager

    var body: some View {
        Group {
            if gameManager.startedEmulation {
                EmulationView(cemuView: cemuView, cemuPadView: cemuPadView)
            } else {
                TabView {
                    GamesListView()
                        .tabItem {
                            Label("Games", systemImage: "house.fill")
                        }

                    SettingsView()
                        .tabItem {
                            Label("Settings", systemImage: "gearshape.fill")
                        }
                }
            }
        }
    }
}

func installJITTrapHandler() {
    var sa = sigaction()
    sa.sa_flags = SA_SIGINFO
    sa.__sigaction_u.__sa_sigaction = { sig, info, context in
        guard let context else { return }
        let uc = context.bindMemory(to: ucontext_t.self, capacity: 1)
        uc.pointee.uc_mcontext.pointee.__ss.__pc += 4
        uc.pointee.uc_mcontext.pointee.__ss.__x.0 = 0
    }
    sigaction(SIGTRAP, &sa, nil)
}

#Preview {
    ContentView(cemuView: MetalView(), cemuPadView: MetalView())
        .environmentObject(GamesManager.shared)
}

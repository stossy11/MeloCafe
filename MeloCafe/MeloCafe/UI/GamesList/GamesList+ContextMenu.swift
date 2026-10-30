//
//  GamesList+ContextMenu.swift
//  MeloCafe
//
//  Created by Stossy11 on 25/4/2026.
//

import SwiftUI

extension GamesListView {
    @ViewBuilder
    func contextMenu(_ game: GameInfoSwift) -> some View {
        Button {
            gamesList.togglePin(game)
        } label: {
            Label(gamesList.isPinned(game) ? "Unpin" : "Pin to Top",
                  systemImage: gamesList.isPinned(game) ? "pin.slash" : "pin")
        }

        Button {
            self.activeSheet = .graphicPacks(game: game)
        } label: {
            Text("Graphic Packs")
        }

    }
}

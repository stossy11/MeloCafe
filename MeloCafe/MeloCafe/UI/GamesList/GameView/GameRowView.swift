//
//  GameRowView.swift
//  MeloCafe
//
//  Created by Stossy11 on 3/4/2026.
//

import SwiftUI

struct GameRowView: View {
    let game: GameInfoSwift
    var isPinned: Bool = false
    let onLaunch: (GameInfoSwift) -> Void
    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        Button {
            onLaunch(game)
        } label: {
            HStack(spacing: 16) {
                gameIcon
                gameInfo
                if isPinned {
                    Spacer()
                    Image(systemName: "pin.fill")
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("Pinned")
                }
            }
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder
    private var gameIcon: some View {
        if let icon = UIImage(data: game.icon) {
            Image(uiImage: icon)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: 55, height: 55)
                .cornerRadius(10)
        } else {
            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .fill(colorScheme == .dark ? Color(.systemGray5) : Color(.systemGray6))
                    .frame(width: 55, height: 55)

                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 24))
                    .foregroundColor(.gray)
            }
        }
    }

    private var gameInfo: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(game.title)
                .font(.system(size: 16, weight: .medium))
                .foregroundColor(.primary)
                .multilineTextAlignment(.leading)

            Text(String(game.id, radix: 16).uppercased())
                .font(.system(size: 12))
                .foregroundColor(.secondary)
        }
    }
}

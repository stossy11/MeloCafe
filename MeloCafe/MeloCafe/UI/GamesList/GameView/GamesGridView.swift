//
//  GamesGridView.swift
//  MeloCafe
//
//  Created by Stossy11 on 5/7/2026.
//

import SwiftUI

enum CardType: String, Codable, CaseIterable {
    case list
    case card
    case compactCard
    case compactCardNoBackground
    case compactCardSmall
    
    var displayName: String {
        switch self {
        case .list: "List"
        case .card: "Card"
        case .compactCard: "Compact Card"
        case .compactCardNoBackground: "Compact Card (No Background)"
        case .compactCardSmall: "Compact Card (Small)"
        }
    }
}


struct GameCardView: View {
    let game: GameInfoSwift
    var isPinned: Bool = false
    let onLaunch: (GameInfoSwift) -> Void
    @Environment(\.colorScheme) private var colorScheme
    @AppStorage("cardType") var cardTypeRawValue: String = CardType.list.rawValue
    var cardType: CardType {
        CardType(rawValue: cardTypeRawValue) ?? .list
    }
    
    @State var icon: UIImage?

    @ViewBuilder
    var smallGrid: some View {
        if let icon = self.icon {
            Image(uiImage: icon)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: 150, height: 150)
                .clipShape(RoundedRectangle(cornerRadius: 12))
        } else {
            RoundedRectangle(cornerRadius: 12)
                .fill(colorScheme == .dark ? Color(.systemGray5) : Color(.systemGray6))
                .frame(width: 150, height: 150)
            
            Image(systemName: "questionmark.square.dashed")
                .font(.system(size: 40))
                .foregroundColor(.gray)
        }
    }
    
    @ViewBuilder
    var wiiUCard: some View {
        Group {
            if let icon = self.icon {
                Image(uiImage: icon)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: 95, height: 90)
                    .clipShape(RoundedRectangle(cornerRadius: 14))
            } else {
                RoundedRectangle(cornerRadius: 14)
                    .fill(colorScheme == .dark ? Color(.systemGray5) : Color(.systemGray6))
                    .frame(width: 95, height: 95)
                
                Image(systemName: "questionmark.square.dashed")
                    .font(.system(size: 40))
                    .foregroundColor(.gray)
            }
        }
        .padding(10)
        .background {
            RoundedRectangle(cornerRadius: 20)
                .fill(.thinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 20)
                        .stroke(Color(.systemGray4), lineWidth: 1)
                )
                .shadow(
                    color: Color(.darkGray).opacity(colorScheme == .dark ? 0.3 : 0.1),
                    radius: 8,
                    x: 0,
                    y: 2
                )
        }
    }
    
    var body: some View {
        Button {
            onLaunch(game)
        } label: {
            switch cardType {
            case .list: EmptyView()
            case .card:
                normalGrid
            case .compactCard:
                smallGrid
                    .padding(12)
                    .background {
                        RoundedRectangle(cornerRadius: 16)
                            .fill(.thinMaterial)
                            .overlay(
                                RoundedRectangle(cornerRadius: 16)
                                    .stroke(Color(.systemGray4), lineWidth: 1)
                            )
                            .shadow(
                                color: Color(.darkGray).opacity(colorScheme == .dark ? 0.3 : 0.1),
                                radius: 8,
                                x: 0,
                                y: 2
                            )
                    }
            case .compactCardNoBackground:
                smallGrid
            case .compactCardSmall:
                wiiUCard
            }
        }
        .overlay(alignment: .topTrailing) {
            if isPinned {
                Image(systemName: "pin.fill")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.primary)
                    .padding(7)
                    .background(.regularMaterial, in: Circle())
                    .padding(6)
                    .accessibilityLabel("Pinned")
                    .allowsHitTesting(false)
            }
        }
        .task {
            self.icon = UIImage(data: game.icon)
        }
    }
    
    
    @ViewBuilder
    var normalGrid: some View {
        VStack(spacing: 8) {
            // Game Icon
            ZStack {
                if let icon = self.icon {
                    Image(uiImage: icon)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 150, height: 150)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                } else {
                    RoundedRectangle(cornerRadius: 12)
                        .fill(colorScheme == .dark ? Color(.systemGray5) : Color(.systemGray6))
                        .frame(width: 150, height: 150)
                    
                    Image(systemName: "questionmark.square.dashed")
                        .font(.system(size: 40))
                        .foregroundColor(.gray)
                }
            }
            
            // Game info
            VStack(alignment: .leading, spacing: 4) {
                Text(game.title)
                    .font(.system(size: 14, weight: .semibold))
                    .multilineTextAlignment(.leading)
                    .foregroundColor(.primary)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                
                HStack {
                    Text(String(format: "%016llX", game.id))
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            
            Spacer()
        }
        .padding(12)
        .frame(width: 174, height: 220)
        .background {
            RoundedRectangle(cornerRadius: 16)
                .fill(.thinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 16)
                        .stroke(Color(.systemGray4), lineWidth: 1)
                )
                .shadow(
                    color: Color(.darkGray).opacity(colorScheme == .dark ? 0.3 : 0.1),
                    radius: 8,
                    x: 0,
                    y: 2
                )
        }
        .buttonStyle(PlainButtonStyle())
    }
}

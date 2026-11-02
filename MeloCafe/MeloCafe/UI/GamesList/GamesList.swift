//
//  GamesList.swift
//  MeloCafe
//
//  Created by Stossy11 on 3/4/2026.
//

import SwiftUI
import UniformTypeIdentifiers

enum ActiveSheet: Identifiable, Equatable {
    static func == (lhs: ActiveSheet, rhs: ActiveSheet) -> Bool {
        lhs.id == rhs.id
    }

    case graphicPacks(game: GameInfoSwift)

    var id: String {
        switch self {
        case .graphicPacks(game: let game):
            return "\(type(of: self))-\(game.id)"
        }
    }
}

struct GamesListView: View {
    @EnvironmentObject var gamesList: GamesManager
    @State var activeSheet: ActiveSheet?
    
    @AppStorage("cardType") var cardTypeRawValue: String = CardType.list.rawValue
    var cardType: CardType {
        CardType(rawValue: cardTypeRawValue) ?? .list
    }

    var body: some View {
        NavigationStack {
            Group {
                if !gamesList.games.isEmpty {
                    switch cardType {
                    case .list: list
                    default: grid
                    }
                } else {
                    ScrollView {
                        VStack(alignment: .center) {
                            emptyStateView
                                .padding(.top)
                        }
                    }
                }
            }
            .sheet(item: $activeSheet) { sheet in
                switch sheet {
                case .graphicPacks(game: let game):
                    GameGraphicPacksView(titleId: game.id, gameName: game.title)
                }
            }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        FileImporterManager.shared.importFiles(types: [.item], allowMultiple: true) { result in
                            switch result {
                            case .success(let urls):
                                for url in urls {
                                    switch url.getUTType {
                                    case .zip:
                                        try? ZIPExtractor.extract(zipURL: url, to: .romsURL)
                                    default:
                                        try? FileManager.default.copyItem(at: url, to: .romsURL.appendingPathComponent(url.lastPathComponent))
                                    }
                                }
                                
                                gamesList.loadGames() // forgot this when uploading to the damn AppStore, i'm stupid :sob: -stossy11
                            case .failure(let err):
                                AppAlerts.showSyncAlert(title: "ROM Import Failed.", message: err.localizedDescription)
                            }
                        }
                    } label: {
                        Image(systemName: "plus")
                    }
                }
            }
        }
    }
    
    @ViewBuilder
    var list: some View {
        List(gamesList.orderedGames) { game in
            GameRowView(game: game, isPinned: gamesList.isPinned(game)) { launched in
                gamesList.loadGame(launched)
            }
            .contextMenu {
                contextMenu(game)
            }
        }
    }
    
    @ViewBuilder
    var grid: some View {
        ScrollView {
            var columns: [GridItem] {
                switch cardType {
                case .card, .compactCard: [GridItem(.adaptive(minimum: 160, maximum: 200), spacing: 16)]
                case .compactCardNoBackground: [GridItem(.adaptive(minimum: 150, maximum: 180), spacing: 16)]
                case .compactCardSmall: [GridItem(.adaptive(minimum: 105, maximum: 120), spacing: 16)]
                default: [GridItem(.adaptive(minimum: 160, maximum: 200), spacing: 16)]
                }
            }
            
            
            LazyVGrid(columns: columns, spacing: columns.first?.spacing ?? 16) {
                ForEach(gamesList.orderedGames) { game in
                    GameCardView(game: game, isPinned: gamesList.isPinned(game)) { launched in
                        gamesList.loadGame(launched)
                    }
                    .id(game.id)
                    .contextMenu {
                        contextMenu(game)
                    }
                }
            }
            .padding(.horizontal)
            .padding(.top)
        }
        .padding(.horizontal)
    }
    
    @ViewBuilder
    private var emptyStateView: some View {
        if #available(iOS 17, *) {
            ContentUnavailableView(
                "No Games Found",
                systemImage: "square.and.arrow.down",
                description: Text("Tap the + button to add legally dumped ROMs!")
            )
        } else {
            VStack(spacing: 20) {
                Spacer()
                
                Image(systemName: "square.and.arrow.down")
                    .font(.system(size: 64))
                    .foregroundColor(.secondary)
                
                Text("No Games Found")
                    .font(.title2)
                    .fontWeight(.semibold)
                
                Text("Tap the + button to add legally dumped ROMs!")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal)
                
                Spacer()
            }
            .frame(maxWidth: .infinity)
            .listRowInsets(EdgeInsets())
        }
    }
}

extension URL {
    var getUTType: UTType? {
        do {
            let resourceValues = try self.resourceValues(forKeys: [.contentTypeKey])
            return resourceValues.contentType
        } catch {
            return nil
        }
    }
}

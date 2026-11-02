//
//  GamesListManager.swift
//  MeloCafe
//
//  Created by Stossy11 on 2/4/2026.
//

import SwiftUI
import Combine

struct GameInfoSwift: Codable, Identifiable {
    let id: UInt64
    let title: String
    let path: String
    var icon: Data
    var bundleId: String? = nil
}

class GamesManager: ObservableObject {
    @Published var games: [GameInfoSwift] = []
    @Published var startedEmulation: Bool = false
    @Published private(set) var pinnedGameIDs: Set<UInt64>

    private static let pinnedGamesKey = "pinnedGameIDs"
    private let fileManager = FileManager.default
    public static let shared = GamesManager()
    
    var gettingIcon: Task<Void, Never>?
    
    var finished: Bool = false

    private init() {
        let savedIDs = UserDefaults.standard.stringArray(forKey: Self.pinnedGamesKey) ?? []
        pinnedGameIDs = Set(savedIDs.compactMap { UInt64($0) })
    }

    var orderedGames: [GameInfoSwift] {
        // Keep the library's existing order within each group.
        games.filter { isPinned($0) } + games.filter { !isPinned($0) }
    }

    func isPinned(_ game: GameInfoSwift) -> Bool {
        pinnedGameIDs.contains(game.id)
    }

    func togglePin(_ game: GameInfoSwift) {
        if isPinned(game) {
            pinnedGameIDs.remove(game.id)
        } else {
            pinnedGameIDs.insert(game.id)
        }
        // Strings preserve the full UInt64 title ID in UserDefaults.
        UserDefaults.standard.set(pinnedGameIDs.sorted().map { String($0) }, forKey: Self.pinnedGamesKey)
    }

    func loadGames()  {
        guard !finished else {
            return
        }
        
        finished = true
        
        if let enumerator = fileManager.enumerator(at: .romsURL, includingPropertiesForKeys: nil) {
            for case let fileURL as URL in enumerator {
                _ = CemuManager.load(path: fileURL.path)
            }
        }
        
        var count: Int32 = 0
        
        if let gamesPtr = CemuGetAllGames(false, false, &count) {
            let gamesArray = UnsafeBufferPointer(start: gamesPtr, count: Int(count))
            
            gettingIcon?.cancel()
            
            gettingIcon = Task {
                var games: [GameInfoSwift] = []
                for game in gamesArray {
                    var gameInfo = GameInfoSwift(
                        id: game.titleId,
                        title: String(cString: game.name),
                        path: String(cString: game.path),
                        icon: Data()
                    )
                    gameInfo.icon = CemuManager.loadGameIcon(game.titleId) ?? Data()
                    games.append(gameInfo)
                }
                
                CemuFreeGameList(gamesPtr, count)
                
                let games2 = games
                await MainActor.run {
                    self.games = games2
                    self.finished = false
                }
            }
            
        } else {
            print("No games found.")
        }
    }

    func loadGame(_ game: String, _ load: Bool = false) {
        let mgr = ControllerManager.shared
        mgr.isRunning = true
        let id = CemuManager.load(path: game, load: true)

        if load {
            if id > 0 {
                _ = CemuManager.loadTitle(titleId: id)
            }
        }

        CemuManager.run()
        startedEmulation = true
    }

    func loadGame(_ game: GameInfoSwift) {
        let mgr = ControllerManager.shared
        mgr.isRunning = true
        _ = CemuManager.loadTitle(titleId: game.id)
        CemuManager.run()
        startedEmulation = true
    }

    func handleDeepLink(_ url: URL) {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: true) else { return }

        switch components.host {
        case "game":
            let idMatch = components.queryItems?.first(where: { $0.name == "id" })?.value
            let nameMatch = components.queryItems?.first(where: { $0.name == "name" })?.value

            Task {

                try? await Task.sleep(nanoseconds: 500_000_000)

                if let query = idMatch ?? nameMatch {

                    let game = self.games.first {
                        $0.id == UInt64(query) || $0.title == query
                    }

                    if let game {
                        loadGame(game)
                    }
                }

            }

        case "gameInfo":
            var games = self.games

            if games.count >= 1 {
                games[0].bundleId = Bundle.main.bundleIdentifier
            }

            for index in games.indices {
                let icon = UIImage(data: games[index].icon)

                if let image = icon?.jpegData(compressionQuality: 0.5) {
                    games[index].icon = image
                }
            }

            guard let urlscheme = components.queryItems?.first(where: { $0.name == "scheme" })?.value,
                  let data = try? JSONEncoder().encode(games) else { return }

            let encoded = data.base64urlEncodedString()
            let scheme = url.scheme ?? "melocafe"
            if let returnURL = URL(string: "\(urlscheme)://\(scheme)?games=\(encoded)") {
                UIApplication.shared.open(returnURL)
            }
        default:
            return
        }
    }

}

extension Data {
    public func base64urlEncodedString() -> String {
        self.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

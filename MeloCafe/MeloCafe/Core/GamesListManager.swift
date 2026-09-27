//
//  GamesListManager.swift
//  MeloCafe
//
//  Created by Stossy11 on 2/4/2026.
//

import SwiftUI
import Combine

nonisolated struct GameInfoSwift: Codable, Identifiable, Sendable {
    let id: UInt64
    let title: String
    let path: String
    var icon: Data
    var bundleId: String? = nil
}

@MainActor
class GamesManager: ObservableObject {

    @Published var games: [GameInfoSwift] = []
    @Published var startedEmulation: Bool = false
    @Published private(set) var isLoading = false

    private var libraryTask: Task<Void, Never>?
    private var refreshRequested = false
    private var preparingGame = false
    public static let shared = GamesManager()

    private init() {

    }

    func loadGames() {
        guard !startedEmulation else { return }
        refreshRequested = true
        guard libraryTask == nil else { return }
        isLoading = true
        libraryTask = Task {
            repeat {
                refreshRequested = false
                let discovered = await Task.detached(priority: .userInitiated) {
                    Self.scanGames()
                }.value
                games = discovered
            } while refreshRequested
            isLoading = false
            libraryTask = nil
        }
    }

    func waitForLibrary() async {
        await libraryTask?.value
    }

    nonisolated private static func scanGames() -> [GameInfoSwift] {
        let start = Date()
        CemuRefreshGames()
        var games: [GameInfoSwift] = []
        var count: Int32 = 0

        if let gamesPtr = CemuGetAllGames(false, false, &count) {
            defer { CemuFreeGameList(gamesPtr, count) }
            let gamesArray = UnsafeBufferPointer(start: gamesPtr, count: Int(count))

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
        }
        print("Library scan: \(games.count) games in \(Date().timeIntervalSince(start)) seconds")
        return games
    }

    func loadGame(_ game: String, _ load: Bool = false) {
        prepareForGame {
            let mgr = ControllerManager.shared
            mgr.isRunning = true
            let id = CemuManager.load(path: game, load: true)

            if load {
                if id > 0 {
                    _ = CemuManager.loadTitle(titleId: id)
                }
            }

            CemuManager.run()
            self.startedEmulation = true
        }
    }

    func loadGame(_ game: GameInfoSwift) {
        prepareForGame {
            let mgr = ControllerManager.shared
            mgr.isRunning = true
            _ = CemuManager.loadTitle(titleId: game.id)
            CemuManager.run()
            self.startedEmulation = true
        }
    }

    private func prepareForGame(_ launch: @escaping () -> Void) {
        guard !preparingGame, !startedEmulation else { return }
        preparingGame = true
        Task {
            defer { preparingGame = false }
            await waitForLibrary()
            launch()
        }
    }

    func handleDeepLink(_ url: URL) {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: true) else { return }

        switch components.host {
        case "game":
            let idMatch = components.queryItems?.first(where: { $0.name == "id" })?.value
            let nameMatch = components.queryItems?.first(where: { $0.name == "name" })?.value

            Task {

                await waitForLibrary()

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
            Task {
                await waitForLibrary()
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
                    await UIApplication.shared.open(returnURL)
                }
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

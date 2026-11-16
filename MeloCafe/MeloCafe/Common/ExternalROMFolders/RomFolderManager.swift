//
//  RomFolderManager.swift
//  MeloNX
//
//  Created by Stossy11 on 31/07/2025.
//

import Foundation
import Combine

final class ROMFolderManager: ObservableObject {
    static let shared = ROMFolderManager()

    private let bookmarksKey = "ROMFolderManagerBookmarks"

    @Published var bookmarks: [Data] = [] {
        didSet {
            UserDefaults.standard.set(bookmarks, forKey: bookmarksKey)
        }
    }

    private init() {
        loadBookmarks()
    }
    
    
    @discardableResult
    func addFolder(url: URL) -> Bool {
        guard url.startAccessingSecurityScopedResource() else {
            return false
        }
        defer { url.stopAccessingSecurityScopedResource() }

        do {
            let bookmark = try makeBookmark(for: url)
            bookmarks.append(bookmark)
            return true
        } catch {
            print("Failed to create ROM folder bookmark: \(error)")
            return false
        }
    }
    
    func getUrl(from bookmark: Data) -> URL? {
        do {
            var isStale = false
            let url = try URL(
                resolvingBookmarkData: bookmark,
                bookmarkDataIsStale: &isStale
            )

            guard url.startAccessingSecurityScopedResource() else {
                return nil
            }

            if isStale, let index = bookmarks.firstIndex(of: bookmark) {
                do {
                    bookmarks[index] = try makeBookmark(for: url)
                } catch {
                    print("Failed to refresh ROM folder bookmark: \(error)")
                }
            }

            return url
        } catch {
            print("Failed to resolve ROM folder bookmark: \(error)")
            return nil
        }
    }

    func stopAccessingFolder(url: URL) {
        url.stopAccessingSecurityScopedResource()
    }

    func loadBookmarks() {
        let defaults = UserDefaults.standard

        if let saved = defaults.array(forKey: bookmarksKey) as? [Data] {
            bookmarks = saved
        } else if let legacy = defaults.dictionary(forKey: bookmarksKey)
                    as? [String: Data] {
            bookmarks = Array(legacy.values)
        }
    }

    private func makeBookmark(for url: URL) throws -> Data {
        try url.bookmarkData(
            options: .minimalBookmark,
            includingResourceValuesForKeys: nil,
            relativeTo: nil
        )
    }
}

//
//  GraphicPacksView.swift
//  MeloCafe
//
//  Created by Stossy11 on 11/4/2026.
//

import SwiftUI
import Combine

struct GraphicPacksView: View {
    @StateObject private var viewModel = GraphicPacksViewModel()
    @State private var searchText = ""
    
    var filteredPacks: [GraphicPack] {
        if searchText.isEmpty {
            return viewModel.packs
        }
        
        return viewModel.packs.filter {
            $0.name.localizedCaseInsensitiveContains(searchText) ||
            $0.virtualPath.localizedCaseInsensitiveContains(searchText)
        }
    }
    
    var groupedPacks: [(String, [GraphicPack])] {
        var dict = [String: [GraphicPack]]()
        for pack in filteredPacks {
            dict[pack.group, default: []].append(pack)
        }
        
        return dict.sorted { $0.key < $1.key }
    }
    
    var body: some View {
        NavigationStack {
            List {
                Section {
                    if viewModel.isDownloading {
                        VStack(spacing: 8) {
                            Text(viewModel.downloadStage)
                                .font(.subheadline)
                            ProgressView(value: viewModel.downloadProgress)
                        }
                        .padding(.vertical, 4)
                    } else {
                        Button {
                            viewModel.downloadPacks()
                        } label: {
                            Label("Download latest graphic packs", systemImage: "arrow.down.circle")
                        }
                    }
                    
                    if let error = viewModel.downloadError {
                        Text(error)
                            .font(.caption)
                            .foregroundStyle(.red)
                    }
                }
                
                ForEach(groupedPacks, id: \.0) { group, packs in
                    Section(group) {
                        ForEach(packs) { pack in
                            GraphicPackRow(pack: pack, viewModel: viewModel)
                        }
                    }
                }
            }
            .searchable(text: $searchText, prompt: "Search graphic packs")
            .navigationTitle("Graphic Packs")
            .onAppear {
                viewModel.refresh()
            }
        }
    }
}

struct GraphicPackRow: View {
    let pack: GraphicPack
    @ObservedObject var viewModel: GraphicPacksViewModel
    
    @State private var isExpanded = false
    
    var body: some View {
        DisclosureGroup(isExpanded: $isExpanded) {
            if !pack.description.isEmpty {
                Text(pack.description)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            
            ForEach(pack.presetCategories, id: \.self) { category in
                let presets = pack.visiblePresets(for: category)
                if !presets.isEmpty {
                    Picker(category.isEmpty ? "Preset" : category, selection: presetBinding(category: category)) {
                        ForEach(presets) { preset in
                            Text(preset.name).tag(preset.name)
                        }
                    }
                }
            }
        } label: {
            Toggle(isOn: enabledBinding) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(pack.name)
                        .font(.body)
                    if !pack.virtualPath.isEmpty && pack.virtualPath != pack.name {
                        Text(pack.virtualPath)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
    
    private var enabledBinding: Binding<Bool> {
        Binding {
            pack.enabled
        } set: { newValue in
            viewModel.setEnabled(newValue, for: pack)
        }
    }
    
    private func presetBinding(category: String) -> Binding<String> {
        Binding {
            pack.activePreset(for: category)
        } set: { newValue in
            viewModel.setActivePreset(newValue, category: category, for: pack)
        }
    }
}

class GraphicPacksViewModel: ObservableObject {
    @Published var packs: [GraphicPack] = []
    @Published var isDownloading = false
    @Published var downloadProgress: Double = 0
    @Published var downloadStage = ""
    @Published var downloadError: String?
    
    private let manager = GraphicPackManager.shared()
    
    func refresh() {
        let entries = manager?.allPacks() as? [ObjCGraphicPackEntry] ?? []
        packs = entries.map { GraphicPack($0) }
    }
    
    func setEnabled(_ enabled: Bool, for pack: GraphicPack) {
        manager?.setEnabled(enabled, forPack: pack.normalizedPath)
        refresh()
    }
    
    func setActivePreset(_ name: String, category: String, for pack: GraphicPack) {
        manager?.setActivePreset(name, category: category, forPack: pack.normalizedPath)
        refresh()
    }
    
    func downloadPacks() {
        isDownloading = true
        downloadError = nil
        downloadProgress = 0
        downloadStage = "Checking for updates..."
        
        let apiURL = URL(string: "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest")!
        var request = URLRequest(url: apiURL)
        request.setValue("MeloCafe", forHTTPHeaderField: "User-Agent")
        
        URLSession.shared.dataTask(with: request) { [weak self] data, _, error in
            guard let self else { return }
            
            guard let data, error == nil else {
                self.finish(error: "Failed to connect to server")
                return
            }
            
            guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let releaseName = json["name"] as? String,
                  let assets = json["assets"] as? [[String: Any]],
                  let firstAsset = assets.first,
                  let downloadURLString = firstAsset["browser_download_url"] as? String,
                  let downloadURL = URL(string: downloadURLString) else {
                self.finish(error: "Failed to parse server response")
                return
            }
            
            if let installed = self.manager?.installedVersion(),
               installed.caseInsensitiveCompare(releaseName) == .orderedSame {
                DispatchQueue.main.async {
                    self.downloadStage = "Already up to date"
                    self.downloadProgress = 1.0
                    self.isDownloading = false
                }
                return
            }
            
            DispatchQueue.main.async {
                self.downloadStage = "Downloading..."
            }
            
            self.downloadZIP(from: downloadURL, version: releaseName)
        }.resume()
    }
    
    private func downloadZIP(from url: URL, version: String) {
        let delegate = DownloadProgressDelegate { [weak self] progress in
            DispatchQueue.main.async {
                self?.downloadProgress = progress
            }
        }
        
        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        session.downloadTask(with: url) { [weak self] tempURL, _, error in
            guard let self else { return }
            
            guard let tempURL, error == nil else {
                self.finish(error: "Failed to download graphic packs")
                return
            }
            
            DispatchQueue.main.async {
                self.downloadStage = "Extracting..."
                self.downloadProgress = 0
            }
            
            DispatchQueue.global(qos: .userInitiated).async {
                do {
                    guard let basePath = self.manager?.graphicPacksBasePath() else {
                        self.finish(error: "Could not determine graphic packs path")
                        return
                    }
                    
                    let destURL = URL(fileURLWithPath: basePath)
                    
                    let fm = FileManager.default
                    if fm.fileExists(atPath: basePath) {
                        let contents = try fm.contentsOfDirectory(atPath: basePath)
                        for item in contents {
                            try fm.removeItem(atPath: (basePath as NSString).appendingPathComponent(item))
                        }
                    }
                    try fm.createDirectory(at: destURL, withIntermediateDirectories: true)
                    
                    try ZIPExtractor.extract(zipURL: tempURL, to: destURL)
                    
                    let versionFile = destURL.appendingPathComponent("version.txt")
                    try version.write(to: versionFile, atomically: true, encoding: .utf8)
                    
                    self.manager?.refreshPacks()
                    
                    DispatchQueue.main.async {
                        self.refresh()
                        self.isDownloading = false
                    }
                } catch {
                    self.finish(error: error.localizedDescription)
                }
            }
        }.resume()
    }
    
    private func finish(error: String) {
        DispatchQueue.main.async {
            self.downloadError = error
            self.isDownloading = false
        }
    }
}

private class DownloadProgressDelegate: NSObject, URLSessionDownloadDelegate {
    let onProgress: (Double) -> Void
    
    init(onProgress: @escaping (Double) -> Void) {
        self.onProgress = onProgress
    }
    
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64, totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        if totalBytesExpectedToWrite > 0 {
            onProgress(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite))
        }
    }
    
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {}
}

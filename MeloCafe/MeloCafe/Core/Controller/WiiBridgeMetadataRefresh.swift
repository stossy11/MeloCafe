import Foundation

/// Optional metadata never owns the UI's busy state. User commands preempt reads;
/// a failed read retries only that item, with a bounded backoff.
@MainActor
final class WiiBridgeMetadataRefresh {
    enum Item: Hashable {
        case bridge, remote(UInt8), players
        var priority: Int {
            switch self { case .bridge: return 0; case .remote(let slot): return Int(slot) + 1; case .players: return 5 }
        }
    }
    private let read: (Item) async throws -> Void
    private let failed: (Item, Error) -> Void
    private let retryDelay: UInt64
    private var pending = Set<Item>()
    private var attempts: [Item: Int] = [:]
    private var task: Task<Void, Never>?
    private var runID = UUID()
    private var epoch = 0
    private var userOperation = false
    private var notBefore: TimeInterval = 0

    init(retryDelay: UInt64 = 2_000_000_000,
         read: @escaping (Item) async throws -> Void,
         failed: @escaping (Item, Error) -> Void) {
        self.retryDelay = retryDelay; self.read = read; self.failed = failed
    }
    func request(_ items: Set<Item>, delay: TimeInterval = 2) {
        for item in items where !pending.contains(item) { attempts[item] = 0 }
        pending.formUnion(items)
        notBefore = max(notBefore, ProcessInfo.processInfo.systemUptime + delay)
        start()
    }
    func reset() {
        epoch += 1
        runID = UUID()
        task?.cancel(); task = nil
        pending = []; attempts = [:]; notBefore = 0; userOperation = false
    }
    /// The caller serializes explicit user actions and owns their progress/error UI.
    func performUserOperation(_ operation: () async throws -> Void) async throws {
        let currentEpoch = epoch
        userOperation = true
        let background = task
        runID = UUID()
        task = nil
        background?.cancel()
        await background?.value
        defer {
            if epoch == currentEpoch { userOperation = false; start() }
        }
        try Task.checkCancellation()
        guard epoch == currentEpoch else { throw CancellationError() }
        try await operation()
    }
    private func start() {
        guard !userOperation, task == nil, !pending.isEmpty else { return }
        let id = UUID(); runID = id
        task = Task { [weak self] in
            guard let self else { return }
            defer { if self.runID == id { self.task = nil } }
            while !Task.isCancelled, self.runID == id,
                  let item = self.pending.min(by: { $0.priority < $1.priority }) {
                let remaining = self.notBefore - ProcessInfo.processInfo.systemUptime
                if remaining > 0 {
                    do { try await Task.sleep(nanoseconds: UInt64(min(remaining, 60) * 1_000_000_000)) }
                    catch { return }
                    continue
                }
                do {
                    try await self.read(item)
                    try Task.checkCancellation()
                    guard self.runID == id else { return }
                    self.pending.remove(item); self.attempts[item] = nil
                } catch {
                    guard !Task.isCancelled, self.runID == id else { return }
                    self.failed(item, error)
                    let count = (self.attempts[item] ?? 0) + 1
                    self.attempts[item] = count
                    if count >= 3 { self.pending.remove(item) }
                    else {
                        self.notBefore = ProcessInfo.processInfo.systemUptime +
                            Double(self.retryDelay) / 1_000_000_000 * Double(1 << (count - 1))
                    }
                }
            }
        }
    }
}

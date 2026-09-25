//
//  ZIPExtractor.swift
//  MeloCafe
//
//  Created by Stossy11 on 11/4/2026.
//

import Foundation
import Compression

enum ZIPExtractor {
    enum ZIPError: Error, LocalizedError {
        case invalidArchive
        case unsupportedCompression(UInt16)
        case zip64NotSupported
        case decompressionFailed
        case corruptEntry(String)

        var errorDescription: String? {
            switch self {
            case .invalidArchive: return "Invalid ZIP archive"
            case .unsupportedCompression(let m): return "Unsupported compression method \(m)"
            case .zip64NotSupported: return "ZIP64 archives are not supported"
            case .decompressionFailed: return "Decompression failed"
            case .corruptEntry(let name): return "Corrupt entry: \(name)"
            }
        }
    }

    private static let chunkSize = 64 * 1024

    static func extract(zipURL: URL, to destinationURL: URL) throws {
        let handle = try FileHandle(forReadingFrom: zipURL)
        defer { try? handle.close() }

        let fileSize = Int(try handle.seekToEnd())
        let eocd = try readEOCD(handle, fileSize: fileSize)
        guard eocd.cdOffset + eocd.cdSize <= fileSize else { throw ZIPError.invalidArchive }
        
        try handle.seek(toOffset: UInt64(eocd.cdOffset))
        let cd = try readExactly(handle, count: eocd.cdSize)

        let fm = FileManager.default
        let root = destinationURL.standardizedFileURL
        try fm.createDirectory(at: root, withIntermediateDirectories: true)

        var offset = 0
        for _ in 0..<eocd.entryCount {
            guard offset + 46 <= cd.count, cd.le32(offset) == 0x02014b50 else {
                throw ZIPError.invalidArchive
            }

            let method = cd.le16(offset + 10)
            let compressedRaw = cd.le32(offset + 20)
            let uncompressedRaw = cd.le32(offset + 24)
            let nameLength = Int(cd.le16(offset + 28))
            let extraLength = Int(cd.le16(offset + 30))
            let commentLength = Int(cd.le16(offset + 32))
            let localHeaderRaw = cd.le32(offset + 42)

            let recordLength = 46 + nameLength + extraLength + commentLength
            guard offset + recordLength <= cd.count else { throw ZIPError.invalidArchive }
            defer { offset += recordLength }   // runs on `continue` too

            if compressedRaw == 0xFFFF_FFFF || uncompressedRaw == 0xFFFF_FFFF || localHeaderRaw == 0xFFFF_FFFF {
                throw ZIPError.zip64NotSupported
            }

            let nameData = cd.subdata(in: (offset + 46)..<(offset + 46 + nameLength))
            guard let name = String(data: nameData, encoding: .utf8),
                  let entryURL = safeURL(for: name, in: root) else {
                continue
            }

            if name.hasSuffix("/") {
                try fm.createDirectory(at: entryURL, withIntermediateDirectories: true)
                continue
            }

            try fm.createDirectory(at: entryURL.deletingLastPathComponent(),
                                   withIntermediateDirectories: true)

            try extractEntry(
                from: handle,
                fileSize: fileSize,
                localHeaderOffset: Int(localHeaderRaw),
                method: method,
                compressedSize: Int(compressedRaw),
                uncompressedSize: Int(uncompressedRaw),
                name: name,
                to: entryURL
            )
        }
    }

    private static func extractEntry(
        from handle: FileHandle,
        fileSize: Int,
        localHeaderOffset: Int,
        method: UInt16,
        compressedSize: Int,
        uncompressedSize: Int,
        name: String,
        to url: URL
    ) throws {
        guard method == 0 || method == 8 else { throw ZIPError.unsupportedCompression(method) }

        guard localHeaderOffset + 30 <= fileSize else { throw ZIPError.corruptEntry(name) }
        try handle.seek(toOffset: UInt64(localHeaderOffset))
        let header = try readExactly(handle, count: 30)
        guard header.le32(0) == 0x04034b50 else { throw ZIPError.corruptEntry(name) }

        let dataOffset = localHeaderOffset + 30 + Int(header.le16(26)) + Int(header.le16(28))
        guard dataOffset + compressedSize <= fileSize else { throw ZIPError.corruptEntry(name) }
        try handle.seek(toOffset: UInt64(dataOffset))

        let fm = FileManager.default
        guard fm.createFile(atPath: url.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        let out = try FileHandle(forWritingTo: url)
        var succeeded = false
        defer {
            try? out.close()
            if !succeeded { try? fm.removeItem(at: url) } // don't leave partial files
        }

        var written = 0

        switch method {
        case 0: // Stored
            guard compressedSize == uncompressedSize else { throw ZIPError.corruptEntry(name) }
            try forEachChunk(of: handle, count: compressedSize) { chunk in
                try out.write(contentsOf: chunk)
                written += chunk.count
            }

        default: // 8 = Deflate (COMPRESSION_ZLIB is raw deflate, which is what ZIP uses)
            let filter = try OutputFilter(.decompress, using: .zlib, bufferCapacity: chunkSize) { decoded in
                guard let decoded, !decoded.isEmpty else { return }
                written += decoded.count
                guard written <= uncompressedSize else { throw ZIPError.corruptEntry(name) }
                try out.write(contentsOf: decoded)
            }
            do {
                try forEachChunk(of: handle, count: compressedSize) { try filter.write($0) }
                try filter.finalize()
            } catch let error as ZIPError {
                throw error
            } catch {
                throw ZIPError.decompressionFailed
            }
        }

        guard written == uncompressedSize else { throw ZIPError.corruptEntry(name) }
        succeeded = true
    }

    private struct EOCD {
        let entryCount: Int
        let cdSize: Int
        let cdOffset: Int
    }
    
    private static func readEOCD(_ handle: FileHandle, fileSize: Int) throws -> EOCD {
        let tailSize = min(fileSize, 22 + 0xFFFF)
        guard tailSize >= 22 else { throw ZIPError.invalidArchive }

        try handle.seek(toOffset: UInt64(fileSize - tailSize))
        let tail = try readExactly(handle, count: tailSize)

        for i in stride(from: tailSize - 22, through: 0, by: -1) where tail.le32(i) == 0x06054b50 {
            let entryCount = tail.le16(i + 10)
            let cdSize     = tail.le32(i + 12)
            let cdOffset   = tail.le32(i + 16)
            if entryCount == 0xFFFF || cdSize == 0xFFFF_FFFF || cdOffset == 0xFFFF_FFFF {
                throw ZIPError.zip64NotSupported
            }
            return EOCD(entryCount: Int(entryCount), cdSize: Int(cdSize), cdOffset: Int(cdOffset))
        }
        throw ZIPError.invalidArchive
    }
    
    private static func forEachChunk(of handle: FileHandle, count: Int, _ body: (Data) throws -> Void) throws {
        var remaining = count
        while remaining > 0 {
            try autoreleasepool {
                guard let chunk = try handle.read(upToCount: min(chunkSize, remaining)),
                      !chunk.isEmpty else {
                    throw ZIPError.invalidArchive // unexpected EOF
                }
                try body(chunk)
                remaining -= chunk.count
            }
        }
    }

    private static func readExactly(_ handle: FileHandle, count: Int) throws -> Data {
        guard count > 0 else { return Data() }
        guard let data = try handle.read(upToCount: count), data.count == count else {
            throw ZIPError.invalidArchive
        }
        return data
    }
    
    private static func safeURL(for name: String, in root: URL) -> URL? {
        let url = root.appendingPathComponent(name).standardizedFileURL
        let rootPath = root.path.hasSuffix("/") ? root.path : root.path + "/"
        return url.path.hasPrefix(rootPath) ? url : nil
    }
}

private extension Data {
    func le16(_ offset: Int) -> UInt16 {
        withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt16.self) }.littleEndian
    }

    func le32(_ offset: Int) -> UInt32 {
        withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self) }.littleEndian
    }
}

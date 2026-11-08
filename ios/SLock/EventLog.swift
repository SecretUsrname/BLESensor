import Combine
import Foundation

/// Timestamped event log, shown in the app and appended to a file you can share.
/// With a sideloaded app there's no debugger, so this is how background behaviour gets debugged.
final class EventLog: ObservableObject {
    static let shared = EventLog()

    @Published private(set) var lines: [String] = []
    let fileURL: URL
    private let formatter: DateFormatter

    private init() {
        fileURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("slock-log.txt")
        formatter = DateFormatter()
        formatter.dateFormat = "MM-dd HH:mm:ss.SSS"
        if let text = try? String(contentsOf: fileURL, encoding: .utf8) {
            lines = text.split(separator: "\n").suffix(300).map(String.init)
        }
    }

    func add(_ text: String) {
        let line = "\(formatter.string(from: Date())) \(text)"
        print(line)
        let append = {
            self.lines.append(line)
            if self.lines.count > 500 { self.lines.removeFirst(self.lines.count - 500) }
        }
        if Thread.isMainThread { append() } else { DispatchQueue.main.async(execute: append) }

        guard let data = (line + "\n").data(using: .utf8) else { return }
        if let handle = try? FileHandle(forWritingTo: fileURL) {
            handle.seekToEndOfFile()
            handle.write(data)
            try? handle.close()
        } else {
            try? data.write(to: fileURL)
        }
    }

    func clear() {
        lines.removeAll()
        try? FileManager.default.removeItem(at: fileURL)
    }
}

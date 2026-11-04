import Foundation
import Darwin

final class LocalControlServer {
    typealias Reply = ([String: Any]) -> Void
    typealias Handler = (ReceiveCommand, @escaping Reply) -> Void
    private var descriptor: Int32 = -1
    private var source: DispatchSourceRead?
    private var path = ""
    private var socketInode: ino_t = 0
    private let queue = DispatchQueue(label: "BluetoothMixer.control.accept")
    private let lock = NSLock()
    private var active = 0

    func start(at url: URL, handler: @escaping Handler) throws {
        path = url.path
        var address = sockaddr_un()
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        address.sun_family = sa_family_t(AF_UNIX)
        let bytes = path.utf8CString
        guard bytes.count <= MemoryLayout.size(ofValue: address.sun_path) else {
            throw POSIXError(.ENAMETOOLONG)
        }
        withUnsafeMutableBytes(of: &address.sun_path) { raw in
            bytes.withUnsafeBytes { raw.copyBytes(from: $0) }
        }
        var existing = stat()
        if lstat(path, &existing) == 0 {
            guard (existing.st_mode & S_IFMT) == S_IFSOCK, existing.st_uid == getuid() else {
                throw POSIXError(.EACCES)
            }
            let probe = socket(AF_UNIX, SOCK_STREAM, 0)
            let connected = withUnsafePointer(to: &address) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.connect(probe, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) == 0
                }
            }
            close(probe)
            guard !connected else { throw POSIXError(.EADDRINUSE) }
            guard unlink(path) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        }
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(.EMFILE) }
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bound == 0 else { close(fd); throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        guard chmod(path, 0o600) == 0, listen(fd, 16) == 0 else {
            close(fd); unlink(path); throw POSIXError(.EACCES)
        }
        var own = stat(); _ = lstat(path, &own); socketInode = own.st_ino
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        descriptor = fd
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in self?.acceptClients(fd, handler: handler) }
        source.setCancelHandler { close(fd) }
        self.source = source; source.resume()
    }
    private func acceptClients(_ listener: Int32, handler: @escaping Handler) {
        while true {
            let client = accept(listener, nil, nil)
            if client < 0 { return }
            _ = fcntl(client, F_SETFD, FD_CLOEXEC)
            _ = fcntl(client, F_SETFL, fcntl(client, F_GETFL) & ~O_NONBLOCK)
            var uid: uid_t = 0, gid: gid_t = 0
            guard getpeereid(client, &uid, &gid) == 0, uid == getuid() else { close(client); continue }
            lock.lock(); let allowed = active < 16; if allowed { active += 1 }; lock.unlock()
            guard allowed else { close(client); continue }
            var timeout = timeval(tv_sec: 2, tv_usec: 0), yes: Int32 = 1
            withUnsafePointer(to: &timeout) {
                _ = setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, $0, socklen_t(MemoryLayout<timeval>.size))
                _ = setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, $0, socklen_t(MemoryLayout<timeval>.size))
            }
            withUnsafePointer(to: &yes) { _ = setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, $0, 4) }
            DispatchQueue.global(qos: .utility).async { [weak self] in
                guard let self = self else { close(client); return }
                let finish: Reply = { [weak self] object in
                    DispatchQueue.global(qos: .utility).async {
                        let data = ((try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])) ?? Data("{\"ok\":false}".utf8)) + Data([10])
                        data.withUnsafeBytes { raw in
                            var offset = 0
                            while offset < raw.count {
                                let n = Darwin.send(client, raw.baseAddress!.advanced(by: offset), raw.count-offset, MSG_NOSIGNAL)
                                if n < 0 && errno == EINTR { continue }; if n <= 0 { break }; offset += n
                            }
                        }
                        close(client)
                        if let self = self { self.lock.lock(); self.active -= 1; self.lock.unlock() }
                    }
                }
                var data = Data(), buffer = [UInt8](repeating: 0, count: 128)
                while data.count <= 512 && !data.contains(10) {
                    let n = Darwin.read(client, &buffer, buffer.count)
                    if n < 0 && errno == EINTR { continue }; if n <= 0 { break }
                    data.append(contentsOf: buffer.prefix(n))
                }
                guard data.count <= 512, let text = String(data: data, encoding: .utf8),
                      let command = ReceiveCommand(rawValue: text.trimmingCharacters(in: .whitespacesAndNewlines)) else {
                    finish(["ok": false, "code": "invalid_command", "message": "仅支持 on、off、toggle、status"]); return
                }
                DispatchQueue.main.async { handler(command, finish) }
            }
        }
    }
    func stop() {
        source?.cancel(); source = nil
        descriptor = -1
        var current = stat()
        if lstat(path, &current) == 0, current.st_ino == socketInode { unlink(path) }
    }
    deinit { stop() }
}

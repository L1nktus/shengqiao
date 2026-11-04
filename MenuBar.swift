import AppKit
import CoreBluetooth

final class OutputLines {
    var pending = Data()
    func objects(_ data: Data) -> [[String: Any]] {
        pending.append(data)
        var result: [[String: Any]] = []
        while let range = pending.firstRange(of: Data([10])) {
            let line = pending.subdata(in: 0..<range.lowerBound)
            pending.removeSubrange(0..<range.upperBound)
            if let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any] { result.append(object) }
        }
        if pending.count > 65536 { pending.removeAll() }
        return result
    }
}

final class SliderRow: NSView {
    let slider: NSSlider
    let valueLabel = NSTextField(labelWithString: "")
    var changed: ((Double) -> Void)?
    init(title: String, minimum: Double, maximum: Double, value: Double, width: CGFloat = 320) {
        slider = NSSlider(value: value, minValue: minimum, maxValue: maximum, target: nil, action: nil)
        super.init(frame: NSRect(x: 0, y: 0, width: width, height: 56))
        let label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 12, weight: .medium)
        label.frame = NSRect(x: 16, y: 32, width: width - 100, height: 18)
        valueLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular)
        valueLabel.alignment = .right
        valueLabel.frame = NSRect(x: width - 80, y: 32, width: 64, height: 18)
        slider.frame = NSRect(x: 14, y: 5, width: width - 28, height: 25)
        slider.target = self; slider.action = #selector(slid); slider.isContinuous = true
        slider.setAccessibilityLabel(title)
        addSubview(label); addSubview(valueLabel); addSubview(slider); update(value)
    }
    required init?(coder: NSCoder) { fatalError() }
    func update(_ value: Double) {
        slider.doubleValue = value; valueLabel.stringValue = "\(Int(value.rounded()))%"
    }
    @objc private func slid() { update(slider.doubleValue); changed?(slider.doubleValue) }
}

final class BluetoothMixerApp: NSObject, NSApplicationDelegate, NSMenuDelegate, CBCentralManagerDelegate {
    private let settings = UserDefaults.standard
    private var statusItem: NSStatusItem!
    private let menu = NSMenu()
    private let statusLabel = NSMenuItem(title: "接收已关闭", action: nil, keyEquivalent: "")
    private let toggleItem = NSMenuItem(title: "开启音频接收", action: #selector(toggle), keyEquivalent: "")
    private let refreshItem = NSMenuItem(title: "刷新音频缓冲", action: #selector(refreshBuffers), keyEquivalent: "")
    private let pairedItem = NSMenuItem(title: "已记住 0 台设备", action: nil, keyEquivalent: "")
    private let pairingItem = NSMenuItem(title: "允许新设备配对", action: #selector(togglePairing), keyEquivalent: "")
    private let sourcesMarker = NSMenuItem(title: "当前音源", action: nil, keyEquivalent: "")
    private let noSources = NSMenuItem(title: "暂无连接 · 新设备连接后自动接管", action: nil, keyEquivalent: "")
    private let master = SliderRow(title: "总音量", minimum: 0, maximum: 100, value: 100)
    private let gain = SliderRow(title: "放大增益", minimum: 100, maximum: 400, value: 100)
    private var sourceItems: [NSMenuItem] = []
    private var sourceRows: [Int: SliderRow] = [:]
    private var sourceLabels: [Int: NSTextField] = [:]
    private var sources: [[String: Any]] = []
    private var sourceVolumes: [String: Double] = [:]
    private var appliedSources = Set<Int>()
    private var runner: Process?
    private var input: FileHandle?
    private let receiving = ReceivingControl()
    private var controlServer: LocalControlServer?
    private var permissionManager: CBCentralManager?
    private var permissionPending = false
    private var phase: String { receiving.phase.rawValue }
    private var lastError: String?
    private var quitting = false
    private var ready = false
    private var pairing = true
    private var logHandle: FileHandle?
    private let ioQueue = DispatchQueue(label: "BluetoothMixer.output")
    private var sessionID = UUID()
    private var statusTimer: Timer?
    private var lastOutput = Date()
    private let support: URL
    private let logs: URL

    override init() {
        let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        support = library.appendingPathComponent("Application Support/BluetoothMixer", isDirectory: true)
        logs = library.appendingPathComponent("Logs/BluetoothMixer", isDirectory: true)
        super.init()
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        do {
            try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            try FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            let file = logs.appendingPathComponent("receiver.log")
            if let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize, size > 4 * 1024 * 1024 {
                let previous = logs.appendingPathComponent("receiver.previous.log")
                try? FileManager.default.removeItem(at: previous); try? FileManager.default.moveItem(at: file, to: previous)
            }
            if !FileManager.default.fileExists(atPath: file.path) { FileManager.default.createFile(atPath: file.path, contents: nil, attributes: [.posixPermissions: 0o600]) }
            logHandle = try FileHandle(forWritingTo: file); try logHandle?.seekToEnd()
        } catch { lastError = "无法打开应用数据目录：\(error.localizedDescription)" }
        let volume = settings.object(forKey: "master") == nil ? 100 : settings.double(forKey: "master")
        let boost = settings.object(forKey: "gain") == nil ? 100 : settings.double(forKey: "gain")
        master.update(min(100, max(0, volume))); gain.update(min(400, max(100, boost)))
        pairing = settings.object(forKey: "pairing") == nil ? true : settings.bool(forKey: "pairing")
        sourceVolumes = settings.dictionary(forKey: "sourceVolumes") as? [String: Double] ?? [:]
        pairedItem.title = "已记住 \(settings.integer(forKey: "pairedCount")) 台设备"
        master.changed = { [weak self] value in self?.settings.set(value, forKey: "master"); self?.send("MASTER \(Int(value.rounded()))") }
        gain.changed = { [weak self] value in self?.settings.set(value, forKey: "gain"); self?.send("GAIN \(Int(value.rounded()))") }
        buildMenu()
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.menu = menu; updateStatus()
        receiving.onStart = { [weak self] in self?.startProcess() }
        receiving.onStop = { [weak self] in self?.stopProcess() }
        receiving.onChange = { [weak self] in self?.updateStatus() }
        do {
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: support.path)
            let server = LocalControlServer()
            try server.start(at: support.appendingPathComponent("control.sock")) { [weak self] command, reply in
                guard let self = self else { reply(["ok": false, "code": "app_exiting"]); return }
                self.handleControl(command, reply: reply)
            }
            controlServer = server
        } catch {
            if let issue = error as? POSIXError, issue.code == .EADDRINUSE {
                NSApp.terminate(nil); return
            }
            try? logHandle?.write(contentsOf: Data("CONTROL_SERVER_ERROR \(error.localizedDescription)\n".utf8))
        }
        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(willSleep), name: NSWorkspace.willSleepNotification, object: nil)
        if CommandLine.arguments.contains("--resume-receive") {
            DispatchQueue.main.async { [weak self] in self?.receiving.request(true) }
        }
    }
    private func buildMenu() {
        menu.delegate = self; menu.autoenablesItems = false
        let header = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 53))
        let title = NSTextField(labelWithString: "声桥")
        title.font = .systemFont(ofSize: 16, weight: .semibold); title.frame = NSRect(x: 16, y: 25, width: 240, height: 22)
        let subtitle = NSTextField(labelWithString: "Mac Switch Speaker  ·  1.0.7")
        subtitle.font = .systemFont(ofSize: 11); subtitle.textColor = .secondaryLabelColor
        subtitle.frame = NSRect(x: 16, y: 8, width: 280, height: 17)
        header.addSubview(title); header.addSubview(subtitle)
        let item = NSMenuItem(); item.view = header; menu.addItem(item)
        statusLabel.isEnabled = false; menu.addItem(statusLabel)
        toggleItem.target = self; menu.addItem(toggleItem)
        let note = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 40))
        let noteLabel = NSTextField(wrappingLabelWithString: "接收开启时，系统蓝牙耳机和键鼠会断开。声音从 Mac 内置扬声器播放。")
        noteLabel.font = .systemFont(ofSize: 11); noteLabel.textColor = .secondaryLabelColor
        noteLabel.frame = NSRect(x: 16, y: 3, width: 288, height: 34); note.addSubview(noteLabel)
        let noteItem = NSMenuItem(); noteItem.view = note; menu.addItem(noteItem)
        menu.addItem(.separator())
        for view in [master, gain] { let item = NSMenuItem(); item.view = view; menu.addItem(item) }
        refreshItem.target = self
        refreshItem.toolTip = "丢弃积压的旧声音，短暂停顿后继续播放，保留连接和音量设置。"
        menu.addItem(refreshItem)
        menu.addItem(.separator()); sourcesMarker.isEnabled = false; menu.addItem(sourcesMarker)
        noSources.isEnabled = false; menu.addItem(noSources)
        menu.addItem(.separator()); pairedItem.isEnabled = false; menu.addItem(pairedItem)
        pairingItem.target = self
        pairingItem.toolTip = "只控制陌生设备；已信任设备关闭此项后仍可重连或重新认证。首次使用请开启并成功连接一次。"
        menu.addItem(pairingItem)
        menu.addItem(.separator())
        let about = NSMenuItem(title: "使用说明", action: #selector(showHelp), keyEquivalent: ""); about.target = self; menu.addItem(about)
        let showLog = NSMenuItem(title: "查看诊断日志", action: #selector(showLogs), keyEquivalent: ""); showLog.target = self; menu.addItem(showLog)
        let quit = NSMenuItem(title: "退出并恢复蓝牙", action: #selector(quitApp), keyEquivalent: "q"); quit.target = self; menu.addItem(quit)
    }
    private func updateStatus() {
        let active = receiving.phase != .off
        switch phase {
        case "starting": statusLabel.title = permissionPending ? "请在 Mac 允许蓝牙权限…" : "正在开启接收…"
        case "stopping": statusLabel.title = "正在恢复系统蓝牙…"
        case "on":
            let playing = sources.filter { ($0["playing"] as? Bool) == true }.count
            statusLabel.title = sources.isEmpty ? "单路自动切换 · 等待音源连接" : (playing > 0 ? "单路自动切换 · 正在播放" : "单路自动切换 · 音源已连接")
        default: statusLabel.title = lastError == nil ? "接收已关闭 · 系统蓝牙可用" : (lastError ?? "接收已关闭")
        }
        toggleItem.title = active ? "关闭音频接收" : "开启音频接收"
        toggleItem.state = active ? .on : .off
        toggleItem.isEnabled = phase != "stopping"
        refreshItem.isEnabled = ready && !sources.isEmpty
        pairingItem.state = pairing ? .on : .off
        let symbol = ready ? "hifispeaker.2.fill" : "hifispeaker.2"
        statusItem?.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: "声桥")
        statusItem?.button?.image?.isTemplate = true
        statusItem?.button?.setAccessibilityLabel("声桥：\(statusLabel.title)")
        statusItem?.button?.toolTip = "声桥 · \(statusLabel.title)"
    }
    private func updateSources() {
        let ids = sources.compactMap { $0["id"] as? Int }
        if Set(ids) != Set(sourceRows.keys) {
            for item in sourceItems { menu.removeItem(item) }; sourceItems.removeAll(); sourceRows.removeAll(); sourceLabels.removeAll()
            if menu.items.contains(noSources) { menu.removeItem(noSources) }
            var index = menu.index(of: sourcesMarker) + 1
            if sources.isEmpty { menu.insertItem(noSources, at: index) }
            for source in sources {
                guard let id = source["id"] as? Int, let address = source["address"] as? String else { continue }
                let title = NSMenuItem(title: "", action: nil, keyEquivalent: "")
                let header = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 29))
                let label = NSTextField(labelWithString: "")
                label.font = .systemFont(ofSize: 12, weight: .medium)
                label.lineBreakMode = .byTruncatingTail
                label.frame = NSRect(x: 16, y: 6, width: 224, height: 18)
                let rename = NSButton(title: "备注…", target: self, action: #selector(renameSource(_:)))
                rename.bezelStyle = .rounded; rename.controlSize = .small
                rename.identifier = NSUserInterfaceItemIdentifier(address)
                rename.frame = NSRect(x: 251, y: 3, width: 55, height: 24)
                rename.setAccessibilityLabel("设置设备备注 \(address.suffix(5))")
                header.addSubview(label); header.addSubview(rename); title.view = header
                sourceLabels[id] = label
                menu.insertItem(title, at: index); sourceItems.append(title); index += 1
                let row = SliderRow(title: "此音源音量", minimum: 0, maximum: 100, value: sourceVolumes[address] ?? 100)
                row.changed = { [weak self] value in
                    guard let self = self else { return }
                    self.sourceVolumes[address] = value; self.settings.set(self.sourceVolumes, forKey: "sourceVolumes")
                    self.send("SOURCE \(id) \(Int(value.rounded()))")
                }
                let rowItem = NSMenuItem(); rowItem.view = row; menu.insertItem(rowItem, at: index)
                sourceItems.append(rowItem); sourceRows[id] = row; index += 1
            }
        }
        for source in sources {
            guard let id = source["id"] as? Int, let address = source["address"] as? String else { continue }
            let playing = (source["playing"] as? Bool) == true
            let savedName = settings.string(forKey: "name.\(address)") ?? "音源 \(address.suffix(5))"
            sourceLabels[id]?.stringValue = "\(savedName) · \(playing ? "播放中" : "已连接")"
            sourceLabels[id]?.toolTip = "\(savedName) · \(address)"
            if !appliedSources.contains(id) {
                appliedSources.insert(id); send("SOURCE \(id) \(Int((sourceVolumes[address] ?? 100).rounded()))")
            }
            if NSEvent.pressedMouseButtons == 0 { sourceRows[id]?.update(sourceVolumes[address] ?? 100) }
        }
    }
    @objc private func toggle() { receiving.toggle() }
    @objc private func refreshBuffers() { send("FLUSH") }
    @objc private func renameSource(_ sender: NSButton) {
        guard let address = sender.identifier?.rawValue else { return }
        menu.cancelTrackingWithoutAnimation()
        // Open only after the menu tracking loop has returned to default mode.
        RunLoop.main.perform(inModes: [.default]) { [weak self] in
            self?.showRemarkDialog(address: address)
        }
    }
    private func showRemarkDialog(address: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "设备备注"
        alert.informativeText = "只修改这台 Mac 中显示的名称。留空可恢复默认名称。"
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        field.isEditable = true; field.isSelectable = true
        field.stringValue = settings.string(forKey: "name.\(address)") ?? ""
        field.placeholderString = "例如 Switch、游戏电脑"
        field.setAccessibilityLabel("设备备注")
        alert.accessoryView = field
        alert.addButton(withTitle: "保存"); alert.addButton(withTitle: "取消")
        alert.window.initialFirstResponder = field
        RunLoop.main.perform(inModes: [.modalPanel]) {
            NSApp.activate(ignoringOtherApps: true)
            alert.window.makeKeyAndOrderFront(nil)
            alert.window.makeFirstResponder(field)
            field.selectText(nil)
        }
        if alert.runModal() == .alertFirstButtonReturn {
            let name = String(field.stringValue.components(separatedBy: .controlCharacters)
                .joined(separator: " ").trimmingCharacters(in: .whitespacesAndNewlines).prefix(64))
            if name.isEmpty { settings.removeObject(forKey: "name.\(address)") }
            else { settings.set(name, forKey: "name.\(address)") }
            updateSources()
        }
    }
    @objc private func togglePairing() {
        pairing.toggle(); settings.set(pairing, forKey: "pairing"); send("PAIRING \(pairing ? 1 : 0)"); updateStatus()
    }
    private func send(_ text: String) {
        guard let data = (text + "\n").data(using: .utf8), let input = input else { return }
        do { try input.write(contentsOf: data) } catch { /* Supervisor/guardian performs recovery. */ }
    }
    private func startProcess() {
        guard runner == nil else { return }
        switch CBManager.authorization {
        case .allowedAlways: launchReceiver()
        case .notDetermined:
            permissionPending = true
            NSApp.activate(ignoringOtherApps: true)
            permissionManager = CBCentralManager(delegate: self, queue: .main,
                options: [CBCentralManagerOptionShowPowerAlertKey: false])
            updateStatus()
        default: permissionDenied()
        }
    }
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard permissionPending, receiving.phase == .starting, receiving.desired else { return }
        switch CBManager.authorization {
        case .allowedAlways:
            permissionPending = false; permissionManager = nil
            launchReceiver()
        case .denied, .restricted:
            permissionPending = false; permissionManager = nil
            permissionDenied()
        default: break
        }
    }
    private func permissionDenied() {
        lastError = "请在系统设置 → 隐私与安全性 → 蓝牙中允许“声桥”，然后重新开启接收"
        receiving.didStop(error: lastError)
    }
    private func launchReceiver() {
        guard runner == nil, let resources = Bundle.main.resourceURL else { return }
        lastError = nil; ready = false; lastOutput = Date()
        statusTimer?.invalidate()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            guard let self = self, self.runner != nil, Date().timeIntervalSince(self.lastOutput) > 24 else { return }
            self.lastError = "接收程序未响应，正在恢复系统蓝牙"; self.receiving.fail(self.lastError!)
        }
        appliedSources.removeAll(); sources.removeAll(); updateSources(); updateStatus()
        let process = Process(), incoming = Pipe(), outgoing = Pipe()
        process.executableURL = resources.appendingPathComponent("session-supervisor")
        process.arguments = ["--run", resources.path, support.path]
        process.standardInput = incoming; process.standardOutput = outgoing; process.standardError = outgoing
        process.environment = ProcessInfo.processInfo.environment.merging(["LC_ALL": "en_US.UTF-8"]) { _, new in new }
        let decoder = OutputLines(), identifier = UUID()
        sessionID = identifier; input = incoming.fileHandleForWriting; runner = process
        outgoing.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { handle.readabilityHandler = nil; return }
            self?.ioQueue.async { [weak self] in self?.consume(data, decoder: decoder, identifier: identifier) }
        }
        process.terminationHandler = { [weak self, weak process] _ in
            DispatchQueue.main.async { [weak self] in
                guard let self = self, let process = process, self.runner === process else { return }
                try? self.logHandle?.write(contentsOf: Data("SESSION_EXIT status=\(process.terminationStatus) reason=\(process.terminationReason.rawValue) phase=\(self.phase)\n".utf8))
                self.runner = nil; try? self.input?.close(); self.input = nil
                self.statusTimer?.invalidate(); self.statusTimer = nil
                self.ready = false; self.sources.removeAll(); self.appliedSources.removeAll()
                if self.receiving.phase != .stopping && self.lastError == nil {
                    self.lastError = "接收程序已退出"
                }
                self.receiving.didStop(error: self.lastError)
                self.updateSources(); self.updateStatus()
                if self.quitting { NSApp.reply(toApplicationShouldTerminate: true) }
            }
        }
        do { try process.run() } catch {
            runner = nil; input = nil; lastError = "无法启动：\(error.localizedDescription)"
            receiving.didStop(error: lastError); updateStatus()
        }
    }
    private func consume(_ data: Data, decoder: OutputLines, identifier: UUID) {
        try? logHandle?.write(contentsOf: data)
        for object in decoder.objects(data) {
            DispatchQueue.main.async { [weak self] in
                guard let self = self, self.sessionID == identifier else { return }
                self.event(object)
            }
        }
    }
    private func event(_ object: [String: Any]) {
        lastOutput = Date()
        switch object["event"] as? String {
        case "ready":
            ready = true; receiving.didStart()
            send("MASTER \(Int(master.slider.doubleValue.rounded()))"); send("GAIN \(Int(gain.slider.doubleValue.rounded()))")
            send("PAIRING \(pairing ? 1 : 0)")
        case "sources":
            sources = object["sources"] as? [[String: Any]] ?? []; updateSources()
            if let count = object["paired"] as? Int { settings.set(count, forKey: "pairedCount"); pairedItem.title = "已记住 \(count) 台设备" }
        case "pairings":
            if let count = object["count"] as? Int { settings.set(count, forKey: "pairedCount"); pairedItem.title = "已记住 \(count) 台设备" }
        case "error":
            lastError = object["message"] as? String ?? "接收失败"
            receiving.fail(lastError!)
        case "stopped": ready = false
        default: break
        }
        updateStatus()
    }
    private func stopProcess() {
        if permissionPending {
            permissionPending = false; permissionManager = nil
            receiving.didStop(); return
        }
        guard runner != nil else { return }
        send("STOP"); updateStatus()
    }
    @objc private func willSleep() { receiving.request(false) }
    private func controlResult(ok: Bool = true, code: String? = nil) -> [String: Any] {
        var result: [String: Any] = ["ok": ok, "state": phase, "receiving": receiving.phase == .on && ready,
            "requested": receiving.desired, "ready": ready, "running": true,
            "sources": sources.count, "connectionLimit": 1, "switchingPolicy": "newestWins", "version": "1.0.7"]
        if permissionPending { result["message"] = "请在 Mac 上允许声桥使用蓝牙" }
        if let code = code { result["code"] = code }
        if let failure = receiving.failure { result["message"] = failure }
        return result
    }
    private func handleControl(_ command: ReceiveCommand, reply: @escaping LocalControlServer.Reply) {
        try? logHandle?.write(contentsOf: Data("CONTROL_CMD \(command.rawValue) phase=\(phase)\n".utf8))
        if command == .status { reply(controlResult()); return }
        if quitting { reply(controlResult(ok: false, code: "app_exiting")); return }
        if command == .toggle { receiving.toggle() }
        else { receiving.request(command == .on) }
        let target = receiving.desired, revision = receiving.revision
        awaitControl(target: target, revision: revision, deadline: ProcessInfo.processInfo.systemUptime+30, reply: reply)
    }
    private func awaitControl(target: Bool, revision: UInt64, deadline: TimeInterval, reply: @escaping LocalControlServer.Reply) {
        if receiving.failure != nil { reply(controlResult(ok: false, code: "receiver_error")); return }
        if receiving.revision != revision { reply(controlResult(ok: false, code: "superseded")); return }
        if (target && receiving.phase == .on && ready) || (!target && receiving.phase == .off) {
            reply(controlResult()); return
        }
        if ProcessInfo.processInfo.systemUptime > deadline {
            reply(controlResult(ok: false, code: permissionPending ? "needs_bluetooth_permission" : "timeout")); return
        }
        DispatchQueue.main.asyncAfter(deadline: .now()+0.05) { [weak self] in
            guard let self = self else { reply(["ok": false, "code": "app_exiting"]); return }
            self.awaitControl(target: target, revision: revision, deadline: deadline, reply: reply)
        }
    }
    @objc private func showLogs() { NSWorkspace.shared.open(logs.appendingPathComponent("receiver.log")) }
    @objc private func showHelp() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert(); alert.messageText = "声桥 1.0.7"
        alert.informativeText = "开启接收后，在音源的蓝牙设置连接 Mac Switch Speaker。同时只播放一台音源。新设备连接时自动断开旧设备并接管声音，无需操作 Mac；可以记住多台设备。关闭“允许新设备配对”后，已信任设备仍可重连；没有历史记录的设备需先开启并成功配对一次。\n\n接收没有五分钟上限，配对保存在本机。接收期间系统蓝牙耳机、键鼠会断开；关闭或退出后恢复。\n\n感觉声音积压时，可点“刷新音频缓冲”，保留连接并丢弃旧声音。播放缓冲从短缓冲开始，仅在实际断流时增加；蓝牙传输本身仍有延迟。\n\n音量可实时调整，放大增益最高400%。过高的增益可能触发限幅。系统更新可能影响此原生接口的兼容性。"
        alert.addButton(withTitle: "知道了"); alert.runModal()
    }
    @objc private func quitApp() { NSApp.terminate(nil) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if runner != nil { quitting = true; receiving.request(false); return .terminateLater }; return .terminateNow
    }
    func applicationWillTerminate(_ notification: Notification) { controlServer?.stop() }
    func menuWillOpen(_ menu: NSMenu) { updateSources(); updateStatus() }
}

@main
struct BluetoothMixerMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = BluetoothMixerApp()
        app.delegate = delegate
        withExtendedLifetime(delegate) { app.run() }
    }
}

import Foundation

enum ReceiveCommand: String {
    case on, off, toggle, status
}

/// Single desired state for menu and automation. A new start waits for restoration.
final class ReceivingControl {
    enum Phase: String { case off, starting, on, stopping }
    private(set) var phase: Phase = .off
    private(set) var desired = false
    private(set) var revision: UInt64 = 0
    private(set) var failure: String?
    var onStart: (() -> Void)?
    var onStop: (() -> Void)?
    var onChange: (() -> Void)?

    func request(_ enabled: Bool) {
        if desired != enabled || failure != nil { revision &+= 1 }
        desired = enabled; failure = nil
        reconcile()
    }
    func toggle() { request(!desired) }
    func didStart() {
        if phase == .starting { phase = .on }
        reconcile()
    }
    func didStop(error: String? = nil) {
        phase = .off
        if let error = error { failure = error; desired = false; revision &+= 1 }
        reconcile()
    }
    func fail(_ message: String) {
        failure = message; desired = false; revision &+= 1
        reconcile()
    }
    private func reconcile() {
        if desired && phase == .off {
            phase = .starting; onStart?()
        } else if !desired && (phase == .on || phase == .starting) {
            phase = .stopping; onStop?()
        }
        onChange?()
    }
}

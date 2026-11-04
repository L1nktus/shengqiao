import Foundation

@main
struct ReceivingControlTests {
    static func main() {
        let control = ReceivingControl()
        var starts=0,stops=0
        control.onStart={starts+=1};control.onStop={stops+=1}
        control.request(true);control.request(true)
        precondition(starts==1 && control.phase == .starting)
        let revision=control.revision;control.didStart();control.request(true)
        precondition(starts==1 && control.phase == .on && control.revision==revision)
        control.request(false);control.request(false)
        precondition(stops==1 && control.phase == .stopping)
        control.request(true)
        precondition(starts==1 && control.phase == .stopping && control.desired)
        control.didStop()
        precondition(starts==2 && control.phase == .starting)
        control.request(false);control.didStart()
        precondition(stops==2 && control.phase == .stopping && !control.desired)
        control.didStop();precondition(control.phase == .off)
        control.request(true);control.didStop(error:"synthetic failure")
        precondition(starts==3 && !control.desired && control.failure != nil && control.phase == .off)
        control.request(true);precondition(starts==4 && control.failure==nil)
        control.didStart();control.fail("synthetic watchdog")
        precondition(stops==3 && control.phase == .stopping && !control.desired)
        control.didStop(error:control.failure);precondition(control.phase == .off && starts==4)
        control.request(false);precondition(control.failure==nil && stops==3)
        print("PASS: idempotent on/off, restart waits for restoration, superseded startup, failure/retry and watchdog shutdown")
    }
}

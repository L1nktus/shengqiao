import Foundation
import Darwin

@main
struct ControlTestHost {
    static func main() throws {
        let control=ReceivingControl(),server=LocalControlServer()
        var starts=0,stops=0
        control.onStart={starts+=1;DispatchQueue.main.asyncAfter(deadline:.now()+0.12){control.didStart()}}
        control.onStop={stops+=1;DispatchQueue.main.asyncAfter(deadline:.now()+0.12){control.didStop()}}
        func snapshot(_ ok:Bool=true,_ code:String="") -> [String:Any] {
            ["ok":ok,"code":code,"state":control.phase.rawValue,"requested":control.desired,"starts":starts,"stops":stops]
        }
        func wait(_ target:Bool,_ revision:UInt64,_ reply:@escaping LocalControlServer.Reply) {
            if control.revision != revision {reply(snapshot(false,"superseded"));return}
            if (target && control.phase == .on) || (!target && control.phase == .off){reply(snapshot());return}
            DispatchQueue.main.asyncAfter(deadline:.now()+0.01){wait(target,revision,reply)}
        }
        try server.start(at:URL(fileURLWithPath:CommandLine.arguments[1])){command,reply in
            if command == .status {reply(snapshot());return}
            if command == .toggle {control.toggle()}else{control.request(command == .on)}
            wait(control.desired,control.revision,reply)
        }
        print("READY");fflush(stdout)
        withExtendedLifetime(server){dispatchMain()}
    }
}

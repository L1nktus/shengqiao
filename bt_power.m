#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#include <string.h>
extern int IOBluetoothPreferenceGetControllerPowerState(void);
extern void IOBluetoothPreferenceSetControllerPowerState(int state);

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc == 2 && !strcmp(argv[1], "state")) {
        printf("%d\n", IOBluetoothPreferenceGetControllerPowerState());
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "off") && strcmp(argv[1], "on"))) return 64;
    const int wanted = !strcmp(argv[1], "on");
    IOBluetoothPreferenceSetControllerPowerState(wanted);
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:4.0];
    do {
        if (IOBluetoothPreferenceGetControllerPowerState() == wanted) {
            printf("POWER_CONFIRMED %d\n", wanted);
            return 0;
        }
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    } while (end.timeIntervalSinceNow > 0);
    puts("POWER_NOT_CONFIRMED");
    return 3;
} }

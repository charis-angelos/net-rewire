//
//  AppDelegate.m
//  NetRewireApp
//
//  Manages net-rewire-daemon via launchd (no Network Extension dependency).
//

#import "AppDelegate.h"
#import <ServiceManagement/ServiceManagement.h>

static NSString * const kDaemonLabel = @"com.netrewire.daemon";

@interface AppDelegate ()
@property (strong) NSTextField *statusLabel;
@property (strong) NSTimer     *statusTimer;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    self.window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 400, 300)
                  styleMask:NSWindowStyleMaskTitled
                    | NSWindowStyleMaskClosable
                    | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [self.window setTitle:@"Net-Rewire"];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];

    /* ── Buttons ── */
    NSButton *startBtn = [[NSButton alloc]
        initWithFrame:NSMakeRect(150, 150, 100, 30)];
    [startBtn setTitle:@"Start"];
    [startBtn setButtonType:NSButtonTypeMomentaryPushIn];
    [startBtn setBezelStyle:NSBezelStyleRounded];
    [startBtn setTarget:self];
    [startBtn setAction:@selector(startDaemon:)];

    NSButton *stopBtn = [[NSButton alloc]
        initWithFrame:NSMakeRect(150, 100, 100, 30)];
    [stopBtn setTitle:@"Stop"];
    [stopBtn setButtonType:NSButtonTypeMomentaryPushIn];
    [stopBtn setBezelStyle:NSBezelStyleRounded];
    [stopBtn setTarget:self];
    [stopBtn setAction:@selector(stopDaemon:)];

    /* ── Status label ── */
    self.statusLabel = [[NSTextField alloc]
        initWithFrame:NSMakeRect(50, 50, 300, 30)];
    [self.statusLabel setStringValue:@"Status: checking…"];
    [self.statusLabel setBezeled:NO];
    [self.statusLabel setDrawsBackground:NO];
    [self.statusLabel setEditable:NO];
    [self.statusLabel setSelectable:NO];

    NSView *content = self.window.contentView;
    [content addSubview:startBtn];
    [content addSubview:stopBtn];
    [content addSubview:self.statusLabel];

    /* Periodically refresh status */
    self.statusTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                        target:self
                                                      selector:@selector(refreshStatus)
                                                      userInfo:nil
                                                       repeats:YES];
    [self refreshStatus];
}

/* ── Daemon control via launchctl ────────────────────────────────── */

- (BOOL)isDaemonRunning {
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = @"/bin/launchctl";
    task.arguments = @[@"list", kDaemonLabel];

    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = [NSFileHandle fileHandleWithNullDevice];

    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException *e) {
        return NO;
    }

    return task.terminationStatus == 0;
}

- (void)startDaemon:(id)sender {
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = @"/bin/launchctl";
    task.arguments = @[@"load", @"/Library/LaunchDaemons/com.netrewire.daemon.plist"];

    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException *e) {
        NSLog(@"Failed to start daemon: %@", e);
    }

    [self refreshStatus];
}

- (void)stopDaemon:(id)sender {
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = @"/bin/launchctl";
    task.arguments = @[@"unload", @"/Library/LaunchDaemons/com.netrewire.daemon.plist"];

    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException *e) {
        NSLog(@"Failed to stop daemon: %@", e);
    }

    [self refreshStatus];
}

- (void)refreshStatus {
    if ([self isDaemonRunning]) {
        self.statusLabel.stringValue = @"Status: Running — SMTP tunnel active";
    } else {
        self.statusLabel.stringValue = @"Status: Stopped";
    }
}

- (void)applicationWillTerminate:(NSNotification *)aNotification {
    [self.statusTimer invalidate];
    self.statusTimer = nil;
}

@end

#import "JITHelperClient.h"
#import <sys/sysctl.h>
#import <unistd.h>

// NSExtension SPI is needed to run the debugger in a different process.
@interface NSObject (MCJITExtension)
+ (id)extensionWithIdentifier:(NSString *)identifier excludingDisabledExtensions:(BOOL)exclude error:(NSError **)error;
- (NSUUID *)beginExtensionRequestWithInputItems:(NSArray *)items error:(NSError **)error;
- (void)_kill:(int)signal;
@end

@interface JITHelperClient () <NSXPCListenerDelegate, MCJITProgress>
@property NSXPCListener *listener;
@property NSXPCConnection *connection;
@property NSObject *helper;
@property NSData *pairingData;
@property BOOL forceScript;
@property BOOL invalidated;
@property (copy) void (^statusBlock)(NSString *);
@property (copy) void (^attachedBlock)(void);
@property (copy) void (^finishedBlock)(NSString *);
@end

@implementation JITHelperClient

- (void)startWithPairingData:(NSData *)data forceScript:(BOOL)forceScript
                    status:(void (^)(NSString *))status attached:(void (^)(void))attached
                  finished:(void (^)(NSString *))finished
{
    self.pairingData = data;
    self.forceScript = forceScript;
    self.statusBlock = status;
    self.attachedBlock = attached;
    self.finishedBlock = finished;
    self.listener = NSXPCListener.anonymousListener;
    self.listener.delegate = self;
    [self.listener resume];

    NSString *identifier = nil;
    NSURL *pluginsURL = NSBundle.mainBundle.builtInPlugInsURL;
    NSArray<NSURL *> *plugins = pluginsURL ? [NSFileManager.defaultManager contentsOfDirectoryAtURL:pluginsURL
                                                        includingPropertiesForKeys:nil options:0 error:nil] : @[];
    for (NSURL *url in plugins)
    {
        NSBundle *bundle = [NSBundle bundleWithURL:url];
        if ([bundle.bundleIdentifier hasSuffix:@".JITEnabler"])
            identifier = bundle.bundleIdentifier;
    }
    Class extensionClass = NSClassFromString(@"NSExtension");
    if (!identifier || ![extensionClass respondsToSelector:@selector(extensionWithIdentifier:excludingDisabledExtensions:error:)])
    {
        [self finishedWithError:@"The JIT helper is missing from this installation. Reinstall MeloCafe with its app extension."];
        return;
    }
    NSError *error = nil;
    self.helper = [extensionClass extensionWithIdentifier:identifier excludingDisabledExtensions:NO error:&error];
    if (![self.helper respondsToSelector:@selector(beginExtensionRequestWithInputItems:error:)])
    {
        [self finishedWithError:error.localizedDescription ?: @"Unable to start the JIT helper."];
        return;
    }
    NSExtensionItem *item = [NSExtensionItem new];
    item.userInfo = @{MCJITEndpointKey: self.listener.endpoint};
    if (![self.helper beginExtensionRequestWithInputItems:@[item] error:&error])
        [self finishedWithError:error.localizedDescription ?: @"Unable to launch the JIT helper."];

    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (weakSelf && !weakSelf.invalidated && !weakSelf.connection)
            [weakSelf finishedWithError:@"Timed out starting the JIT helper."];
    });
}

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
    // Listener callbacks arrive on an XPC queue; serialize ownership with invalidate.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.invalidated || self.connection)
        {
            [connection invalidate];
            return;
        }
        self.connection = connection;
        connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MCJITProgress)];
        connection.exportedObject = self;
        connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MCJITService)];
        __weak typeof(self) weakSelf = self;
        connection.interruptionHandler = ^{ [weakSelf finishedWithError:@"The JIT helper was interrupted. Try enabling JIT again."]; };
        connection.invalidationHandler = ^{ [weakSelf finishedWithError:@"The JIT helper disconnected. Try enabling JIT again."]; };
        [connection resume];
        id<MCJITService> service = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
            [weakSelf finishedWithError:error.localizedDescription];
        }];
        [service enableJITForPID:getpid() pairingData:self.pairingData forceScript:self.forceScript];
        self.pairingData = nil;
    });
    return YES;
}

- (void)updateStatus:(NSString *)status
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.invalidated)
            self.statusBlock(status);
    });
}

- (void)debuggerAttached
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.invalidated)
            self.attachedBlock();
    });
}

- (void)finishedWithError:(NSString *)error
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.invalidated)
            self.finishedBlock(error);
    });
}

- (void)invalidate
{
    self.invalidated = YES;
    self.connection.interruptionHandler = nil;
    self.connection.invalidationHandler = nil;
    [self.connection invalidate];
    self.connection = nil;
    [self.listener invalidate];
    self.listener = nil;
    if ([self.helper respondsToSelector:@selector(_kill:)])
        [self.helper _kill:SIGKILL];
    self.helper = nil;
    self.pairingData = nil;
    self.statusBlock = nil;
    self.attachedBlock = nil;
    self.finishedBlock = nil;
}
@end

extern int csops(pid_t, unsigned int, void *, size_t);
BOOL MCJITIsDebugged(void)
{
    uint32_t flags = 0;
    return csops(getpid(), 0, &flags, sizeof(flags)) == 0 && (flags & 0x10000000) != 0;
}

BOOL MCJITIsTraced(void)
{
    struct kinfo_proc info = {0};
    size_t size = sizeof(info);
    int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(query, 4, &info, &size, NULL, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0;
}

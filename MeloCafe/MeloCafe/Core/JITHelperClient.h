#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Shared by the host and its private helper; pairing credentials never leave the device.
static NSString *const MCJITEndpointKey = @"MeloCafeJITEndpoint";

@protocol MCJITService
- (void)enableJITForPID:(int)pid pairingData:(NSData *)data forceScript:(BOOL)forceScript;
@end

@protocol MCJITProgress
- (void)updateStatus:(NSString *)status;
- (void)debuggerAttached;
- (void)finishedWithError:(nullable NSString *)error;
@end

@interface JITHelperClient : NSObject
// All callbacks are delivered on the main queue. Retain the client until finished.
- (void)startWithPairingData:(NSData *)data
               forceScript:(BOOL)forceScript
                    status:(void (^)(NSString *))status
                  attached:(void (^)(void))attached
                  finished:(void (^)(NSString * _Nullable))finished;
- (void)invalidate;
@end

BOOL MCJITIsDebugged(void);
BOOL MCJITIsTraced(void);

NS_ASSUME_NONNULL_END

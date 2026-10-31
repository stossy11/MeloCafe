//
//  MLXRamDonor.h
//  MeloNX
//
//  Created by Stossy11 on 29/9/2026.
//

#ifndef MLXRamDonor_h
#define MLXRamDonor_h

@import Foundation;

NS_ASSUME_NONNULL_BEGIN

@interface MLXDonorLogs : NSObject
+ (NSArray<NSString *> *)snapshot;
+ (void)clear;
@end

@interface MLXRamDonor : NSObject

@property (class, readonly) MLXRamDonor *shared;
@property (class, readonly, nullable) MLXRamDonor *sharedIfExists;
@property (readonly) NSUInteger liveDonorCount;
@property (nonatomic) NSUInteger maxDonors;
@property (nonatomic) bool killedAll;

- (NSUInteger)startDonors:(NSUInteger)count timeout:(NSTimeInterval)timeout;
- (nullable void *)acquireBytes:(uint64_t)size;
- (nullable void *)acquireUpTo:(uint64_t)size
                       minimum:(uint64_t)minimum
                        actual:(uint64_t *_Nullable)actual;
- (void)releaseRegionAt:(void *)base;

@property (nonatomic, readonly, copy, nullable) NSString *lastError;

@property (class, readonly, nullable) NSString *donorBundleIdentifier;

- (uint64_t)donatedBytes;
- (uint64_t)donorAvailableMemory;

- (void)shutdown;

// Kept for source compatibility; every batch now exits after mapping.
@property (nonatomic) BOOL killDonorsAfterMapping;

- (void)killDonorProcesses;

@end

NS_ASSUME_NONNULL_END

#ifdef __cplusplus
extern "C" {
#endif

uint32_t mlx_ram_donor_start(uint32_t count, double timeoutSeconds);

void *_Nullable mlx_ram_donor_acquire(uint64_t size);

void *_Nullable mlx_ram_donor_acquire_up_to(uint64_t size, uint64_t minimum,
                                            uint64_t *_Nullable actual);

void mlx_ram_donor_release(void *_Nonnull base);

uint32_t mlx_ram_donor_live_count(void);

uint64_t mlx_ram_donor_available_memory(void);

bool mlx_ram_donor_is_available(void);

const char *_Nullable mlx_ram_donor_last_error(void);

void mlx_ram_donor_shutdown(void);

void mlx_ram_donor_set_kill_after_mapping(bool enabled);

void mlx_ram_donor_kill_processes(void);

#ifdef __cplusplus
}
#endif

#endif /* MLXRamDonor_h */

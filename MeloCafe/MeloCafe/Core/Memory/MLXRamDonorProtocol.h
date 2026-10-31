//
//  MLXRamDonorProtocol.h
//  MeloNX
//
//  Created by Stossy11 on 29/9/2026.
//

#ifndef MLXRamDonorProtocol_h
#define MLXRamDonorProtocol_h

@import Foundation;
#import <mach/mach.h>
#import <xpc/xpc.h>


#define MLX_DONOR_CHUNK_SIZE (256ull * 1024ull * 1024ull)

#pragma mark Message keys

// Request: which operation. One of the MLXDonorOp values below.
#define MLXDonorKeyOp        "op"
// Request: bytes wanted, for MLXDonorOpCreate.
#define MLXDonorKeySize      "size"
// Reply: the shared memory, for MLXDonorOpCreate.
#define MLXDonorKeyShmem     "shmem"
// Reply: why the operation failed. Absent on success.
#define MLXDonorKeyError     "error"
// Reply: os_proc_available_memory() in the donor, for MLXDonorOpAvailable.
#define MLXDonorKeyAvailable "available"
// Reply: task info phys_footprint from the donor, for MLXDonorOpPhysMem.
#define MLXDonorKeyPhysMem   "phys_footprint"

// TODO: actually add the debugger stuff and handle when we kill after mapping.

// Reply: allocated/blessed memory from the debugger, for MLXDonorOpBless.
#define MLXDonorKeyExecMem   "exec_memory"

#pragma mark Operations

// Allocates purgeable memory owned by the donor and returns it as an xpc_shmem.
#define MLXDonorOpCreate     "create"
// Drops every region the donor owns. The app must have unmapped them first.
#define MLXDonorOpRelease    "release"
// Asks how much the donor may still allocate before jetsam kills it. This is the number that decides whether donating is worth anything.
#define MLXDonorOpAvailable  "available"
// Asks how much physical memory the donor is using.
#define MLXDonorOpPhysMem    "phys_footprint"
// Exits after acknowledging the request; the host keeps its shared mappings.
#define MLXDonorOpExit       "exit"

// TODO: actually add the debugger stuff and handle when we kill after mapping.

// Attaches the debugger.
#define MLXDonorOpAttach     "attach"
// Blesses provided memory mapping for exeution and returns it.
#define MLXDonorOpBless     "bless"
// Detaches the debugger.
#define MLXDonorOpDetach     "detach"

NS_ASSUME_NONNULL_BEGIN

@interface NSXPCListenerEndpoint (MLXPrivate)
@property (nonatomic, setter=_setEndpoint:) xpc_object_t _endpoint;
@end

static inline NSXPCListenerEndpoint *MLXWrapEndpoint(xpc_endpoint_t endpoint)
{
    NSXPCListenerEndpoint *wrapper = [NSXPCListenerEndpoint new];
    wrapper._endpoint = endpoint;

    return wrapper;
}

static inline xpc_endpoint_t _Nullable MLXUnwrapEndpoint(NSXPCListenerEndpoint *_Nullable wrapper)
{
    xpc_object_t inner = wrapper._endpoint;

    if (!inner || xpc_get_type(inner) != XPC_TYPE_ENDPOINT)
    {
        return nil;
    }

    return (xpc_endpoint_t)inner;
}

NS_ASSUME_NONNULL_END

#endif /* MLXRamDonorProtocol_h */

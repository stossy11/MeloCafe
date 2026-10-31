//
//  main.m
//  MeloNX
//
//  Created by Stossy11 on 2/10/2026.
//


@import Foundation;

#import <mach/mach.h>
#import <objc/runtime.h>
#import <os/proc.h>
#import <xpc/xpc.h>
#import "../MeloCafe/Core/Memory/MLXRamDonorProtocol.h"


#ifndef VM_FLAGS_PURGABLE
#define VM_FLAGS_PURGABLE 0x2
#endif

static NSMutableArray<NSValue *> *gRegions;

static void MLXReleaseRegions(void)
{
    for (NSUInteger i = 0; i + 1 < gRegions.count; i += 2)
    {
        const vm_address_t base = (vm_address_t)(uintptr_t)gRegions[i].pointerValue;
        const vm_size_t size = (vm_size_t)(uintptr_t)gRegions[i + 1].pointerValue;

        vm_deallocate(mach_task_self(), base, size);
    }

    [gRegions removeAllObjects];
}

static xpc_object_t MLXCreateRegion(uint64_t size, NSString **error)
{
    if (size == 0)
    {
        *error = @"Refusing to create a zero sized region";
        return nil;
    }

    vm_address_t address = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &address, size,
                                        VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);

    if (kr != KERN_SUCCESS)
    {
        *error = [NSString stringWithFormat:@"vm_allocate failed: %s",
                                            mach_error_string(kr)];
        return nil;
    }
    
    xpc_object_t shmem = xpc_shmem_create((void *)address, (size_t)size);

    if (!shmem)
    {
        vm_deallocate(mach_task_self(), address, size);

        *error = @"xpc_shmem_create returned NULL";
        return nil;
    }

    gRegions = gRegions ?: [NSMutableArray array];

    [gRegions addObject:[NSValue valueWithPointer:(void *)(uintptr_t)address]];
    [gRegions addObject:[NSValue valueWithPointer:(void *)(uintptr_t)size]];

    return shmem;
}

static uint64_t getMemoryFootprint(void)
{
    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    
    kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count);
    
    if (result != KERN_SUCCESS) {
        return 0; // Failed to fetch metrics
    }
    
    
    return (uint64_t)vmInfo.phys_footprint;
}

static void MLXHandleMessage(xpc_connection_t channel, xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY)
    {
        return;
    }
    
    const char *op = xpc_dictionary_get_string(message, MLXDonorKeyOp);
    
    if (!op)
    {
        return;
    }
    
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    
    if (!reply)
    {
        return;
    }
    
    if (strcmp(op, MLXDonorOpCreate) == 0)
    {
        NSString *error = nil;
        xpc_object_t shmem = MLXCreateRegion(xpc_dictionary_get_uint64(message, MLXDonorKeySize), &error);
        
        if (shmem)
        {
            xpc_dictionary_set_value(reply, MLXDonorKeyShmem, shmem);
        }
        else
        {
            xpc_dictionary_set_string(reply, MLXDonorKeyError, error.UTF8String);
            NSLog(@"Could not create a region: %@", error);
        }
    }
    else if (strcmp(op, MLXDonorOpRelease) == 0)
    {
        MLXReleaseRegions();
    }
    else if (strcmp(op, MLXDonorOpAvailable) == 0)
    {
        xpc_dictionary_set_uint64(reply, MLXDonorKeyAvailable,
                                  (uint64_t)os_proc_available_memory());
    }
    else if (strcmp(op, MLXDonorOpPhysMem) == 0)
    {
        xpc_dictionary_set_uint64(reply, MLXDonorKeyPhysMem,
                                  (uint64_t)getMemoryFootprint());
    }
    else if (strcmp(op, MLXDonorOpExit) == 0)
    {
        xpc_connection_send_message(channel, reply);
        xpc_connection_send_barrier(channel, ^{ exit(0); });
        return;
    }
    else
    {
        xpc_dictionary_set_string(reply, MLXDonorKeyError, "Unknown operation");
    }

    xpc_connection_send_message(channel, reply);
}


static void mlx_do_nothing(void) {}

__attribute__((constructor))
static void MLXAllowEndpointDecoding(void)
{
    Class decoder = NSClassFromString(@"NSXPCDecoder");
    SEL selector = NSSelectorFromString(@"_validateAllowedClass:forKey:allowingInvocations:");
    Method method = class_getInstanceMethod(decoder, selector);

    if (method)
    {
        method_setImplementation(method, (IMP)mlx_do_nothing);
        NSLog(@"Loaded in pid %d, decoder validation disabled", getpid());
    }
    else
    {
        NSLog(@"Loaded in pid %d, but NSXPCDecoder validation hook was NOT found", getpid());
    }
}

@interface MLXRamDonorHandler : NSObject <NSExtensionRequestHandling>
@end

@implementation MLXRamDonorHandler
{
    xpc_connection_t _channel;
}

- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
    NSLog(@"beginRequest with %lu input item(s)",
          (unsigned long)context.inputItems.count);
    
    NSExtensionItem *item = (NSExtensionItem *)context.inputItems.firstObject;
    NSDictionary *info = item.userInfo;
    id wrapper = info[@"endpoint"];
    
    if (wrapper && ![wrapper isKindOfClass:NSXPCListenerEndpoint.class])
    {
        NSLog(@"The endpoint decoded as %@, not an NSXPCListenerEndpoint",
              [wrapper class]);
        wrapper = nil;
    }
    
    xpc_endpoint_t endpoint = MLXUnwrapEndpoint(wrapper);
    
    if (!endpoint)
    {
        NSLog(@"Started, but the request carried no usable endpoint (userInfo keys: %@)", info.allKeys);
        return;
    }
    
    _channel = xpc_connection_create_from_endpoint(endpoint);
    
    xpc_connection_set_event_handler(_channel, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_ERROR)
        {
            NSLog(@"Channel error, exiting: %s",
                  xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION));
            exit(0);
        }
        
        MLXHandleMessage(self->_channel, event);
    });
    
    xpc_connection_resume(_channel);
    
    
    NSLog(@"Sending host handshake, pid %d", getpid());
    
    xpc_object_t hello = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(hello, MLXDonorKeyOp, "hello");
    xpc_connection_send_message(_channel, hello);
    
    NSLog(@"Ready, pid %d", getpid());
}

@end

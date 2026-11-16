#include <xpc/xpc.h>

static inline xpc_object_t _Nullable mlx_create_shared_memory(void * _Nonnull address, size_t size)
{
    return xpc_shmem_create(address, size);
}

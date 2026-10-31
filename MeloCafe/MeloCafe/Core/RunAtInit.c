//
//  RunAtInit.c
//  MeloCafe
//
//  Created by Stossy11 on 16/4/2026.
//

#include <mach/mach.h>
#include <stdio.h>
#include <os/proc.h>

size_t g_available_memory = 0;

__attribute__((constructor))
static void set_available_memory(void) {
    g_available_memory =  os_proc_available_memory();
    printf("Available memory: %zu bytes\n", g_available_memory);
}

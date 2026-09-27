#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>

// NSExtensionItem's decoder does not normally permit an anonymous XPC endpoint.
// Permit only that additional class and retain validation for every other class.
static void (*originalValidate)(id, SEL, Class, id, BOOL);
static void ValidateJITEndpoint(id decoder, SEL selector, Class cls, id key, BOOL invocations)
{
    if ([cls isSubclassOfClass:NSXPCListenerEndpoint.class])
        return;
    originalValidate(decoder, selector, cls, key, invocations);
}

__attribute__((used, visibility("default")))
int NSExtensionMain(int argc, char **argv)
{
    SEL selector = NSSelectorFromString(@"_validateAllowedClass:forKey:allowingInvocations:");
    Method method = class_getInstanceMethod(NSClassFromString(@"NSXPCDecoder"), selector);
    if (method)
        originalValidate = (void *)method_setImplementation(method, (IMP)ValidateJITEndpoint);
    int (*entry)(int, char **) = dlsym(RTLD_NEXT, "NSExtensionMain");
    return entry ? entry(argc, argv) : 1;
}

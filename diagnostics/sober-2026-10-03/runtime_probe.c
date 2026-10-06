#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(XrResult result, const char *name)
{
    printf("%s result=%d\n", name, result);
    if (XR_FAILED(result)) exit(1);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(library, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) return 1;
    XrNegotiateLoaderInfo loader = {
        .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
        .structVersion = XR_LOADER_INFO_STRUCT_VERSION,
        .structSize = sizeof(loader), .minInterfaceVersion = 1, .maxInterfaceVersion = 1,
        .minApiVersion = XR_MAKE_VERSION(1, 0, 0), .maxApiVersion = XR_MAKE_VERSION(1, 1, 0),
    };
    XrNegotiateRuntimeRequest runtime = {
        .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
        .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION, .structSize = sizeof(runtime),
    };
    check(negotiate(&loader, &runtime), "negotiate");
    PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
    check(runtime.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                     (PFN_xrVoidFunction *)&enumerate), "resolve enumerate");
    uint32_t count = 0;
    check(enumerate(NULL, 0, &count, NULL), "extension count");
    XrExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (!properties) return 1;
    for (uint32_t i = 0; i < count; ++i) properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    check(enumerate(NULL, count, &count, properties), "extensions");
    for (uint32_t i = 0; i < count; ++i) printf("extension=%s\n", properties[i].extensionName);
    free(properties);
    PFN_xrCreateInstance create = NULL;
    check(runtime.getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create), "resolve create");
    XrInstanceCreateInfo info = {.type = XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(info.applicationInfo.applicationName, "sober-wivrn-diagnostic-control");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance instance = XR_NULL_HANDLE;
    check(create(&info, &instance), "xrCreateInstance");
    PFN_xrGetSystem get_system = NULL;
    PFN_xrGetSystemProperties get_properties = NULL;
    PFN_xrDestroyInstance destroy = NULL;
    check(runtime.getInstanceProcAddr(instance, "xrGetSystem", (PFN_xrVoidFunction *)&get_system), "resolve system");
    check(runtime.getInstanceProcAddr(instance, "xrGetSystemProperties", (PFN_xrVoidFunction *)&get_properties), "resolve properties");
    check(runtime.getInstanceProcAddr(instance, "xrDestroyInstance", (PFN_xrVoidFunction *)&destroy), "resolve destroy");
    XrSystemGetInfo system_info = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system = XR_NULL_SYSTEM_ID;
    check(get_system(instance, &system_info, &system), "xrGetSystem");
    XrSystemProperties system_properties = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    check(get_properties(instance, system, &system_properties), "xrGetSystemProperties");
    printf("system=%s orientationTracking=%u positionTracking=%u\n", system_properties.systemName,
           system_properties.trackingProperties.orientationTracking, system_properties.trackingProperties.positionTracking);
    check(destroy(instance), "xrDestroyInstance");
    dlclose(library);
    return 0;
}

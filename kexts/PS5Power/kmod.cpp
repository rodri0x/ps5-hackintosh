// The kmod_info record every kext needs; Xcode normally generates this file.
#include <mach/mach_types.h>

extern "C" {
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(local.ps5.driver.PS5Power, "0.1", _start, _stop)
__attribute__((visibility("default"))) kmod_start_func_t *_realmain = 0;
__attribute__((visibility("default"))) kmod_stop_func_t *_antimain = 0;
__attribute__((visibility("default"))) int _kext_apple_cc = __APPLE_CC__;
}

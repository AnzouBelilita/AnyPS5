#include "prx/libc/include/general/VabiMacros.hpp"
#include <mutex>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {
std::mutex loaderMutex;
}

extern "C" void AgcDriverLockVulkanLoader_nid_postfix() {
    loaderMutex.lock();
}

extern "C" void AgcDriverUnlockVulkanLoader_nid_postfix() {
    loaderMutex.unlock();
}

// The standalone visual test needs only the AGC PM4 global-data-share allocation;
// it does not load the guest libkernel PRX. Provide a host-native anonymous mapping.
extern "C" void* APS5_VABI mmap_nid_postfix(void* address, std::size_t length, int protection,
                                            int flags, int descriptor, std::int64_t offset) noexcept {
#ifdef _WIN32
    (void)address;
    (void)protection;
    (void)flags;
    (void)descriptor;
    (void)offset;
    return VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
#else
    return ::mmap(address, length, protection, flags, descriptor, static_cast<off_t>(offset));
#endif
}

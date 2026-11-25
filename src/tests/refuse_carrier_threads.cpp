// LD_PRELOAD fault injection: refuse pthread_create after N successful calls.
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>

namespace {
std::atomic<unsigned> attempts{0};
std::atomic<unsigned> refused{0};
__attribute__((destructor)) void report() {
    std::fprintf(stderr, "thread attempts=%u refused=%u\n", attempts.load(), refused.load());
}
}

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                               void* (*start)(void*), void* argument) noexcept {
    const char* setting = std::getenv("JDVRIF_TEST_THREADS_ALLOWED");
    const unsigned allowed = setting ? static_cast<unsigned>(std::strtoul(setting, nullptr, 10)) : 0;
    if (attempts.fetch_add(1) >= allowed) {
        ++refused;
        return EAGAIN;
    }
    const auto real_create = reinterpret_cast<decltype(&pthread_create)>(dlsym(RTLD_NEXT, "pthread_create"));
    return real_create(thread, attributes, start, argument);
}

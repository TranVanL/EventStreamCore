#pragma once

#define ESC_PLATFORM_ID_UNKNOWN 0
#define ESC_PLATFORM_ID_LINUX   1
#define ESC_PLATFORM_ID_QNX     2

#if defined(__QNXNTO__) || defined(__QNX__)

    #define ESC_PLATFORM_ID ESC_PLATFORM_ID_QNX

#elif defined(__linux__)

    #define ESC_PLATFORM_ID ESC_PLATFORM_ID_LINUX

#else

    #define ESC_PLATFORM_ID ESC_PLATFORM_ID_UNKNOWN
    #error "EventStreamCore: unsupported operating system"

#endif

#if ESC_PLATFORM_ID == ESC_PLATFORM_ID_LINUX

    #define ESC_PLATFORM_LINUX 1
    #define ESC_PLATFORM_QNX    0

    #define ESC_HAS_PTHREAD             1
    #define ESC_HAS_POSIX_SEMAPHORE     1
    #define ESC_HAS_MONOTONIC_CLOCK     1
    #define ESC_HAS_TIMERFD              1
    #define ESC_HAS_POSIX_MESSAGE_QUEUE 1
    #define ESC_HAS_QNX_CHANNEL         0

#elif ESC_PLATFORM_ID == ESC_PLATFORM_ID_QNX

    #define ESC_PLATFORM_LINUX 0
    #define ESC_PLATFORM_QNX    1

    #define ESC_HAS_PTHREAD             1
    #define ESC_HAS_POSIX_SEMAPHORE     1
    #define ESC_HAS_MONOTONIC_CLOCK     1
    #define ESC_HAS_TIMERFD              0
    #define ESC_HAS_POSIX_MESSAGE_QUEUE 0
    #define ESC_HAS_QNX_CHANNEL         1

#else

    #define ESC_PLATFORM_LINUX 0
    #define ESC_PLATFORM_QNX    0

    #define ESC_HAS_PTHREAD             0
    #define ESC_HAS_POSIX_SEMAPHORE     0
    #define ESC_HAS_MONOTONIC_CLOCK     0
    #define ESC_HAS_TIMERFD              0
    #define ESC_HAS_POSIX_MESSAGE_QUEUE 0
    #define ESC_HAS_QNX_CHANNEL         0

#endif

/* Operating-system and CPU detection are deliberately independent. */
#if defined(__x86_64__) || defined(_M_X64)

    #define ESC_ARCH_X86_64  1
    #define ESC_ARCH_AARCH64 0

#elif defined(__aarch64__) || defined(_M_ARM64)

    #define ESC_ARCH_X86_64  0
    #define ESC_ARCH_AARCH64 1

#else

    #define ESC_ARCH_X86_64  0
    #define ESC_ARCH_AARCH64 0

#endif

namespace eventstream::platform {

enum class PlatformId : unsigned char {
    Unknown = ESC_PLATFORM_ID_UNKNOWN,
    Linux = ESC_PLATFORM_ID_LINUX,
    QNX = ESC_PLATFORM_ID_QNX
};

#if ESC_PLATFORM_ID == ESC_PLATFORM_ID_LINUX

inline constexpr PlatformId currentPlatformId = PlatformId::Linux;

#elif ESC_PLATFORM_ID == ESC_PLATFORM_ID_QNX

inline constexpr PlatformId currentPlatformId = PlatformId::QNX;

#else

inline constexpr PlatformId currentPlatformId = PlatformId::Unknown;

#endif

} // namespace eventstream::platform

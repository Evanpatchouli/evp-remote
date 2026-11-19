#pragma once

#include <winsock2.h>
#include <webrtc_c.h>

#include <mutex>

namespace evp::rtc {

inline std::mutex runtimeMutex;
inline unsigned runtimeUsers = 0;
inline std::once_flag sslInitialized;

inline bool acquireRuntime() {
    const std::scoped_lock lock(runtimeMutex);
    if (runtimeUsers == 0) {
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return false;
        std::call_once(sslInitialized, webrtc_InitializeSSL);
    }
    ++runtimeUsers;
    return true;
}

inline void releaseRuntime() {
    const std::scoped_lock lock(runtimeMutex);
    if (--runtimeUsers == 0) WSACleanup();
}

} // namespace evp::rtc

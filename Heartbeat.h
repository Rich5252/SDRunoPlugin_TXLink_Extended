// Heartbeat.h
#pragma once
#include <atomic>
#include <chrono>
#include <thread>

#include "MessageBox.h"     //for get_millis()



inline std::atomic<int64_t> g_formHeartbeatMs{ 0 };

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void Watchdog_Loop(std::atomic<bool>& exitRequest);

// Add near the top of SDRunoPlugin_TXLinkForm.cpp, or in a shared header
#define TIME_STEP(label, call) \
    do { \
        auto __t0 = std::chrono::steady_clock::now(); \
        call; \
        auto __t1 = std::chrono::steady_clock::now(); \
        auto __ms = std::chrono::duration_cast<std::chrono::milliseconds>(__t1 - __t0).count(); \
        if (__ms > 50) { \
            std::cout << get_millis() << " OnTimer SLOW STEP [" << label << "] took " << __ms << "ms" << std::endl; \
        } \
    } while (0)

#define TRY_STEP(label, call) \
    do { \
        try { \
            call; \
        } catch (const std::exception& e) { \
            std::cout << get_millis() << " *** EXCEPTION in [" << label << "]: " << e.what() << std::endl; \
        } catch (...) { \
            std::cout << get_millis() << " *** UNKNOWN EXCEPTION in [" << label << "]" << std::endl; \
        } \
    } while (0)
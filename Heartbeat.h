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


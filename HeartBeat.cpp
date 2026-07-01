#include "Heartbeat.h"


void Watchdog_Loop(std::atomic<bool>& exitRequest)
{
    const int64_t expectedTickMs = 100;       // match your timer interval
    const int64_t stallThresholdMs = 1000;    // flag if gap exceeds this
    const int64_t checkIntervalMs = 2000;

    int64_t lastSeenHeartbeat = g_formHeartbeatMs.load();
    bool currentlyStalled = false;

	bool bFirstHeartBeat = true;

    while (!exitRequest)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(checkIntervalMs));

        int64_t currentHeartbeat = g_formHeartbeatMs.load();
        int64_t nowMs = now_ms();
        int64_t ageMs = nowMs - currentHeartbeat;
        std::string SinceStart = "";
		if (!currentlyStalled) SinceStart = get_millis();

        if (bFirstHeartBeat)
        {
            bFirstHeartBeat = false;
            std::cout << get_millis() << " WATCHDOG: heartbeat loop started. " << std::endl;
        }

        if (ageMs > stallThresholdMs)
        {
            if (!currentlyStalled)
            {
                std::cout << get_millis() << " WATCHDOG: Form OnTimer appears STALLED. "
                    << "Last heartbeat " << ageMs << "ms ago. At " << SinceStart << std::endl;
                currentlyStalled = true;
            }
            else
            {
                std::cout << get_millis() << " WATCHDOG: Form OnTimer STILL stalled. "
                    << "Last heartbeat " << ageMs << "ms ago. At " << SinceStart << std::endl;
            }
        }
        else if (currentlyStalled)
        {
            std::cout << get_millis() << " WATCHDOG: Form OnTimer RECOVERED after stall." << std::endl;
            currentlyStalled = false;
        }

        lastSeenHeartbeat = currentHeartbeat;
    }

    std::cout << get_millis() << " WATCHDOG: thread exiting." << std::endl;
}
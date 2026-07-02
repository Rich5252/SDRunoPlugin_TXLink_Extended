#pragma once

void SyncClockFromInternet(const char* ntpServer, int offsetMs);

bool SetSystemTimeWithOffset(time_t utcTime, int offsetMs);

std::string FormatUtcTime(uint64_t timeMs);
uint64_t GetCurrentTimeMs();

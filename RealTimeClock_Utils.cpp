#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <time.h>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

// Helper to set the Windows System Clock
bool SetSystemTimeWithOffset(time_t utcTime, int offsetMs) {
    // Apply the user-defined offset (SDR Latency compensation)
    utcTime += (offsetMs / 1000);
    int remainingMs = offsetMs % 1000;

    tm gmt;
    gmtime_s(&gmt, &utcTime);

    SYSTEMTIME st;
    st.wYear = gmt.tm_year + 1900;
    st.wMonth = gmt.tm_mon + 1;
    st.wDay = gmt.tm_mday;
    st.wHour = gmt.tm_hour;
    st.wMinute = gmt.tm_min;
    st.wSecond = gmt.tm_sec;
    st.wMilliseconds = (WORD)remainingMs;

    // SetSystemTime requires SE_SYSTEMTIME_NAME privilege
    if (SetSystemTime(&st)) {
        return true;
    }
    else {
        std::cerr << "Failed to set clock. Ensure app is running as Administrator. Error: " << GetLastError() << std::endl;
        return false;
    }
}

// Main function to sync with NTP server (e.g., pool.ntp.org)
void SyncClockFromInternet(const char* ntpServer, int offsetMs) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    struct addrinfo hints = { 0 }, * result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(ntpServer, "123", &hints, &result) != 0) return;

    // NTP Packet Buffer (48 bytes)
    unsigned char packet[48] = { 0 };
    packet[0] = 0x23; // LI = 0, VN = 4, Mode = 3 (Client)

    if (sendto(sock, (char*)packet, sizeof(packet), 0, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) return;

    // Wait for response
    int addrLen = (int)result->ai_addrlen;
    if (recvfrom(sock, (char*)packet, sizeof(packet), 0, result->ai_addr, &addrLen) != SOCKET_ERROR) {
        // NTP timestamp is in bytes 40-43 (seconds since 1900)
        unsigned long highWord = htonl(*(unsigned long*)&packet[40]);
        // Convert to Unix Epoch (Seconds since 1970)
        time_t txTime = (time_t)(highWord - 2208988800UL);

        std::cout << "NTP Server Time Captured. Applying " << offsetMs << "ms offset..." << std::endl;
        if (SetSystemTimeWithOffset(txTime, offsetMs)) {
            std::cout << "System Clock Updated Successfully!" << std::endl;
        }
    }

    freeaddrinfo(result);
    closesocket(sock);
    WSACleanup();
}




// Returns current UTC time in milliseconds since Unix Epoch
uint64_t GetCurrentTimeMs() {
    FILETIME ft;
    // High-resolution UTC time (sub-microsecond precision)
    GetSystemTimePreciseAsFileTime(&ft);

    // Convert FILETIME (100-nanosecond intervals since 1601) to 64-bit integer
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;

    // Shift epoch from 1601 to 1970 (difference is 11,644,473,600 seconds)
    const uint64_t UNIX_EPOCH_OFFSET_100NS = 116444736000000000ULL;
    uint64_t unixTime100ns = ull.QuadPart - UNIX_EPOCH_OFFSET_100NS;

    // Convert 100ns units to milliseconds
    return unixTime100ns / 10000;
}

// Usage Example
void DebugCurrentTime() {
    uint64_t now = GetCurrentTimeMs();
    std::cout << "Current UTC (ms): " << now << std::endl;
}


std::string FormatUtcTime(uint64_t timeMs) {
    // Separate seconds and milliseconds
    time_t seconds = (time_t)(timeMs / 1000);
    int milliseconds = (int)(timeMs % 1000);

    // Convert to UTC structure
    struct tm gmt;
    gmtime_s(&gmt, &seconds);

    // Format using stringstream
    std::stringstream ss;
    ss << std::setfill('0')
        << std::setw(2) << gmt.tm_hour << ":"
        << std::setw(2) << gmt.tm_min << ":"
        << std::setw(2) << gmt.tm_sec << "."
        << std::setfill('0') << std::setw(3) << milliseconds;

    return ss.str();
}

// Example usage in your TxLink loop:
// std::string displayTime = FormatUtcTime(GetCurrentTimeMs());
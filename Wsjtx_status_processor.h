#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "UDPStatusEvent.h"
#include "MessageQueue.h"

// Forward declarations - full definitions only needed in the .cpp
struct WSJTX_Status;

//---------------------------------------------------------------------------
// WSJTX_StatusProcessor
//
// Owns and processes WSJT-X status packets (PacketType == 1) and the
// associated PreemptiveTX logic. Previously this logic lived as a mix of
// file-scope statics and free functions in UDP_Server.cpp; making it a class
// gives the shared working state a clean, explicit home and makes it easy
// to extend with new functionality.
//
// One instance is created as a static local in UDP_Server.cpp and lives for
// the duration of the UDP thread - same lifetime semantics as the old statics,
// just properly encapsulated.
//
// Threading: all methods are called only from the UDP server thread.
// The UDPStatusQueue passed to ProcessStatus/PreemptiveTXCheck is the
// thread-safe handoff point to the Form thread.
//---------------------------------------------------------------------------
class WSJTX_StatusProcessor
{
public:
    WSJTX_StatusProcessor() = default;

    // Called from ProcessPacket when PacketType == 1.
    // Decodes the status packet, builds and pushes a complete UDPStatusEvent,
    // then calls PreemptiveTXCheck if appropriate.
    // Returns a log-friendly string describing what happened.
    std::string ProcessStatus(const std::vector<uint8_t>& pkt,
        size_t index,
        ThreadSafeMessageQueue<UDPStatusEvent>& queue,
        int rtcDelay);

    // Called from the UDP receive-timeout branch (between packets) and from
    // ProcessStatus (after building the status event) to check whether a
    // preemptive TX start is warranted.
    void PreemptiveTXCheck(ThreadSafeMessageQueue<UDPStatusEvent>& queue,
        int rtcDelay);

    // Read-only accessors used by UDP_Server_Loop for diagnostic output
    const std::string& GetTXmodeStr() const { return WSJTX_TXmodeStr; }

private:
    // -----------------------------------------------------------------------
    // UDP-thread-local working/decision state.
    // These never cross threads - they exist purely to track context between
    // successive status packets and to build each outgoing UDPStatusEvent.
    // -----------------------------------------------------------------------

    // Current mode and decode message (written as side-effect by EncodeMessage)
    std::string WSJTX_TXmodeStr = "FT8";
    std::string strDecodeMsg = "NOCALL";

    // TX message carry-forward, for detecting changes
    std::string LastTXmsg = "";
    bool        TXmodeChange = true;    // true on first call, set true on mode change

    // Frequency carry-forward, for detecting changes
    uint64_t curFrequency = 0;

    // TX-slot decision state
    bool msgChanged = false;
    bool TXeven = true;
    bool TXenabled = false;

    // Preemptive TX state
    bool     TXpreempted = false;
    int      TXpreemptedStartSlot = 0;
    uint64_t PreemptiveTXstartMs = 0;

    // Most recently pushed FULL UDPStatusEvent.
    // PreemptiveTXCheck() copies this as its base rather than constructing a
    // blank event, so it never pushes a partially-populated snapshot that could
    // overwrite the Form's m_lastUDPStatus with empty dxCall/report/etc.
    // One-shot fields (newDXcall, haltTXevent, dataChanged, newFrequency,
    // txModeChanged) are cleared on this copy immediately after each push.
    UDPStatusEvent lastFullEvent;

    // Carry-forward "last known value" state.
    // These persist across packets so the Form always receives a complete
    // snapshot even when a given field didn't change this packet.
    bool        TXflgLocal = false;
    int         TXDFLocal = 0;
    std::string TXsymbolsLocal = "3140652002457262353553516142250015633140652505007562410457700470324420543140652";
    std::string DXcallLocal = "";
    std::string DXgridLocal = "";
    std::string strReportLocal = "";
    uint32_t    RxDFLocal = 0;
};

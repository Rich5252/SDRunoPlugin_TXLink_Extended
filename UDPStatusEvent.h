#pragma once

#include <string>
#include <cstdint>

//---------------------------------------------------------------------------
// UDPStatusEvent - single combined "everything ProcessUDPEvent and the DX-info
// button handlers need from one WSJT-X UDP status datagram" snapshot.
//
// Replaces the old pattern of: UDPevent flag + ~13 independent extern globals
// written by ProcessPacket()/PreemptiveTXCheck() on the UDP thread and read
// directly (unsynchronised, or only partially synchronised) by the Form
// thread. All fields below describe ONE datagram's worth of state and are
// always pushed together, so a consumer never sees a mix of old/new values.
//
// IMPORTANT: This struct lives in its own header, deliberately with NO other
// includes beyond <string>/<cstdint>, so both UDP_Server.h and
// SDRunoPlugin_TXLinkForm.h can include it directly without re-creating the
// circular dependency between them (UDP_Server.h -> SDRunoPlugin_TXLink.h ->
// SDRunoPlugin_TXLinkForm.h -> UDP_Server.h). Do not add any project-specific
// includes to this file.
//---------------------------------------------------------------------------
struct UDPStatusEvent
{
    // --- Consumed by SDRunoPlugin_TXLinkForm::ProcessUDPEvent() (TX control path) ---
    uint64_t    newFrequency = 0;      // non-zero if WSJT-X reported a band/freq change
    bool        dataChanged = false;   // true if any TX-relevant data changed this packet
    int         txDF = 0;              // TX delta-freq offset
    std::string txSymbols;             // FT8/FT4/FT2 tone symbols to transmit
    bool        txFlag = false;        // WSJT-X transmitting on/off
    std::string txModeStr;             // "FT8" / "FT4" / "FT2"
    bool        haltTXevent = false;   // force immediate TX halt
    bool        txModeChanged = false; // true if WSJT-X mode (FT8/FT4/FT2) changed since the last pushed event
    std::string newDXcall;             // non-empty if this is a new DX call (triggers QRZ lookup)
    std::string decodeMsg;             // strDecodeMsg - current decode/status text for display

    // --- Consumed by Form button-action handlers (DX info display) ---
    std::string dxCall;                // current DX call being worked
    std::string dxGrid;                // current DX grid square
    std::string report;                // signal report to send
    uint32_t    rxDF = 0;              // receive delta-freq offset
};
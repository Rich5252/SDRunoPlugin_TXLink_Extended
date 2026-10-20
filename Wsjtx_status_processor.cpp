#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

#include "WSJTX_Status_Processor.h"
#include "WSJTX_Packet.h"
#include "UDP_Server.h"         // LPWSTRGuard, EncodeMessage, removeTrailingSpaces
#include "RealTimeClock_Utils.h"
#include "MessageBox.h"         // get_millis()
#include "QRZ_XML_Lookup.h"
#include "JTTY_UDP.h"

using namespace std;

//-----------------------------------------------------------------------------
// WSJTX_StatusProcessor::ProcessStatus
//
// Handles PacketType == 1 (Status) from WSJT-X.
// Builds a complete UDPStatusEvent from the packet and pushes it to the queue.
// Calls PreemptiveTXCheck afterwards if no halt was signalled.
//-----------------------------------------------------------------------------
std::string WSJTX_StatusProcessor::ProcessStatus(
    const std::vector<uint8_t>& pkt,
    size_t index,
    ThreadSafeMessageQueue<UDPStatusEvent>& queue,
    int rtcDelay)
{
    WSJTX_Status StatusPacket(pkt, index);
    StatusPacket.Decode();

    // JTTY has no T/R slots and WS supplies the tones directly in txSymbols,
    // so EncodeMessage, the slot/preemptive logic and the halt rule are skipped.
    const bool isJtty = (StatusPacket.Mode == "JTTY");

    std::string msg = StatusPacket.DefaultTXMessage;
    msg = removeTrailingSpaces(msg);

    std::cout << "\n" << get_millis()
        << "NewStatusPacket: DefaultTXMessage = \""
        << StatusPacket.DefaultTXMessage << "\"" << std::endl;
    std::cout <<  get_millis()
        << "NewStatusPacket: Mode:" << StatusPacket.Mode << ", txSymbols = \""
        << StatusPacket.txSymbols << "\"" << std::endl;

    if (msg.empty()) { msg = "CQ G4AHN IO91"; }   // default if empty

    // Build the outgoing event locally - nothing shared is touched until
    // the single queue.push() at the end. No flag, no wait-loop, no possibility
    // of two threads seeing a half-written snapshot.
    UDPStatusEvent evt;

    // --- Frequency change detection ---
    bool dataChanged = false;
    uint64_t newFrequency = 0;
    if (StatusPacket.Frequency != curFrequency)
    {
        newFrequency = StatusPacket.Frequency;
        curFrequency = newFrequency;
        dataChanged = true;
    }

    // --- Mode change detection ---
    if (WSJTX_TXmodeStr != StatusPacket.Mode) { TXmodeChange = true; dataChanged = true; }
    WSJTX_TXmodeStr = StatusPacket.Mode;

    // --- TX flag and DF ---
    bool haltTXevent = false;
    if (!isJtty && TXflgLocal && !StatusPacket.Transmitting && !StatusPacket.TxEnabled) { haltTXevent = true; }
    bool txFlag = StatusPacket.Transmitting;
    if (TXDFLocal != StatusPacket.TxDF) { dataChanged = true; }
    TXDFLocal = StatusPacket.TxDF;

    // --- Message/symbol encoding ---
    msgChanged = false;
    std::string txSymbols = TXsymbolsLocal;   // carry forward unless changed
    std::string jttyCmd;
    if (isJtty)
    {
        // Tune: WS sends Transmitting with Tx message "TUNE" and no symbols.
        // Start a steady carrier on the Nano ({JT}) and stop it ({X0}) when
        // WS stops transmitting (Tune button off, Halt Tx, or WS tune timeout).
        bool tuneNow = StatusPacket.Transmitting && msg == "TUNE";
        if (tuneNow != jttyTuning)
        {
            jttyTuning = tuneNow;
            jttyCmd = tuneNow ? "{JT}{X1}" : "{X0}";
            if (tuneNow) { strDecodeMsg = "TUNE"; }
            lastJttyId = 0;
            std::cout << get_millis() << "JTTY: Tune " << (tuneNow ? "ON -> Nano {JT}{X1}" : "OFF -> Nano {X0}") << std::endl;
        }
        else if (!tuneNow)
        {
            jttyCmd = ProcessJttyTxSymbols(StatusPacket.txSymbols, StatusPacket.Transmitting, msg);
        }
    }
    else if (LastTXmsg.empty() || LastTXmsg != msg)
    {
        EncodeResult encoded = EncodeMessage(WSJTX_TXmodeStr, msg);
        txSymbols = encoded.symbols;
        strDecodeMsg = encoded.decodeMsg;   // now properly stored on this class member
        dataChanged = true;
        msgChanged = true;
    }

    if (!isJtty)                    // keep the FT8/FT4/FT2 message state untouched while in JTTY
    {
        LastTXmsg = msg;
        TXsymbolsLocal = txSymbols;
    }
    TXflgLocal = txFlag;

    // --- DX call / grid / report ---
    std::string newDXcall;
    if (StatusPacket.DXCall != DXcallLocal) { newDXcall = StatusPacket.DXCall; }
    DXcallLocal = StatusPacket.DXCall;
    DXgridLocal = StatusPacket.DXgrid;
    strReportLocal = StatusPacket.Report;
    RxDFLocal = StatusPacket.RxDF;

    // --- Preemptive TX slot tracking ---
    uint64_t ms = GetCurrentTimeMs();
    int slotPeriod = WSJTX_TXmodeStr == "FT2" ? 3750
        : WSJTX_TXmodeStr == "FT4" ? 7500 : 15000;
    int nSlot = static_cast<int>((ms % 60000) / slotPeriod);
    bool IsSlotEven = ((ms / slotPeriod) % 2) == 0;
    int DTC = static_cast<int>(ms % slotPeriod);
    int SlotTimeLeft = slotPeriod - DTC;

    TXenabled = isJtty ? false : StatusPacket.TxEnabled;     // no slot logic for JTTY
    if (TXenabled)
    {
        // Abort preemptive TX if it started in the wrong slot.
        // Ignore late FT2 end-tx packets that arrive close to slot boundary.
        if (TXpreempted
            && nSlot != TXpreemptedStartSlot
            && (ms - PreemptiveTXstartMs) < 1000
            && !StatusPacket.Transmitting
            && TXeven == IsSlotEven)
        {
            haltTXevent = true;
            TXpreempted = false;
            std::string str1 = get_millis() + "Preemptive TX Halt. DT: "
                + to_string(DTC) + "  trPeriod = "
                + to_string(StatusPacket.trPeriod) + "\r\n";
            std::cout << str1;
        }

        // Assess which is the correct TX slot.
        bool even = (ms / slotPeriod) % 2 == 0;
        if (StatusPacket.Transmitting) TXeven = even;
        if (nSlot != TXpreemptedStartSlot
            && !msgChanged
            && !StatusPacket.Transmitting
            && TXenabled
            && SlotTimeLeft > 3000)
        {
            TXeven = !even;
        }
    }
    if (StatusPacket.Transmitting) TXpreempted = false;

    // --- Diagnostic log ---
    std::cout << get_millis()
        << "UDPPush: "
        << (dataChanged ? "Data change. " : "Data nochange. ")
        << (msgChanged ? "Msg change. " : "Msg unchanged. ")
        << (TXenabled ? " TXenabled. " : " TXdisabled. ")
        << (TXeven ? "TXslot Even. " : "TXslot Odd. ")
        << (StatusPacket.Transmitting ? "TX On. " : "TX off. ")
        << "Slot: " << nSlot
        << " SlotTimeLeft: " << SlotTimeLeft
        << std::endl;

    // --- Populate and push the complete event ---
    // Single source of truth for this packet, consumed as one atomic unit
    // by the Form thread via UDPStatusQueue.
    evt.newFrequency = newFrequency;
    evt.dataChanged = dataChanged;
    evt.txDF = TXDFLocal;
    evt.txSymbols = TXsymbolsLocal;
    evt.txFlag = TXflgLocal;
    evt.txModeStr = WSJTX_TXmodeStr;
    evt.haltTXevent = haltTXevent;
    evt.txModeChanged = TXmodeChange;
    TXmodeChange = false;   // one-shot: consumed into this event
    evt.newDXcall = newDXcall;
    evt.decodeMsg = strDecodeMsg;
    evt.jttyCmd = jttyCmd;
    evt.dxCall = DXcallLocal;
    evt.dxGrid = DXgridLocal;
    evt.report = strReportLocal;
    evt.rxDF = RxDFLocal;

    lastFullEvent = evt;
    queue.push(evt);

	//keep copies for QRZ lookup below        
    std::string callsignCopy = newDXcall;
    std::string dxGridCopy = DXgridLocal;   // WSJT-X grid as fallback for QRZ validation

    // Clear one-shot fields from the carry-forward copy so PreemptiveTXCheck
    // doesn't re-signal events that already happened on this packet.
    lastFullEvent.newDXcall = "";
    lastFullEvent.haltTXevent = false;
    lastFullEvent.txModeChanged = false;
    lastFullEvent.dataChanged = false;
    lastFullEvent.newFrequency = 0;
    lastFullEvent.jttyCmd = "";

    // Call PreemptiveTXCheck AFTER the event is built and pushed, so it
    // starts from this packet's fully up-to-date lastFullEvent rather than
    // the previous packet's. The haltTXevent guard still correctly prevents
    // re-triggering a preemptive start in the same packet that just halted one.
    if (!haltTXevent) PreemptiveTXCheck(queue, rtcDelay);

    // QRZ lookup - detached thread so it never blocks the UDP loop.
    // Note: detached threads can outlive the DLL on unload - see known
    // limitations in the codebase notes. ShellExecute normally returns fast.
    if (!newDXcall.empty())
    {
        std::string strURL = "https://www.qrz.com/db/" + newDXcall;

        std::thread([strURL, callsignCopy, dxGridCopy]()
            {
                // Open the QRZ page in Chrome as before
                LPWSTRGuard op("open");
                LPWSTRGuard app("chrome.exe");
                LPWSTRGuard url(strURL.c_str());
                ShellExecute(NULL, op, app, url, NULL, SW_SHOWNORMAL);

                // Also do a QRZ XML grid lookup and copy to clipboard.
                // Static instance persists its session key across calls so we
                // re-authenticate only when the session actually expires.
                static QRZLookup qrz;
                qrz.LookupAndCopyGrid(callsignCopy, dxGridCopy);
            }).detach();
    }

    return "Status Update Pushed to Form.";
}

//-----------------------------------------------------------------------------
// WSJTX_StatusProcessor::PreemptiveTXCheck
//
// Called both from UDP_Server_Loop's timeout branch (between packets) and
// from ProcessStatus (after building/pushing the status event).
// Checks whether conditions are right to start TX one slot early, and if so
// pushes a preemptive UDPStatusEvent built from the last known full snapshot.
//-----------------------------------------------------------------------------
void WSJTX_StatusProcessor::PreemptiveTXCheck(
    ThreadSafeMessageQueue<UDPStatusEvent>& queue,
    int rtcDelay)
{
    if (WSJTX_TXmodeStr == "JTTY") { return; }   // JTTY has no T/R slots
    if (rtcDelay <= 50) { return; }   // minimal delay - let wsjtx control

    uint64_t ms = GetCurrentTimeMs();
    int slotPeriod = WSJTX_TXmodeStr == "FT2" ? 3750
        : WSJTX_TXmodeStr == "FT4" ? 7500 : 15000;
    int SlotNumber = static_cast<int>(ms % 60000) / slotPeriod;
    int TimeInSlot = static_cast<int>(ms % slotPeriod);
    int TimeToNextSlot = slotPeriod - TimeInSlot;
    bool inTxSlot = (((ms / slotPeriod) % 2) == 0) == TXeven;

    if (strDecodeMsg != "TUNE"
        && TXenabled
        && !inTxSlot
        && TimeToNextSlot < rtcDelay
        && !TXpreempted)
    {
        PreemptiveTXstartMs = GetCurrentTimeMs();
        TXpreempted = true;
        TXpreemptedStartSlot = SlotNumber;

        std::string str1 = TXeven ? " TX even. " : " TX odd. ";
        std::string str2 = "\r\n" + get_millis()
            + "Preemptive TX: " + FormatUtcTime(PreemptiveTXstartMs)
            + " Slot: " + to_string(SlotNumber)
            + " DT: " + to_string(PreemptiveTXstartMs % slotPeriod)
            + str1 + "RTCdelay: " + to_string(rtcDelay) + "\r\n";
        std::cout << str2;

        // Build from the last FULL snapshot - NOT a blank UDPStatusEvent.
        // See lastFullEvent comment in the header for the full explanation.
        UDPStatusEvent evt = lastFullEvent;
        evt.txFlag = true;
        evt.txModeStr = WSJTX_TXmodeStr;
        evt.decodeMsg = strDecodeMsg;
        queue.push(evt);
    }
}


//-----------------------------------------------------------------------------
// WSJTX_StatusProcessor::ProcessJttyTxSymbols
//
// JTTY: WS sends one Status per accepted message with txSymbols = "J<id> t t t..."
// (repeated by later Status packets), "JABORT" on abort, and "" otherwise.
// Returns the Nano command for this packet, or "" if nothing is to be sent.
//-----------------------------------------------------------------------------
std::string WSJTX_StatusProcessor::ProcessJttyTxSymbols(
    const std::string& txSymbols,
    bool transmitting,
    const std::string& msg)
{
    Jtty::TxSymbols t = Jtty::ParseTxSymbols(txSymbols);
    switch (t.kind)
    {
    case Jtty::Kind::Message:
        if (t.id == lastJttyId) { return ""; }          // repeat of a message already sent
        lastJttyId = t.id;
        strDecodeMsg = msg;
        msgChanged = true;
        std::cout << get_millis() << "JTTY: J" << t.id << " " << t.frames
            << " frame(s) -> Nano: " << msg << std::endl;
        return "{J" + t.hex + "}{X1}";                 // {X1} harmless if already transmitting

    case Jtty::Kind::Abort:
        lastJttyId = 0;
        std::cout << get_millis() << "JTTY: ABORT -> Nano {X0}" << std::endl;
        return "{X0}";

    case Jtty::Kind::Error:
        std::cout << get_millis() << "JTTY: txSymbols error: " << t.error << std::endl;
        return "";

    case Jtty::Kind::None:
    default:
        if (!transmitting) { lastJttyId = 0; }          // TX over (WS ids restart if WS restarts)
        return "";
    }
}

#pragma once

#include <Windows.h>
#include <string>
#include <atomic>
#include "MessageQueue.h"
#include "SDRunoPlugin_TXLink.h"

std::string ProcessPacket(char* message, int message_len);
void UDP_Server_Loop(IUnoPluginController& _controller, SDRunoPlugin_TXLink& _parent);


LPWSTR ConvertToLPWSTR(const char* charString);
// Small RAII helper to ensure mem is released when the function exits, even if an exception is thrown.
struct LPWSTRGuard {
    LPWSTR ptr;
    explicit LPWSTRGuard(const char* s) : ptr(ConvertToLPWSTR(s)) {}
    ~LPWSTRGuard() { delete[] ptr; }
    operator LPWSTR() const { return ptr; }
};


std::string EncodeMessage(const std::string& WSJTX_TXmodeStr, const std::string& strMessage);
std::string removeTrailingSpaces(std::string str);
void PreemptiveTXCheck();

#include "UDPStatusEvent.h"   // struct UDPStatusEvent - kept in its own dependency-free header
// to avoid a circular include with SDRunoPlugin_TXLinkForm.h

// Single queue carrying UDPStatusEvent snapshots from the UDP thread to the Form thread.
// Replaces: std::atomic<bool> UDPevent + UDPEvent_mtx + the ~13 globals below.
extern ThreadSafeMessageQueue<UDPStatusEvent> UDPStatusQueue;

//following are simple comms with the plugin
extern std::atomic <bool> UDPExitRequest;

extern int RTCdelay;			//actual preemptive advanced start for FT2 = RTCcurrentDelay - RTCdelayOffset
extern int RTCdelayOffset;		//local offset from Settings that reduces RTCcurrentDelay value
extern int RTCcurrentDelay;	//current delay in ms applied to RTC - updated from NamedPipe

extern SOCKET server_socket;
extern ThreadSafeMessageQueue<std::string> WSJTX_UDPInQ;			//data from wsjtx to plugin
extern ThreadSafeMessageQueue<uint64_t> WSJTX_UDPOutQ;				//data from plugin to wsjtx (SetFreq)

extern std::string strWSJTXpath;                                    //access to the symbol list calc apps
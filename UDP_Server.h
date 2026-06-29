#pragma once

#include <Windows.h>
#include <string>
#include <mutex>
#include <atomic>
#include "MessageBox.h"
#include "MessageQueue.h"
#include "SDRunoPlugin_TXLink.h"

#include "UDP_Type17.h"

std::string ProcessPacket(char * message, int message_len);
void UDP_Server_Loop(IUnoPluginController& _controller, SDRunoPlugin_TXLink& _parent);

// Small RAII helper to ensure mem is released when the function exits, even if an exception is thrown.
struct LPWSTRGuard {
    LPWSTR ptr;
    explicit LPWSTRGuard(const char* s) : ptr(ConvertToLPWSTR(s)) {}
    ~LPWSTRGuard() { delete[] ptr; }
    operator LPWSTR() const { return ptr; }
};
LPWSTR ConvertToLPWSTR(const char* charString);

std::string EncodeMessage(const std::string& WSJTX_TXmodeStr, const std::string& strMessage);
std::string removeTrailingSpaces(std::string str);
void PreemptiveTXCheck();

//following are simple comms with the plugin
extern std::atomic <bool> UDPExitRequest;
extern std::atomic <bool> UDPevent;				//new status update
extern std::mutex UDPEvent_mtx;					// Declare a mutex

extern bool UDPHaltTXevent;
extern std::string WSJTX_TXmodeStr;	//needed		//"FT8", "FT4", "FT2"
extern bool TXmodeChange;
extern bool TXflg;                  //needed
extern int TXDF;                    //needed
extern std::string TXsymbols;       //needed
extern std::string DXcall;          //needed
extern std::string DXgrid;          //needed
extern std::string newDXcall;       //needed
extern std::string strDecodeMsg;    //needed
extern std::string strWSJTXpath;
extern std::string strCallsign;
extern std::string strLocator;
extern uint64_t curTX
;		//set from form
extern uint64_t newFrequency;		//needed // non zero if it has just changed
extern bool dataChanged;			//needed //set when new data received from wsjtx to trigger form update
extern bool msgChanged;				//set when new message received from wsjtx to trigger form update
extern std::string strReport;       //needed
extern uint32_t RxDF;               //needed
extern int RTCdelay;			//actual preemptive advanced start for FT2 = RTCcurrentDelay - RTCdelayOffset
extern int RTCdelayOffset;		//local offset from Settings that reduces RTCcurrentDelay value
extern int RTCcurrentDelay;	//current delay in ms applied to RTC - updated from NamedPipe

extern SOCKET server_socket;
extern ThreadSafeMessageQueue<std::string> WSJTX_UDPInQ;			//data from wsjtx to plugin
extern ThreadSafeMessageQueue<uint64_t> WSJTX_UDPOutQ;				//data from plugin to wsjtx (SetFreq)
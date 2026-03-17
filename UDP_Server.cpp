#include <iostream>
#include <winsock2.h>
#include <vector>
#include <cstring>
#include <format>
#include <tuple>
#include <chrono>
#include <thread>
#include <windows.h>
#include <atomic>

#include <sstream>     // For std::stringstream (to build the hex string efficiently)
#include <iomanip>     // For std::hex, std::uppercase, std::setw, std::setfill
#include <cstdint>     // For uint8_t (optional, but good practice for clarity)

#include "WSJTX_Packet.h"
#include "UDP_Server.h"
#include "MessageQueue.h"
#include "RealTimeClock_Utils.h"



#define debugUDP

using namespace std;

#pragma comment(lib,"ws2_32.lib") // Winsock Library
#pragma warning(disable:4996) 

//UDP port defaults for wsjtx
#define BUFLEN 512
#define PORT 2237
#define SERVER "127.0.0.1"  // or "localhost" - ip address of UDP server

//externals that provide the simple comms to the plugin
std::atomic <bool> UDPExitRequest = false;
std::atomic <bool> UDPevent = false;          //set to indicate new status update
std::mutex UDPEvent_mtx;

ThreadSafeMessageQueue<std::string> WSJTX_UDPInQ;            //TODO data from wsjtx to plugin
ThreadSafeMessageQueue<uint64_t> WSJTX_UDPOutQ;           //data from plugin to wsjtx (SetFreq)

bool UDPHaltTXevent = false;    // force immediate TX halt
bool TXflg = false;
int TXDF = 0;
std::string WSJTX_TXmodeStr = "FT8";
std::string LastTXmsg = ""; 
bool TXmodeChange = true;
std::string DXcall = "";
std::string DXgrid = "";
std::string newDXcall = "";
std::string strReport = "";
uint32_t RxDF = 0;
std::string strDecodeMsg = "NOCALL";
uint64_t curFrequency = 0;                  //local store
uint64_t curTXFrequency = 0;                  //from form
uint64_t newFrequency = 0;                  // non zero ifchanged (extern)
bool dataChanged = false;                        //set when new data received from wsjtx to trigger form update
bool msgChanged = false;                         //set when new message received from wsjtx to trigger form update
bool TXeven = true;
bool TXenabled = false;
bool TXpreempted = false;           //Preemptive TX has started
uint64_t  PreemptiveTXstartMs = 0;  //  start time
int RTCdelay = 300;			//delay in ms currently applied to RTC
int RTCdelayOffset = 0;		//local offset from Settings that reduces RTCdelay value
int RTCcurrentDelay = 300;	//current delay in ms applied to RTC - updated from NamedPipe


std::string FT8TXsymbols = "3140652002457262353553516142250015633140652505007562410457700470324420543140652";       //"NOCALL"
std::string FT8TxNoSymbols = "0000000000000000000000000000000000000000000000000000000000000000000000000000000";
std::string strFT8WSJTXpath = "C:\\WSJT\\wsjtx\\bin\\ft8code.exe";         // must use double \\ (or /). This is emcapsulated in "" so can have spaces.
                                                                        // path is stored in SDRuno ini file (wsjtxPath) and can be modified manually if required.

std::string FT4TXsymbols = "001321033112330313210320122110210110232220103032312323100312203003023102222320131323332231310212123032010";     //"NOCALL"
std::string FT4TxNoSymbols = "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
std::string strFT4WSJTXpath = "C:\\WSJT\\wsjtx\\bin\\ft4code.exe";         // must use double \\ (or /). This is emcapsulated in "" so can have spaces.

std::string FT2TXsymbols = "001321033112330313210320122110210110232220103032312323100312203003023102222320131323332231310212123032010";     //"NOCALL"
std::string FT2TxNoSymbols = "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
std::string strFT2WSJTXpath = "C:\\WSJT\\wsjtx\\bin\\ft4code.exe";         // must use double \\ (or /). This is emcapsulated in "" so can have spaces.


std::string TXsymbols = "3140652002457262353553516142250015633140652505007562410457700470324420543140652";       //"NOCALL" default
std::string TxNoSymbols = FT8TxNoSymbols;                                           //default in case of error or unsupported modes
std::string strWSJTXpath = "C:\\WSJT\\wsjtx\\bin\\ft8code.exe";

std::string strCallsign = "G4AHN";
std::string strLocator = "IO91";

std::mutex m_lock2;

LPWSTR ConvertToLPWSTR(const char* charString);
SOCKET server_socket;

SDRunoPlugin_TXLink* m_parent = nullptr;

void dump_packet_hex(const std::vector<uint8_t>& packet) {
    std::stringstream ss;
    ss << std::hex << std::uppercase;

    for (size_t i = 0; i < packet.size(); ++i) {
        ss << std::setw(2) << std::setfill('0') << (int)packet[i] << " ";
        if ((i + 1) % 16 == 0) {
            ss << "\n";
        }
    }
    std::cout << "\n--- INCOMING HEX DUMP (" << packet.size() << " bytes) ---\n" << ss.str() << std::endl;
}

// *********  NamedPipeServer class to get RTCupdate app offset ms. *********
// Pipe name MUST match the VB6 client exactly!
#define PIPE_NAME "\\\\.\\pipe\\TXLinkCommandPipeUDP"
#define BUFSIZE 512
#define PipeTimeout 10		//time to wait for client to connect
#define ReadTimeout 10		//time to wait for read message

NamedPipeServer UDPpipe(PIPE_NAME, BUFSIZE);					//get messages from RTCupdater app


//wsjtx UDP datagram server with parser and ft8 encoding using wsjtx ft8code.exe utility
//this server is invoked into a seperate thread from the TXlink plugin for SDRuno app.
// status update to the plugin is via the simple extern data values above.
void UDP_Server_Loop(IUnoPluginController& _controller, SDRunoPlugin_TXLink& _parent)
{
	m_parent = &_parent;    // Set the global reference to the parent plugin instance

    sockaddr_in server{}, client{};

    // initialise winsock
    WSADATA wsa;
    std::cout << get_millis() << "Initialising Winsock..." << std::endl;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cout << get_millis() << "Failed. Error Code: " << WSAGetLastError() << std::endl;
        exit(0);
    }
    std::cout << get_millis() << "Initialised." << std::endl;

    // create a socket
  
    if ((server_socket = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
    {
        std::cout << get_millis() << "Could not create socket: " << WSAGetLastError() << std::endl;
    }
    std::cout << get_millis() << "Socket created." << std::endl;

    // prepare the sockaddr_in structure
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(SERVER);
    server.sin_port = htons(PORT);

    // bind
    if (::bind(server_socket, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
    {
        int errorCode = WSAGetLastError();
        std::cout << get_millis() << "Bind failed with error code: " << errorCode << std::endl;
        exit(EXIT_FAILURE);
    }
    std::cout << get_millis() << "Bind done." << std::endl;

    // Set a receive timeout (e.g., 500 milliseconds)
    // The timeout value is in milliseconds for SO_RCVTIMEO
    int timeout_ms = 50;
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms)) == SOCKET_ERROR)
    {
        std::cout << get_millis() << "setsockopt(SO_RCVTIMEO) failed with error code: " << WSAGetLastError() << std::endl;
        // Decide if you want to exit or log a warning and continue without timeout
    }

	UDPpipe.createAndListenAsync();     //init NamedPipeServer to listen for RTC delay updates

	int waitCount = 0;
    while (!UDPExitRequest)
    {
        //check NamedPipe connection and get any update of the offset value
		if (UDPpipe.pollConnection())
        {
            std::string msg;
            if (UDPpipe.pollData(msg))
            {
                //msg received
                try {
                    int newDelay = std::stoi(msg);
                    RTCcurrentDelay = static_cast<uint64_t>(newDelay);
                    
                    RTCdelay = RTCcurrentDelay - RTCdelayOffset;
                    RTCdelay < 50 ? RTCdelay = 50 : RTCdelay;
                    std::cout << std::endl;
                    std::cout << get_millis() << "Updated RTC current delay: " << RTCcurrentDelay << " ms. Preempt: " << RTCdelay << std::endl;
                }
                catch (const std::exception& e) {
                    std::cout << std::endl;
                    std::cerr << get_millis() << "Error parsing RTC delay from pipe message: " << msg << ". Exception: " << e.what() << std::endl;
				}
            }
        }

        char message[BUFLEN] = {};

        // try to receive some data, this is a blocking call
        int message_len;
        int slen = sizeof(sockaddr_in);

		// Use recvfrom with a timeout
        message_len = recvfrom(server_socket, message, BUFLEN, 0, (sockaddr*)&client, &slen);
        if (message_len == SOCKET_ERROR)
        {
            int lastError = WSAGetLastError();
            if (lastError == WSAETIMEDOUT)
            {
                // This is the expected timeout error, just continue the loop
                // to check UDPExitRequest. No message was received.

				if (waitCount == 0) { std::cout << std::endl << "UDP waiting : "; }
                //std::cout << " " << ++waitCount;
                if ((++waitCount % 10) == 0) { std::cout << " " << waitCount; }

				PreemptiveTXCheck();   // Check for preemptive TX conditions during wait

                continue; // Go back to the top of the loop to check UDPExitRequest
            }
            else
            {
                // A real error occurred (not just a timeout)
                std::cout << std::endl << "recvfrom() failed with error code: " << lastError << std::endl;
                // Consider more robust error handling than exit(0)
                // Maybe set UDPExitRequest = true to gracefully exit the loop
                // Or log the error and break; to ensure cleanup
                break; // Exit the loop on critical error
            }
        }
        else if (message_len == 0)
        {
            // Connection gracefully closed (less common for UDP, but good to handle)
            std::cout << std::endl << "recvfrom() returned 0 (connection closed)." << std::endl;
            break; // Exit the loop
        }
        else
        {
			if (waitCount > 0) { std::cout << std::endl; }
            // Message received successfully
            if (UDPExitRequest) {
                std::cout << get_millis() << "UDP Exit Request received, exiting server loop without processing received packet." << std::endl;
                break; // Exit the loop if exit request is set
			}

            waitCount = 0;                              // Reset wait count on successful receive
			std::string strPacketType = ProcessPacket(message, message_len);
			uint64_t ms = GetCurrentTimeMs();
            int slotPeriod = WSJTX_TXmodeStr == "FT2" ? 3750 : (WSJTX_TXmodeStr == "FT4" ? 7500 : 15000);
			string str1 = get_millis() + "UDP packet : " + strPacketType + " " + FormatUtcTime(ms) + " DTC: " + to_string(ms % slotPeriod) + "\r\n";
			std::cout <<  str1;
        
        }

	}                   //loop until UDPExitRequest is set

	// Cleanup
	std::cout << "\n" << get_millis() << "UDP Server cleanup..." << std::endl;
	UDPpipe.close();                // Ensure the NamedPipeServer is closed properly
    closesocket(server_socket);
    WSACleanup();
}

void PreemptiveTXCheck()
{

    uint64_t ms = GetCurrentTimeMs();
    int slotPeriod = WSJTX_TXmodeStr == "FT2" ? 3750 : (WSJTX_TXmodeStr == "FT4" ? 7500 : 15000);
	int SlotNumber = ms % 60000 / slotPeriod;                     //current slot number in minute (0-15)
	int TimeInSlot = ms % slotPeriod;
	int TimeToNextSlot = slotPeriod - TimeInSlot;
	bool inTxSlot = (((ms / slotPeriod) % 2) == 0) == TXeven;   //true if we are in the TX slot for the current even/odd setting

	// Preemptive TX start for FT2 - start in previous slot if enabled and not already preempted
    if ((WSJTX_TXmodeStr == "FT2" || WSJTX_TXmodeStr == "FT4") && strDecodeMsg != "TUNE" && TXenabled && !inTxSlot && TimeToNextSlot < (RTCdelay) && !TXpreempted)   //note we start TX in previous slot
    {                                   //expeimental - tx early cf PC RTC since PC clock delayed for SDR etc
        //should wait for any current UDP event to complete in Form
        int sleeps = 0;
        while (UDPevent && sleeps < 5 && !UDPExitRequest)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));         //wait for previous event to be processed
            sleeps++;
        }
        PreemptiveTXstartMs = GetCurrentTimeMs();
		string str1 = ( TXeven ? " TX even. " : " TX odd. " );
        string str2 = "\r\n" + get_millis() + "Preemptive TX: " + FormatUtcTime(PreemptiveTXstartMs) + " Slot: " + to_string(SlotNumber) + " DTC: " + to_string(PreemptiveTXstartMs % slotPeriod) + str1 + "RTCdelay: " + to_string(RTCdelay) + "\r\n";
        std::cout << str2;
        TXpreempted = true;      //indicates when Preempted TX has started. Reset when next packet received (checking correct slot)
        TXflg = true;            //force TX now
        UDPevent = true;         //flag for main UDP server code to update Arduino - avoid handshake race
    }
}

std::string ProcessPacket(char * message, int message_len)
{
    std::vector<uint8_t> pkt(message, message + message_len);
    size_t idx = 0;
    
    WSJTX_Packet NewPacket( pkt, idx);
    NewPacket.Decode();

    if (NewPacket.PacketType == 0) {
        return "Heartbeat ";
        WSJTX_Heartbeat HeartbeatPacket(pkt, NewPacket.index);
        HeartbeatPacket.Decode();
        // emit UI update signal for heartbeat if needed
    }
    else if (NewPacket.PacketType == 1) {
		std::string ret = "Status";

        //status packet contains next ft8 message and TX on/off updates
        WSJTX_Status StatusPacket( pkt, NewPacket.index);
        StatusPacket.Decode();

        //std::cout << "Status decode. Frequency = " << StatusPacket.Frequency << " Mode = " << StatusPacket.Mode << " TxEnabled = " << StatusPacket.TxEnabled << std::endl;

        std::string msg = StatusPacket.DefaultTXMessage;
        msg = removeTrailingSpaces(msg);
		if (msg == "") { msg = "CQ G4AHN IO91"; }                           //default message if empty

		
        if (UDPevent) { std::cout << "UDP_Server: ******* WARNING ************  UDP event already set\r\n"; }
		int sleeps = 0;
        while (UDPevent && sleeps < 50)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));         //wait for previous event to be processed
            sleeps++;
			if (UDPExitRequest) { return "UDP_Server: UDPExitRequest"; } // exit if requested
        } 
        if (sleeps > 0) { std::cout << get_millis() << " UDP event wait: " << sleeps << " times" << std::endl; }
       
        dataChanged = false;   //reset data changed flag - set to true if any data changes below
        if (StatusPacket.Frequency != curFrequency && newFrequency == 0)
        {
            newFrequency = StatusPacket.Frequency;
            curFrequency = newFrequency;                //reset to zero in form when UDPevent detected
			dataChanged = true;
        }
        if (WSJTX_TXmodeStr != StatusPacket.Mode) { TXmodeChange = true; dataChanged = true; }
        WSJTX_TXmodeStr = StatusPacket.Mode;
        if (TXflg && !StatusPacket.Transmitting && !StatusPacket.TxEnabled) { UDPHaltTXevent = true; }
		TXflg = StatusPacket.Transmitting;  //transmit on/off
		if (TXDF != StatusPacket.TxDF) { dataChanged = true; }
        TXDF = StatusPacket.TxDF;           //transmit freq offset added to set TX freq
		msgChanged = false;
        if (LastTXmsg == "" || LastTXmsg != msg)
        {
            TXsymbols = EncodeMessage(WSJTX_TXmodeStr, msg);         //list of tones to transmit
			dataChanged = true;
			msgChanged = true;
        }
        else
        {
            ret = ret + ": Msg unchanged: " + msg;
        }
		LastTXmsg = msg;                                                                        //store last message sent   
        if (StatusPacket.DXCall != DXcall) { newDXcall = StatusPacket.DXCall; }
        DXcall = StatusPacket.DXCall;
		DXgrid = StatusPacket.DXgrid;
		strReport = StatusPacket.Report;    //report to send
		RxDF = StatusPacket.RxDF;       //receive freq offset

        //special handling for the Preemptive TX, remember tx period
		int slotPeriod = WSJTX_TXmodeStr == "FT2" ? 3750 : (WSJTX_TXmodeStr == "FT4" ? 7500 : 15000);
		TXenabled = StatusPacket.TxEnabled;
        if (TXenabled)
        {
			uint64_t ms = GetCurrentTimeMs();
            int DTC = ms % slotPeriod;
            if (TXpreempted && (ms - PreemptiveTXstartMs) < 1000 && !StatusPacket.Transmitting)
                       // We are in the slot following preemptive TX but wsjtx says not TX so must mean slot was wrong
                        // so do a halt TX
            {
                UDPHaltTXevent = true;
                TXpreempted = false;
                string str1 = get_millis() + "Preemptive TX Halt. DTC: " + to_string(DTC) + "\r\n";
				std::cout << str1;
            }

			bool even = (ms / slotPeriod) % 2 == 0;
			int SlotTimeLeft = slotPeriod - DTC;
            if (StatusPacket.Transmitting)   TXeven = even;              //can only be certain when wsjtx is tx'ing
            if (!msgChanged && !StatusPacket.Transmitting && TXenabled && SlotTimeLeft > 3000) TXeven = !even;
			string str1 = dataChanged ? " Data changed. " : " Data unchanged. ";
			string str2 = msgChanged ? " Message changed. " : " Message unchanged. ";
			string str3 = TXeven ? " TXslot Even " : " TXslot Odd ";
			string str4 = StatusPacket.Transmitting ? " Transmitting. " : " Not transmitting. ";
			string str5 = TXenabled ? " TXenabled. " : " TXdisabled. ";
            std::cout << get_millis() << str1 << str2 << str5 << str3 << str4 << " Slot: " << (ms % 60000) / slotPeriod << " SlotTimeLeft: " << SlotTimeLeft << std::endl;
        }
        if (StatusPacket.Transmitting)  TXpreempted = false;            //reset preemptive flag when actual TX starts

		if (!UDPHaltTXevent) PreemptiveTXCheck();                // Check for preemptive TX conditions after processing status update 

        if (!UDPevent) UDPevent = true;     //flag for main UDP server code to update Arduino
                                            //avoid UDPevent race in case Preempt already set it                    

        return ret;
    }
    else if (NewPacket.PacketType == 2) {
        WSJTX_Decode DecodePacket(pkt, NewPacket.index);
        DecodePacket.Decode();
        // Process DecodePacket and emit UI update signal
        return "Decode: " + DecodePacket.Message + "  DT: " + to_string(DecodePacket.DeltaTime);
    }
    else if (NewPacket.PacketType == 3) {
        return "Erase";
        WSJTX_Erase ErasePacket(pkt, NewPacket.index);
        ErasePacket.Decode();
        // emit UI erase message signal
    }
    else if (NewPacket.PacketType == 4) {
		return "Reply";
        WSJTX_Reply ReplyPacket(pkt, NewPacket.index);
        ReplyPacket.Decode();
        // Process ReplyPacket and emit UI update signal
    }
    else if (NewPacket.PacketType == 5) {
		return "Logged";
        WSJTX_Logged LoggedPacket(pkt, NewPacket.index);
        LoggedPacket.Decode();
        // emit UI update signal for logged packet
    }
	else if (NewPacket.PacketType == 17) {
		std::cout << "SetFreq Packet DUMP" << std::endl;
        dump_packet_hex(pkt);
        
        WSJTX_SetFreq SetFreqPacket(pkt, NewPacket.index);
		std::cout << "SetFreq Packet Head: ClientID = " << NewPacket.ClientID << " Index after head = " << NewPacket.index << std::endl;
        try {
            SetFreqPacket = WSJTX_SetFreq(pkt, NewPacket.index);
        }
        catch (const std::exception& e) {
            std::cerr << "Exception during WSJTX_SetFreq construction: " << e.what() << std::endl;
			return "SetFreq Error";
		}
        try {
            SetFreqPacket.Decode();
        }
        catch (const std::exception& e) {
            std::cerr << "Exception during WSJTX_SetFreq Decode: " << e.what() << std::endl;
            return "SetFreq Error";
                    }
        // Process SetFreqPacket and emit UI update signal

		std::cout << "SetFreq Packet: ClientID = " << SetFreqPacket.ClientID << " Freq = " << SetFreqPacket.dialFrequency << " Mode = " << SetFreqPacket.Mode << " TxPeriod = " << SetFreqPacket.TxPeriod << std::endl;
	    return "SetFreq";
    }
    std::cout << "Unknown Packet DUMP type " << to_string(NewPacket.PacketType) << std::endl;
    dump_packet_hex(pkt);
	return "Unknown " + to_string(NewPacket.PacketType);
}

std::string removeTrailingSpaces(std::string str) {
    // Find the position of the last non-space character
    auto pos = str.find_last_not_of(' ');

    // Erase all trailing spaces
    if (pos != std::string::npos) {
        str.erase(pos + 1);
    }
    else {
        // The string contains only spaces
        str.clear();
    }
    return str;
}


std::string EncodeMessage(const std::string& WSJTX_TXmodeStr, const std::string& strMessage) {
    std::cout << get_millis() << "EncodeMessage start: " << WSJTX_TXmodeStr << " " << strMessage << std::endl;

    const std::string ft8Path = strFT8WSJTXpath;
    const std::string ft4Path = strFT4WSJTXpath;
    const std::string ft2Path = strFT2WSJTXpath;

    std::string symbols;
    size_t requiredLength = 0;

    if (WSJTX_TXmodeStr == "FT8") {
        symbols = FT8TxNoSymbols;
        requiredLength = 79;
    }
    else if (WSJTX_TXmodeStr == "FT4") {
        symbols = FT4TxNoSymbols;
        requiredLength = 105;
    }
    else if (WSJTX_TXmodeStr == "FT2") {
        symbols = FT2TxNoSymbols;
        requiredLength = 105;
    }
    else {
        strDecodeMsg = "Invalid WSJTX_TXmodeStr";
        return TxNoSymbols;
    }

    if (strMessage == "TUNE") {
        strDecodeMsg = "TUNE";
        return symbols;
    }

    // --- Command setup with wstring for Unicode API ---
    std::wstring command;
    if (WSJTX_TXmodeStr == "FT8") {
        command = L"\"" + std::wstring(ft8Path.begin(), ft8Path.end()) + L"\"";
    }
    else if (WSJTX_TXmodeStr == "FT4") {
        command = L"\"" + std::wstring(ft4Path.begin(), ft4Path.end()) + L"\"";
    }
    else if (WSJTX_TXmodeStr == "FT2") {
        command = L"\"" + std::wstring(ft2Path.begin(), ft2Path.end()) + L"\"";
    }
    command += L" \"" + std::wstring(strMessage.begin(), strMessage.end()) + L"\"";

    // --- Handle and pipe setup ---
    HANDLE hChildStdOutRead = NULL;
    HANDLE hChildStdOutWrite = NULL;

    SECURITY_ATTRIBUTES saAttr{};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &saAttr, 0)) {
        strDecodeMsg = "CreatePipe failed.";
        return symbols;
    }
    if (!SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        strDecodeMsg = "SetHandleInformation failed.";
        CloseHandle(hChildStdOutRead);
        CloseHandle(hChildStdOutWrite);
        return symbols;
    }

    // --- Process creation ---
    PROCESS_INFORMATION piProcInfo{};
    STARTUPINFO siStartInfo{};
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = hChildStdOutWrite;
    siStartInfo.hStdOutput = hChildStdOutWrite;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    if (!CreateProcess(NULL, command.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &siStartInfo, &piProcInfo)) {
        strDecodeMsg = "CreateProcess failed.";
        CloseHandle(hChildStdOutRead);
        CloseHandle(hChildStdOutWrite);
        return symbols;
    }

    // --- Close parent's write handle to the pipe ---
    CloseHandle(hChildStdOutWrite);

    // --- Read and process output from child process ---
    DWORD bytesRead = 0;
    char buffer[1024]{};
    std::string output;

    while (ReadFile(hChildStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    // --- Final cleanup after reading is complete ---
    CloseHandle(hChildStdOutRead);
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);

    // --- Output parsing and validation ---
    // Simplified parsing logic
    std::istringstream iss(output);
    std::string line;
    int lineCount = 0;
    std::string decodedMessage;

    while (std::getline(iss, line, '\n')) {
        lineCount++;
		std::cout << "Line " << lineCount << ": " << line << std::endl;
        // Use a more robust check for decoded message and symbols line
        if (lineCount == 3) {
            if (line.length() >= 42) {
                decodedMessage = line.substr(42, 40);
            }
            else {
                strDecodeMsg = "Encode error L3 - short line.";
                return TxNoSymbols;
            }
        }
        if ((WSJTX_TXmodeStr == "FT8" && lineCount == 16) || ((WSJTX_TXmodeStr == "FT2" || WSJTX_TXmodeStr == "FT4") && lineCount == 22)) {
            symbols = line;
            break;
        }
    }

    // --- Final symbol processing ---
    if (!symbols.empty()) {
        symbols.erase(std::remove_if(symbols.begin(), symbols.end(), ::isspace), symbols.end());
        if (symbols.length() == requiredLength) {
            strDecodeMsg = removeTrailingSpaces(decodedMessage);
            std::cout << get_millis() << "Encode Done: " << "symbols length = " << symbols.length() << "  strDecodeMsg = " << strDecodeMsg << std::endl;
            std::cout << symbols << std::endl;
            return symbols;
        }
    }

    strDecodeMsg = "ftxcode.exe symbols error - unexpected output.";
    return TxNoSymbols;
}

std::string strPacketType(int type)
{
    switch (type) {
    case 0: return "Heartbeat";
    case 1: return "Status";
    case 2: return "Decode";
    case 3: return "Erase";
    case 4: return "Reply";
    case 5: return "Logged";
	case 6: return "Close";
	case 7: return "HeartbeatEx";
	case 8: return "StatusEx";
	case 9: return "DecodeEx";
	case 10: return "WSPRDecode";
	case 17: return "SetFreq";
    default: return "Unknown";
    }
}

// Function to convert char* to LPWSTR
LPWSTR ConvertToLPWSTR(const char* charString) {
    int len = MultiByteToWideChar(CP_ACP, 0, charString, -1, NULL, 0);
    LPWSTR wcharString = new WCHAR[len];
    MultiByteToWideChar(CP_ACP, 0, charString, -1, wcharString, len);
    return wcharString;
}



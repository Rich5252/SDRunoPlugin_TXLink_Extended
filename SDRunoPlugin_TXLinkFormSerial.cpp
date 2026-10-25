#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#endif

#include <iostream>

#include "SDRunoPlugin_TXLinkForm.h"
#include "RTTY_Server.h"
#include "Heartbeat.h"

#include <nana/gui.hpp>
#include <nana/gui/widgets/label.hpp>
#include <nana/gui/programming_interface.hpp> // Often where 'main_thread_dispatch' lives


//Serial handlers for commands to/from PC TXlink and Arduino.
// 
// Serial commands from PC TXlink sent to Arduino
// {Bn}     Ext PA fan hold time minutes
// {Cn}     DDS Frequency calibration  
// {D}      Power Down (TX off)
// {Fn}     TX frequency
// {Hn}     TX key up hold time ms
// {L%}     Local PA level (QRP)
// {P%)     External PA power level
// {Q0/1}   External PA Off/On
// {U}      Power Up (TX On)
// {V%}		FT8 TX start delay ms (default 250ms)
// {W%}     TX FT8 watchdog - millisecs to end of this tx period
// {X0/1)   FT8 TX on/off
// {Yn}     FT8 TXDF (delta freq)
// {Zmsg}   FT8 message


void SDRunoPlugin_TXLinkForm::ProcessIncomingSerial()
{
	// called from timer event. Read any data while available, uses {} as begin/end message
	// also send FT8 event data

	string NextChar = "";
	string NewMsg = "";
	string LastMsg = "";
	static bool VRX0mute = false;
	static bool VRX1mute = true;

	auto startTime = std::chrono::steady_clock::now();
	const auto maxDuration = std::chrono::milliseconds(20);  // tune as needed
	const size_t MAX_MSG_LEN = 64;   // generous given your ~30-byte normal messages

	while (Serial.DataAvailable() > 0)
	{
		if (std::chrono::steady_clock::now() - startTime > maxDuration)
		{
			string str = "\r\n" + get_millis() + " ProcessIncomingSerial: time budget exceeded, "
				+ "defer to next tick. LastMsg = \"" + LastMsg + "\"";
			if (NewMsg != "") str += ", NewMsg = \"" + NewMsg + "\"";
			str += "\r\n\r\n";
			std::cout << str;		//this helps avoid splitting line in console
			break;
		}

		NextChar = Serial.ReadSerialPortChar();
		//valid read
		if (NextChar == "{")
		{
			NewMsg = "";
			LastMsg = "";
		}
		else if (NextChar == "}")
		{
			//execute compiled message
			if (NewMsg == "")					//null command or an error
			{
				//do nothing for now
			}
			else if (NewMsg == "~TX")					//init message from TXlink Arduino
			{
				//so send all the initial data to TXlink Arduino
				if (Serial.Connected()) {
					//force a freq update here as it can be 20-30seconds after DLL load before TXlink Arduino resets and sends ~TX
					string str = "{F" + std::to_string((int64_t)CurrentTXfreq) + "}";
					std::cout << get_millis() << " SDRunoPlugin_TXLinkForm::ProcessIncomingSerial() - Arduino ~TX received - init TXfreq = " << str << std::endl;
					Serial.WriteSerialPort(str);
					RTTYmsgOutQ.push(str);
					//TODO send to RTTYpipe
				}
				SendFreqCalToTX();
				SendTXlevel(TXlevelTb.caption());
				SendPAlevel(PAlevelTb.caption());
				SendPAstate(false);					//always start with ext PA off for safety
				SendFanHold(GetValidFanHold());
				SendFanLowPWM(GetValidFanLowPWM());
				SendFT8txDelay(GetValidFT8txDelay());
				SendTempLimit(GetValidTempLimit());
				SetMuteTX(true);					//enable vrx1 mute as default

			}
			else if (NewMsg.substr(0, 1) == "M" && DigiTXmodeStr != "TTY")
			{
				VRX0mute = m_controller.GetAudioMute(0);
				VRX1mute = m_controller.GetAudioMute(1);
				if (!DiagnosticTX) {
					m_controller.SetAudioMute(0, true);
					m_controller.SetAudioMute(1, true);
				}
				DiagnosticTX = false;
			}
			else if (NewMsg.substr(0, 1) == "N")
			{
				m_controller.SetAudioMute(0, VRX0mute);
				m_controller.SetAudioMute(1, VRX1mute);
			}
			else if (NewMsg.substr(0, 1) == "F")
			{
				auto __t0 = std::chrono::steady_clock::now();
				SWRFWDtxt.caption(NewMsg.substr(1));
				auto __ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - __t0).count();
				if (__ms > 10) {
					std::cout << get_millis() << " SWRFWDtxt.caption() took " << __ms << "ms" << std::endl;
				}
			}
			else if (NewMsg.substr(0, 1) == "R")
			{
				auto __t0 = std::chrono::steady_clock::now();
				SWRREFtxt.caption(NewMsg.substr(1));
				auto __ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - __t0).count();
				if (__ms > 10) {
					std::cout << get_millis() << " SWRREFtxt.caption() took " << __ms << "ms" << std::endl;
				}

			}
			else if (NewMsg.substr(0, 1) == "S")
			{
				auto __t0 = std::chrono::steady_clock::now();
				SWRSWRtxt.caption(NewMsg.substr(1));
				auto __ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - __t0).count();
				if (__ms > 10) {
					std::cout << get_millis() << " SWRSWRtxt.caption() took " << __ms << "ms" << std::endl;
				}

			}
			else if (NewMsg.substr(0, 1) == "T")
			{
				auto __t0 = std::chrono::steady_clock::now();
				SetTXtemp(NewMsg.substr(1));
				TXtempLbl.caption(NewMsg.substr(1) + " degC");
				auto __ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - __t0).count();
				if (__ms > 10) {
					std::cout << get_millis() << " SetTXtemp.caption() took " << __ms << "ms" << std::endl;
				}

			}
			else if (NewMsg.substr(0, 1) == "D")				//force TX off due to TXtempLimit exceeded
			{
				StandbyBtn.caption("Set TX on");
				StandbyMode = true;
				TXtempLbl.fgcolor(nana::colors::red);
			}
			LastMsg = NewMsg;
			NewMsg = "";		//message completed
		}
		else
		{
			NewMsg.append(NextChar);
			if (NewMsg.length() > MAX_MSG_LEN)
			{
				std::cout << std::endl << get_millis() << " ProcessIncomingSerial: message exceeded max length, discarding (possible noise/desync): \""
					<< NewMsg.substr(0, 20) << "...\"" << std::endl << std::endl;
				NewMsg = "";
			}
		}
		NextChar = "";
	}
}


//Form receives messages to send from other threads on a msgQ that is processed here
//This is called from OnTimer()
void SDRunoPlugin_TXLinkForm::ProcessMessageQueue()
{
	// Process msgs to be sent to hardware

	std::string message = "";
	bool bMsgDone = false;
	while (!bMsgDone)
	{
		{
			std::lock_guard<std::mutex> lock(m_queueMutex);
			if (m_messageQueue.empty()) {
				bMsgDone = true;
			}
			else
			{
				message = m_messageQueue.front();
				m_messageQueue.pop();
				bMsgDone = false;
			}
		}

		if (bMsgDone) { return; }

		// special cases
		if (message == "$SendRXfreq$")
		{
			TRY_STEP("SendRXfreq", SendRXfreq());
			return;
		}
		else if (message == "$SendTXfreq$")
		{
			TRY_STEP("SendTXfreq", SendTXfreq());
			return;
		}
		else if (message == "$SaveIniData$")
		{
			TRY_STEP("SaveIniData", SaveIniData());
			return;
		}
		else
		{
			if (message.rfind("RTTY:", 0) == 0) { // Command starts with "RTTY:"
				std::string rttyCmdText = message.substr(5); // Extract text after "RTTY:"
				message = ProcessRTTYcommand(rttyCmdText);
			}

			if (message.rfind("JTTY:", 0) == 0) { // Command starts with "RTTY:"
				std::string jttyCmdText = message.substr(5); // Extract text after "RTTY:"
				message = ProcessJTTYcommand(jttyCmdText);
			}

			// Send serial message string to Arduino
			_SendSerialMsg(message);
		}
	}
}


std::string SDRunoPlugin_TXLinkForm::ProcessRTTYcommand(std::string rttyCmdText)
{
	// Process command message string from RTTY server.
	// eg {M1}{Zrtty message to transmit}{X1}
	// use the {Mx} to set mode, {Z} to set message, {X1} to start TX, {X0} to stop TX

	if (rttyCmdText.rfind("{M1}", 0) >= 0) {				//set DigiTXmodeStr and UI mode Lbl to RTTY
		DigiTXmodeStr = "TTY";							//only space for 3 chars in Lbl
		FT8Lbl.format(true);
		FT8Lbl.caption("<bold=true size=10>" + DigiTXmodeStr + "</>");    // FT8Lbl.caption(WSJTX_TXmodeStr);		//FT8Lbl.caption(< bold size = 12>" + "FT8" + " < / > ");
		FT8txOnLbl.bgcolor(nana::colors::red);
	}

	return rttyCmdText;
}

std::string SDRunoPlugin_TXLinkForm::ProcessJTTYcommand(std::string jttyCmdText)
{
	// Process command message string from JTTY (RTTY) server.
	// eg {X0}

	if (jttyCmdText.rfind("{X0}", 0) >= 0) {			//used for abort from JTTY app
		DigiTXmodeStr = "JTTY";							//only space for 3 chars in Lbl
		FT8Lbl.format(true);
		FT8Lbl.caption("<bold=true size=10>" + DigiTXmodeStr + "</>");    // FT8Lbl.caption(WSJTX_TXmodeStr);		//FT8Lbl.caption(< bold size = 12>" + "FT8" + " < / > ");
		FT8txOnLbl.bgcolor(nana::colors::light_grey);
	}

	return jttyCmdText;
}

bool SDRunoPlugin_TXLinkForm::SendSerialMsg(const std::string& msg)
{
	// Lock the mutex to safely push the message
	std::lock_guard<std::mutex> lock(m_queueMutex);
	m_messageQueue.push(msg);
	return true;
}

// used in ATU tune dialog to change settings
bool SDRunoPlugin_TXLinkForm::_SendSerialMsg(string Msg)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		is_sent = Serial.WriteSerialPort(Msg);
	}
	return is_sent;
}

//Load COM port
string SDRunoPlugin_TXLinkForm::LoadCOM()
{
	std::string tmp = "COM5";
	m_controller.GetConfigurationKey("TXLink.COM", tmp);

	if (tmp.empty())
	{
		return "";
	}
	return tmp;
}

void SDRunoPlugin_TXLinkForm::SaveCOM(string strCOM)
{
	m_controller.SetConfigurationKey("TXLink.COM", strCOM);
}

void SDRunoPlugin_TXLinkForm::CloseCOMPort()
{
	std::cout << get_millis() << "Request Close TxLink COM port" << std::endl;
	Serial.CloseSerialPort();
	std::cout << get_millis() << "TxLink COM port closed" << std::endl;
}
#include <sstream>

#include "SDRunoPlugin_TXLinkForm.h"
#include "SDRunoPlugin_TXLinkUi.h"
#include "UDP_Server.h"
#include "SDRUnoHelper.h"
#include "TunerServer.h"
#include "RealTimeClock_Utils.h"

#include <nana/gui.hpp>
#include <nana/gui/widgets/label.hpp>
#include <nana/gui/programming_interface.hpp> // Often where 'main_thread_dispatch' lives


//part of the Form OnTimer poll processing, called from OnTimer() in SDRunoPlugin_TXLinkForm.cpp
// handles the UDPserver messages that are sent via the MessageQueue by the UDPserver thread

void SDRunoPlugin_TXLinkForm::ProcessUDPEvent()
{													//called from Form timer event to process wsjtx UDP	
	//WSJT_X FT8 UDP request?

	// Process EVERY queued event, in order - NOT coalesced to just the latest.
	// Some fields here are one-shot/edge-triggered (newDXcall, haltTXevent,
	// txModeChanged) rather than persistent state, so collapsing a burst down
	// to "just the last one" can silently drop a real occurrence (e.g. a new
	// DX call) if a later, unrelated event arrives in the same drain cycle.
	// WSJT-X's UDP packet rate is naturally low, so processing every event
	// individually here is fine - unlike the frequency/tuner queues, which
	// deliberately DO coalesce because those really are "only the latest
	// value matters" updates.
	UDPStatusEvent evt;
	while (UDPStatusQueue.try_pop(evt))
	{
		m_lastUDPStatus = evt;		// Form-owned copy, safe for button handlers/millisToEnd to read anytime afterward
		if (evt.txModeChanged) { m_txModeChangePending = true; }

		//following logic to handle correct mode restore on startup
		static bool FirstTime = true;				//first time in this function so maybe ignore wsjtx for now?
		if (FirstTime && SplitMode != SplitFT8)     //only reset datamode when SplitModeFT8 has been restored on startup
		{
			if (evt.newFrequency > 0) { FirstTime = false; }			//wait until wsjtx first changes freq to set data mode if datamode was not restored on startup
			continue;		// skip this event, but keep processing any further queued events
		}

		FirstTime = false;		//setup data mode on first UDP event only when SplitFT8 restored in data mode

		bool is_sent = false;
		long TXtimeToEnd = 0;
		if (Serial.Connected())
		{
			//always update freq when changed in wsjtx
			if (evt.newFrequency > 0)			//user changed band / mode in wsjtx
			{
				CurrentWSJTXfreq = static_cast<double>(evt.newFrequency);					//local form master setting
				SetupDataMode();
			}

			std::string str = "{X0}";
			std::string strW = "";			//for W message to set time to end of TX
			if (evt.dataChanged)
			{
				//refresh TX data - this will usually be after wsjtx decodes but before next TX start period
				str = "{M8}";
				if (evt.txModeStr == "FT4") { str = "{M4}"; }
				if (evt.txModeStr == "FT2") { str = "{M2}"; }

				str = str + "{Y" + to_string(evt.txDF) + "}";

				str = str + "{Z" + evt.txSymbols + "}";
				is_sent = Serial.WriteSerialPort(str);
			}

			str = "{X0}";
			strW = "";
			if (evt.txFlag) {
				//this UDP datagram is sent immediately at start of tx period
				if (evt.decodeMsg != "TUNE")
				{
					TXtimeToEnd = millisToEnd();
				}
				else
				{
					TXtimeToEnd = 30000;		//TUNE for 30 seconds from now
				}
				strW = "{W" + to_string(TXtimeToEnd) + "}";

				// TX key on message
				str = "{X1}";							//if (TXtimeToEnd >= 12000) { str = "{X1}"; }		//only enable if enough time left
			}
			//indicate FT8 tx on UI form
			if (str == "{X0}")
			{
				FT8txOnLbl.bgcolor(nana::colors::light_grey);
			}
			else           //FT8 TX
			{
				DigiTXmodeStr = evt.txModeStr;		//save current TX mode
				FT8Lbl.format(true);
				FT8Lbl.caption("<bold=true size=10>" + DigiTXmodeStr + "</>");
				FT8txOnLbl.bgcolor(nana::colors::red);

				SetupDataMode();
			}

			if (evt.haltTXevent) {
				TXtimeToEnd = 0;
				strW = "{X0}";
				str = "{W0}";					//force TX to stop (needs this command order)
			}

			//send TX last so freq & message updated first in Arduino
			is_sent = true;
			if (SplitMode == SplitFT8 && (DigiTXmodeStr == "FT2" || DigiTXmodeStr == "FT4" || DigiTXmodeStr == "FT8"))
			{
				str = strW + str;
				is_sent = Serial.WriteSerialPort(str); //Arduino setup commands will now send in earlier status UDP
				// then Wn & X1 will be sent here
			}

		}
		ShowIsSent(is_sent);
		FT8msgLbl.caption(evt.decodeMsg + " - " + to_string(TXtimeToEnd));

	}	// end while - next queued event, if any
}

void SDRunoPlugin_TXLinkForm::ReTransmit() {
	//diagnostic only

	uint64_t Ms = GetCurrentTimeMs();
	int slotPeriod = m_lastUDPStatus.txModeStr == "FT2" ? 3750 : (m_lastUDPStatus.txModeStr == "FT4" ? 7500 : 15000);
	Ms = Ms % slotPeriod;
	while (Ms > 10)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		Ms = GetCurrentTimeMs() % slotPeriod;
	}

	long TimeToEnd = millisToEnd();
	std::string str = "{W" + to_string(TimeToEnd) + "}{X1}";
	Serial.WriteSerialPort(str);
	DiagnosticTX = true;				//controls mute
}

void SDRunoPlugin_TXLinkForm::SetupDataMode()
{
	freqTb.bgcolor(nana::colors::light_salmon);
	FT8Lbl.fgcolor(nana::colors::black);
	FT8Lbl.bgcolor(nana::colors::light_salmon);
	FT8Lbl.transparent(false);
	{
		std::lock_guard<std::mutex> lock(g_splitModeMutex);
		SplitMode = SplitFT8;
	}
	// and set FT8 RX in vrx0 & vrx1
	if (CurrentTXfreq != CurrentWSJTXfreq)				//make sure freq is set
	{
		CurrentTXfreq = CurrentWSJTXfreq;
		SetRXFreq();									//sets both vfos to CurrentTXfreq
		SendTXfreq();									//update TX hardware
	}
	CurrentRXfreq = CurrentTXfreq + m_lastUDPStatus.txDF;

	double f0 = GetVfoFrequencySafe(m_controller, 0);
	double f1 = GetVfoFrequencySafe(m_controller, 1);
	if (CurrentTXfreq != f0) SetVfoFrequencySafe(m_controller, 0, CurrentTXfreq);
	if (CurrentRXfreq != f1) SetVfoFrequencySafe(m_controller, 1, CurrentRXfreq);

	m_parent._updateTX_AnnoEntry("FT8Base", "", (int64_t)CurrentTXfreq);
	m_parent._updateRX_AnnoEntry("FTX", "", (int64_t)CurrentRXfreq);
	m_controller.SetDemodulatorType(0, m_controller.DemodulatorUSB);
	FixFreqBtn.caption("UnSplit");
	if (m_txModeChangePending)
	{
		int bndw = FilterBandwidthFT8;
		if (DigiTXmodeStr == "FT4") { bndw = FilterBandwidthFT4; }
		if (DigiTXmodeStr == "FT2") { bndw = FilterBandwidthFT2; }
		m_controller.SetFilterBandwidth(0, bndw);
		m_txModeChangePending = false;
	}
}


long SDRunoPlugin_TXLinkForm::millisToEnd() {

	//calc millis to end of this period relative to world UTC
	long millisToEnd = (15000 - (GetCurrentTimeMs() + RTCdelay) % 15000 - 150);								//FT8 15sec slot
	if (m_lastUDPStatus.txModeStr == "FT4") { millisToEnd = (7500 - (GetCurrentTimeMs() + RTCdelay) % 7500 - 50); }   //7.5sec slot
	if (m_lastUDPStatus.txModeStr == "FT2") { millisToEnd = (3750 - (GetCurrentTimeMs() + RTCdelay) % 3750); }		//3.75sec slot

	return millisToEnd;
}
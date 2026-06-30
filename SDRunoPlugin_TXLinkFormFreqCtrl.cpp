#include <sstream>

#include "SDRunoPlugin_TXLinkForm.h"
#include "SDRunoPlugin_TXLinkATUDialog.h"
#include "SDRunoPlugin_TXLinkUi.h"
#include "RTTY_Server.h"
#include "DXspiderClient.h"
#include "Heartbeat.h"
#include "SDRUnoHelper.h"

#include <nana/gui.hpp>
#include <nana/gui/programming_interface.hpp> // Often where 'main_thread_dispatch' lives


// Process frequency changes from SDRuno thread (via HandleEvent() in SDRunoPlugin_TXLink.cpp)
void SDRunoPlugin_TXLinkForm::QueueFrequencyChanged(double freq0, double freq1)
{
	std::lock_guard<std::mutex> lock(m_freqQueueMutex);
	m_freqQueue.push({ freq0, freq1 });
}


// Called from OnTimer(), same place ProcessMessageQueue() is called
void SDRunoPlugin_TXLinkForm::DrainFreqQueue()
{
	FreqChangeRequest latest;
	{
		std::lock_guard<std::mutex> lock(m_freqQueueMutex);
		while (!m_freqQueue.empty())
		{
			latest = m_freqQueue.front();
			m_freqQueue.pop();
			ProcessFrequencyChanged(latest.freq0, latest.freq1);
		}
	}
}

// Called only from the Nana thread (e.g. from your existing window-proc hook
// that already handles WM_UDP_DATA_READY, or from a nana::timer tick).
void SDRunoPlugin_TXLinkForm::ProcessFrequencyChanged(double freq0, double freq1)
{
	// All the logic that used to live in UiHandleEvent's FrequencyChanged case
	// moves here. It's now safe to touch m_form->... directly because we're
	// guaranteed to be on the form's own thread.

	static double lastF0 = 0;
	static double lastF1 = 0;
	static double OFStxFreq = 0;

	bool bF0changed = (uint32_t)freq0 != (uint32_t)lastF0;
	if (bF0changed) lastF0 = freq0;

	bool bF1changed = (uint32_t)freq1 != (uint32_t)lastF1;
	if (bF1changed) lastF1 = freq1;

	if (!bOFSchkbox) OFStxFreq = 0;
	if (bOFSchkbox && bF1changed) { OFStxFreq = freq1 - freq0; }

	if (!bF0changed && !bF1changed) return;

	if (freq0 != 0 && freq0 != CurrentRXfreq && SplitMode != SplitFT8)
	{
		CurrentRXfreq = freq0;
		m_parent._updateRX_AnnoEntry("RX", "", (int64_t)CurrentRXfreq);
	}

	if (SplitMode == SplitNone && bF1changed)
	{
		SetVfoFrequencySafe(m_controller, 1, freq0);
	}
	else if (SplitMode == SplitNone && bF0changed)
	{
		SendSerialMsg("$SendRXfreq$");
	}
	else if (SplitMode == SplitFT8)
	{
		if (freq0 != CurrentTXfreq)
		{
			SetVfoFrequencySafe(m_controller, 0, CurrentTXfreq);
		}
	}
	else
	{
		if (freq1 > 0 && SplitMode != SplitFT8)
		{
			if (bOFSchkbox) {
				freq1 = freq0 + OFStxFreq;
				SetVfoFrequencySafe(m_controller, 1, freq1);
			}
			if (freq1 != CurrentTXfreq)
			{
				CurrentTXfreq = freq1;
				SendSerialMsg("$SendTXfreq$");
			}
		}
	}
}


void SDRunoPlugin_TXLinkForm::SendRXfreq()
{

	if (TrackFreq || freqTb.caption() == "")
	{
		//update TX freq fron VRX0 freq change
		double tmp = GetVfoFrequencySafe(m_controller, 0);
		if (tmp > 0)
		{
			CurrentTXfreq = tmp;
			CurrentRXfreq = tmp;
		}
	}
	TRY_STEP("SendRXfreq-SendTXfreq", SendTXfreq());
}

std::mutex mtx2;
void SDRunoPlugin_TXLinkForm::SendTXfreq()
{
	// called from timer event msg queue, sends current TX freq to TXlink Arduino
	bool is_sent = false;
	string strTXfreq = "";
	std::lock_guard<std::mutex> lock(mtx2);

	// following needed to avoid sending same freq multiple times
	// by the time it gets here the freq may have stabalised in tuner thread, so only send if different.
	static double LastTXfreq = -1.0;		//static so only set once per call
	if (CurrentTXfreq == LastTXfreq) { return; }		//no change so skip sending
	LastTXfreq = CurrentTXfreq;				//set last TX freq to current

	strTXfreq = std::to_string((int64_t)CurrentTXfreq);

	// send current TX freq to TX
	if (!Serial.Connected())
	{
		//attempt to restart COM port
		string strCOM = COMportTb.caption();
		strCOM = COMportPrefix + strCOM;
		Serial.ReOpenPort(strCOM, COM_BAUD_RATE, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(250));		//wait for Arduino to restart
	}
	if (Serial.Connected()) {

		std::string str = "{F" + strTXfreq + "}";
		is_sent = Serial.WriteSerialPort(str);
		RTTYmsgOutQ.push(str);
		std::cout << str << std::endl;
	}

	ShowIsSent(is_sent);				//animate form button to show if sent ok

	//update Plugin label to show TXfreq
	//add dots to show 1000's in Plugin label
	for (int i = strTXfreq.length(); i > 3; i = i - 3)
	{
		strTXfreq = strTXfreq.substr(0, i - 3) + "." + strTXfreq.substr(i - 3);
	}
	//freqTb.format(true);
	//freqTb.caption("<bold size=12>" + strTXfreq + "</>");				//TXfreq + "< bold size = 14 color = 0x000000 font = \"Verdana\"></>");
	//freqTb.caption(strTXfreq);

	if (SplitMode != SplitFT8)
	{
		m_parent._updateTX_AnnoEntry("TX", "", (int64_t)CurrentTXfreq);
		m_parent._updateRX_AnnoEntry("RX", "", (int64_t)CurrentRXfreq);
	}
	else
	{
		m_parent._updateTX_AnnoEntry("FT8base", "", (int64_t)CurrentTXfreq);
		m_parent._updateRX_AnnoEntry("FTX", "", (int64_t)CurrentRXfreq);
	}

	double vtxFreq = GetVfoFrequencySafe(m_controller, 1);
	if (vtxFreq != CurrentTXfreq && vtxFreq > 0 && SplitMode != SplitFT8)
	{
		SetVfoFrequencySafe(m_controller, 1, CurrentTXfreq);					// force vrx1 to track TX freq
	}
}


void SDRunoPlugin_TXLinkForm::SetRXFreq()
{
	SetVfoFrequencySafe(m_controller, 0, CurrentTXfreq);
	SetVfoFrequencySafe(m_controller, 1, CurrentTXfreq);
}

void SDRunoPlugin_TXLinkForm::SwapTxRxFreq()
{
	if (SplitMode == SplitTxRx)
	{
		//swap TX and RX freqs
		double VFO0 = GetVfoFrequencySafe(m_controller, 0);
		double VFO1 = GetVfoFrequencySafe(m_controller, 1);
		SetVfoFrequencySafe(m_controller, 0, VFO1);
		SetVfoFrequencySafe(m_controller, 1, VFO0);
	}
}
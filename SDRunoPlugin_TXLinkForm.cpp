#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#endif

#include "SDRunoPlugin_TXLinkForm.h"
#include "SDRunoPlugin_TXLinkSettingsDialog.h"
#include "SDRunoPlugin_TXLinkATUDialog.h"
#include "SDRunoPlugin_TXLinkUi.h"
#include "UDP_Server.h"
#include "RTTY_Server.h"
#include "DXspiderClient.h"
#include "Heartbeat.h"
#include "SDRUnoHelper.h"

#include <nana/gui.hpp>
#include <nana/gui/widgets/listbox.hpp>
#include <nana/gui/widgets/label.hpp>
#include <nana/gui/programming_interface.hpp> // Often where 'main_thread_dispatch' lives



// Form constructor with handles to parent and uno controller - launches form Setup
SDRunoPlugin_TXLinkForm::SDRunoPlugin_TXLinkForm(SDRunoPlugin_TXLinkUi& parent, IUnoPluginController& controller) :
	nana::form(nana::API::make_center(formWidth, formHeight), nana::appearance(false, true, false, false, true, false, false)),
	m_parent(parent),
	m_timerCount(0),
	m_controller(controller),
	Serial(com_port, COM_BAUD_RATE, false)
{
	Setup();
}

// Form deconstructor
SDRunoPlugin_TXLinkForm::~SDRunoPlugin_TXLinkForm()
{
	std::cout << get_millis() << " SDRunoPlugin_TXLinkForm destructor called" << std::endl;
}

// Start Form and start Nana UI processing
void SDRunoPlugin_TXLinkForm::Run()
{
	show();
	nana::exec();			//returns here after nana::API::exit_all() called

	std::cout << get_millis() << " SDRunoPlugin_TXLinkForm Run() EXIT" << std::endl;
	//$m_parent.FormExitAllRequest = false;		//tell parent closedown function that nana forms have exited
}

//This is main processing function for the form and it handles all of the incoming messages and serial data.
//Note multiple timers can cause event interruption on the thread so were difficult to make thread safe.
// This is why we use a single timer and message queues to handle all of the events in a single thread.

void SDRunoPlugin_TXLinkForm::OnTimer()
{
	g_formHeartbeatMs.store(now_ms(), std::memory_order_relaxed);
	m_timerCount++;

	if (m_parent.FormPrepareUnloadRequest)				//request from TXLink destructor - THIS NEEDS TO HAPPEN FIRST
	{
		std::cout << get_millis() << " SDRunoPlugin_TXLinkForm::m_timer.elapse - FormPrepareUnloadRequest" << std::endl;
		UnloadInProgress = true;						//set unload in progress flag
		SaveIniData();								//save ini data before closing
		InitiateFormUnload();						//request form unload
		CloseCOMPort();									//COM to TX no longer needed
		m_parent.FormPrepareUnloadRequest = false;		//clear the request flag to signal complete to Ui
		return;											//skip the rest of the code in this elapse function
	}
	if (UnloadInProgress)								//prep already done for final unload			
	{
		if (m_parent.FormExitAllRequest && !BackFromExitRequest)				////request from TXLinkUI destructor
		{
			std::cout << get_millis() << " SDRunoPlugin_TXLinkForm::m_timer.elapse - FormExitAllRequest" << std::endl;
			nana::API::exit_all();						//close the nana UI
			std::cout << get_millis() << " SDRunoPlugin_TXLinkForm::m_timer.elapse - back from nana::API::exit_all()" << std::endl;
			BackFromExitRequest = true;				//set flag to signal back from exit request
			m_parent.FormExitAllRequest = false;		//clear the request flag to signal complete to Ui
			return;									//another tick never happens after nana::API::exit_all();
		}
		if (BackFromExitRequest)						//shouldnt get here as exit_all() turns off timer too
		{
			std::cout << get_millis() << " SDRunoPlugin_TXLinkForm::m_timer.elapse - BackFromExitRequest - SHOULD NOT GET HERE" << std::endl;
			return;									//skip the rest of the code in this elapse function if UnloadInProgress
		}
		//std::cout << get_millis() << " SDRunoPlugin_TXLinkForm::m_timer.elapse - UnloadInProgress - waiting for ExitAllRequest" << std::endl;
		return;										//skip the rest of the code in this elapse function if UnloadInProgress
	}

	TRY_STEP("ProcessUDPEvent", ProcessUDPEvent());		//now drains UDPStatusQueue internally; cheap no-op if empty

	TRY_STEP("CheckWindowState", CheckWindowState());					//check if main SDRuno window minimized or restored

	//update TX Freq control
	string strTXfreq = AddThousandsSeparator((int64_t)CurrentTXfreq);
	freqTb.format(true);
	freqTb.caption("<bold size=12>" + strTXfreq + "</>");
	//freqTb.caption(strTXfreq);

	bRITchkbox = RITchkbox.checked();		//get RIT checkbox state
	bOFSchkbox = OFSchkbox.checked();

	TRY_STEP("DrainFreqQueue", DrainFreqQueue());	//process any new freq changes from SDRuno

	TRY_STEP("ProcessMessageQueue", ProcessMessageQueue());					//process any external messages in the queue

	// look for incoming serial and UDP on timer event
	TRY_STEP("ProcessIncomingSerial", ProcessIncomingSerial());

}


string SDRunoPlugin_TXLinkForm::GetCOM()
{
	return COMportTb.caption();
}

string SDRunoPlugin_TXLinkForm::GetTXlevel()
{
	return TXlevelTb.caption();
}

string SDRunoPlugin_TXLinkForm::GetPAlevel()
{
	return PAlevelTb.caption();
}

string SDRunoPlugin_TXLinkForm::GetRIT()
{
	string ret = RITchkbox.checked() ? "true" : "false";
	return ret;
}

void SDRunoPlugin_TXLinkForm::SetRIT(string RIT)
{
	RITchkbox.check(RIT == "true");
}

string SDRunoPlugin_TXLinkForm::GetOFS()
{
	string ret = OFSchkbox.checked() ? "true" : "false";
	return ret;
}

void SDRunoPlugin_TXLinkForm::SetOFS(string RIT)
{
	OFSchkbox.check(RIT == "true");
}

void SDRunoPlugin_TXLinkForm::SetStandbyMode(bool Mode)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{D}";
		if (!Mode)
		{
			//make sure TX levels and QRO mode are set correctly
			string strTXlevel = TXlevelTb.caption();
			SendTXlevel(strTXlevel);
			string strPAlevel = PAlevelTb.caption();
			SendPAlevel(strPAlevel);
			SendPAstate(PAchkbox.checked());
			str = "{U}";
		}
		is_sent = Serial.WriteSerialPort(str);
	}
}


void SDRunoPlugin_TXLinkForm::CheckWindowState()
{
	WINDOWPLACEMENT wd{};
	wd.length = sizeof(WINDOWPLACEMENT);
	wd.flags = 0;
	UINT nCurShow;
	bool UNOvisible = true;
	if (GetWindowPlacement(UNOhwnd, &wd))
	{
		nCurShow = wd.showCmd;
		if (nCurShow == SW_SHOWMINIMIZED) { UNOvisible = false; }
	}
	if (UNOvisible)
	{
		//normal
		show();
	}
	else
	{
		hide();
	}
}

std::string SDRunoPlugin_TXLinkForm::AddThousandsSeparator(int64_t value)
{
	bool negative = value < 0;
	std::string digits = std::to_string(negative ? -value : value);

	std::string result;
	int count = 0;
	for (int i = static_cast<int>(digits.length()) - 1; i >= 0; --i)
	{
		result.insert(result.begin(), digits[i]);
		if (++count % 3 == 0 && i != 0)
		{
			result.insert(result.begin(), '.');
		}
	}
	return negative ? "-" + result : result;
}




bool SDRunoPlugin_TXLinkForm::bLastTenCalls(string DXcall)
{
	// look for recent DXcalls
	bool ret = false;			//not found

	if (DXcall != "")
	{
		for (int i = 0; i < nCalls; i++)
		{
			if (strLastTenCalls[i] == DXcall)
			{
				ret = true;
				break;
			}
		}
		if (!ret)
		{
			//call not found so insert new one at next location
			if (++nLastCall >= nCalls)			//nLastCall == -1 means empty
			{
				nLastCall = 0;
			}
			strLastTenCalls[nLastCall] = DXcall;
		}
	}
	return false;		// return ret;
}





void SDRunoPlugin_TXLinkForm::ShowIsSent(bool is_sent)
{
	//return;
	if (is_sent) {
		if (COMportTb.bgcolor() == nana::colors::green)
		{
			COMportTb.bgcolor(nana::colors::blue);
			COMportTb.fgcolor(nana::colors::white);
		}
		else
		{
			COMportTb.bgcolor(nana::colors::green);
			COMportTb.fgcolor(nana::colors::white);
		}
	}
	else
	{
		//Port not open or write failed
		COMportTb.bgcolor(nana::colors::red);
		COMportTb.fgcolor(nana::colors::white);
		std::cout << "SDRunoPlugin_TXLinkForm::ShowIsSent() - Serial port not connected or write failed" << std::endl;
	}
}

void SDRunoPlugin_TXLinkForm::SendTXlevel(string strTXlevel)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{L" + strTXlevel + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
	if (is_sent) {
		TXlevelTb.bgcolor(nana::colors::white);
	}
	else
	{
		TXlevelTb.bgcolor(nana::colors::red);
	}

	//display dB rel full output
	string strDB = "";
	double dB = strToDouble(strTXlevel);
	dB = abs(dB);
	if (dB == 0)
	{
		dB = -99;
	}
	else
	{
		if (dB > 100)
		{
			dB = 100;
		}
		dB = 20 * log10(dB / 100);
		dB = int(dB);
	}
	strDB = to_string(dB);

	//only need upto decimal
	strDB = strDB.substr(0, strDB.find(".", 0));
	TXleveldBLbl.caption(strDB + "dB");
}

void SDRunoPlugin_TXLinkForm::SendPAlevel(string strPAlevel)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{P" + strPAlevel + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
	if (is_sent) {
		PAlevelTb.bgcolor(nana::colors::white);
	}
	else
	{
		PAlevelTb.bgcolor(nana::colors::red);
	}

	//display dB rel full output
	string strDB = "";
	double dB = strToDouble(strPAlevel);
	dB = abs(dB);
	if (dB == 0)
	{
		dB = -99;
	}
	else
	{
		if (dB > 100)
		{
			dB = 100;
		}
		dB = 20 * log10(dB / 100);
		dB = int(dB);
	}
	strDB = to_string(dB);

	//only need upto decimal
	strDB = strDB.substr(0, strDB.find(".", 0));
	PAleveldBLbl.caption(strDB + "dB");
}

void SDRunoPlugin_TXLinkForm::SendPAstate(bool PAstate)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string strState = "0";				//PA off
		if (PAstate) { strState = "1"; }	//PA on
		string str = "{Q" + strState + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
}

void SDRunoPlugin_TXLinkForm::SendTXhold(string TXhold)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{H" + TXhold + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
}

void SDRunoPlugin_TXLinkForm::SendFanHold(string FanHold)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{B" + FanHold + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
}

void SDRunoPlugin_TXLinkForm::SendFanLowPWM(string FanLowPWM)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{R" + FanLowPWM + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
}

void SDRunoPlugin_TXLinkForm::SendFT8txDelay(string FT8txDelay)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{V" + FT8txDelay + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
}

void SDRunoPlugin_TXLinkForm::SendTempLimit(string TempLimit)
{
	bool is_sent = false;
	if (Serial.Connected())
	{
		string str = "{T" + TempLimit + "}";
		is_sent = Serial.WriteSerialPort(str);
	}
}

void SDRunoPlugin_TXLinkForm::SetMuteTX(bool chk)
{
	m_controller.SetAudioMute(1, chk);
}

void SDRunoPlugin_TXLinkForm::SendFreqCalToTX()
{
	string tmp = "";
	m_controller.GetConfigurationKey("TXLink.freqCal", tmp);
	if (tmp.empty()) {
		tmp = "0";								//default
		std::lock_guard<std::mutex> l(m_lock);
		m_controller.SetConfigurationKey("TXLink.freqCal", tmp);
	}
	try {
		int CalVal = stoi(tmp);
		//only send if it can be converted to int
		bool is_sent = false;
		if (Serial.Connected())
		{
			string str = "{C" + tmp + "}";
			is_sent = Serial.WriteSerialPort(str);
		}
		SendRXfreq();				//to reset it????
	}
	catch (const std::invalid_argument&) {
		std::cerr << "Argument is invalid\n";
		//throw;
	}
	catch (const std::out_of_range&) {
		std::cerr << "Argument is out of range for a double\n";
		//throw;
	}
}

double SDRunoPlugin_TXLinkForm::strToDouble(string str)
{
	double d = 0;

	try {
		d = std::stod(str);
	}
	catch (const std::invalid_argument&) {
		std::cerr << "Argument is invalid\n";
		//throw;
	}
	catch (const std::out_of_range&) {
		std::cerr << "Argument is out of range for a double\n";
		//throw;
	}
	return d;
}

void SDRunoPlugin_TXLinkForm::FixFreqBtnClicked()
{
	// sets split mode back to normal from FT8 and toggles normal Split
	if (SplitMode != SplitNone)
	{
		FixFreqBtn.caption("Split");
		TrackFreq = true;
		{
			std::lock_guard<std::mutex> lock(g_splitModeMutex);
			SplitMode = SplitNone;
		}
		freqTb.bgcolor(nana::colors::white);

		FT8Lbl.fgcolor(nana::colors::white);
		FT8Lbl.transparent(true);
		freqTb.fgcolor(nana::colors::black);
		m_controller.SetDemodulatorType(0, m_controller.DemodulatorCW);
		m_controller.SetFilterBandwidth(0, FilterBandwidthCW);
	}
	else
	{
		FixFreqBtn.caption("UnSplit");
		TrackFreq = false;
		{
			std::lock_guard<std::mutex> lock(g_splitModeMutex);
			SplitMode = SplitTxRx;
		}
		freqTb.bgcolor(nana::colors::red);
		freqTb.fgcolor(nana::colors::white);
		FT8Lbl.fgcolor(nana::colors::white);
		FT8Lbl.transparent(true);
	}
	FixFreqButtonClicked();
}

void SDRunoPlugin_TXLinkForm::AtuBtnClicked()
{
	// Open Atu dialog - one only

	if (!bATUformOpen)
	{
		//Create a new settings dialog object
		//SDRunoPlugin_TemplateATUDialog AtuDialog{ this, m_controller };
		m_atuDialog = std::make_unique<SDRunoPlugin_TemplateATUDialog>(*this, m_controller);

		//disable this form so settings dialog retains top level focus
		//this->enabled(false);

		//Attach a handler to the settings dialog close event
		m_atuDialog->events().unload([&] { AtuDialog_Closed(); });
		bATUformOpen = true;

		//Show the setttings dialog
		m_atuDialog->Run();
	}
}

void SDRunoPlugin_TXLinkForm::AtuDialog_Closed()
{
	//DO NOT REMOVE THE FLLOWING CODE it is required for the proper operation of the settings dialog form

	this->enabled(true);
	this->focus();
	bATUformOpen = false;

	//TODO: Extra code goes here to be preformed when settings dialog form closes
}



//get setting from ini file and create a default if not already set
//
std::string SDRunoPlugin_TXLinkForm::GetSetConfigurationKey(std::string strItem, std::string strDefault)
{
	std::string tmp = "";
	m_controller.GetConfigurationKey(strItem, tmp);
	if (tmp == "") {
		tmp = strDefault;
		m_controller.SetConfigurationKey(strItem, tmp);
	}
	return tmp;
}


void SDRunoPlugin_TXLinkForm::FixTXup()
{
	TXupHz = GetValidTXupHz();
	CurrentTXfreq = CurrentTXfreq + TXupHz;			//increment every click
	if (TrackFreq)
	{	//take it out of tracking
		FixFreqBtn.caption("UnSplit");
		TrackFreq = false;
		{
			std::lock_guard<std::mutex> lock(g_splitModeMutex);
			SplitMode = SplitTxRx;
		}
		freqTb.bgcolor(nana::colors::red);
		freqTb.fgcolor(nana::colors::white);
		FT8Lbl.fgcolor(nana::colors::white);
		FT8Lbl.transparent(true);
	}
	SendTXfreq();					//and update TX module
}

int SDRunoPlugin_TXLinkForm::GetValidTXupHz()
{
	string tmp = "";
	int Val = 500;
	m_controller.GetConfigurationKey("TXLink.UPfreq", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "500";								//default
		Val = 500;
	}
	try {
		Val = stoi(tmp);
	}
	catch (const std::invalid_argument&) {
		Val = 500;
		tmp = "500";
		//std::cerr << "Argument is invalid\n";
		//throw;
	}
	catch (const std::out_of_range&) {
		Val = 500;
		tmp = "500";
		//std::cerr << "Argument is out of range for a double\n";
		//throw;
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.UPfreq", tmp);
	return Val;
}

string SDRunoPlugin_TXLinkForm::GetValidTXhold()
{
	string tmp = "";
	int Val = 450;
	m_controller.GetConfigurationKey("TXLink.TXhold", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "450";								//default
		Val = 450;
	}
	try {
		Val = stoi(tmp);
	}
	catch (const std::invalid_argument&) {
		Val = 450;
		tmp = "450";
		//std::cerr << "Argument is invalid\n";
		//throw;
	}
	catch (const std::out_of_range&) {
		Val = 450;
		tmp = "450";
		//std::cerr << "Argument is out of range for a double\n";
		//throw;
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.TXhold", tmp);
	return tmp;
}

string SDRunoPlugin_TXLinkForm::GetValidFanHold()
{
	string tmp = "";
	int Val = 450;
	m_controller.GetConfigurationKey("TXLink.FanHold", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "3";								//default
		Val = 3;
	}
	try {
		Val = stoi(tmp);
	}
	catch (const std::invalid_argument&) {
		Val = 3;
		tmp = "3";
		//std::cerr << "Argument is invalid\n";
		//throw;
	}
	catch (const std::out_of_range&) {
		Val = 3;
		tmp = "3";
		//std::cerr << "Argument is out of range for a double\n";
		//throw;
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.FanHold", tmp);
	return tmp;
}

string SDRunoPlugin_TXLinkForm::GetValidFanLowPWM()
{
	string tmp = "";
	int Val = 175;
	m_controller.GetConfigurationKey("TXLink.FanLowPWM", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "175";								//default
		Val = 175;
	}
	try {
		Val = stoi(tmp);
	}
	catch (const std::invalid_argument&) {
		Val = 175;
		tmp = "175";
		//std::cerr << "Argument is invalid\n";
		//throw;
	}
	catch (const std::out_of_range&) {
		Val = 175;
		tmp = "175";
		//std::cerr << "Argument is out of range for a double\n";
		//throw;
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.FanLowPWM", tmp);
	return tmp;
}

string SDRunoPlugin_TXLinkForm::GetValidFT8txDelay()
{
	string tmp = "";
	int Val = 250;
	m_controller.GetConfigurationKey("TXLink.FT8txDelay", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "250";								//default
		Val = 250;
	}
	try {
		Val = stoi(tmp);
		if (Val < 0 || Val >= 3000)
		{
			Val = 250;
			tmp = "250";
		}
	}
	catch (int errNum) {
		if (errNum) {};
		Val = 250;
		tmp = "250";
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.FT8txDelay", tmp);
	return tmp;
}

string SDRunoPlugin_TXLinkForm::GetValidRTCdelayOffset()
{
	string tmp = "";
	int Val = 250;
	m_controller.GetConfigurationKey("TXLink.RTCdelayOffset", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "300";								//default
		Val = 300;
	}
	try {
		Val = stoi(tmp);
		if (Val < 0 || Val >= 1000)
		{
			Val = 300;
			tmp = "300";
		}
	}
	catch (int errNum) {
		if (errNum) {};
		Val = 300;
		tmp = "300";
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.RTCdelayOffset", tmp);
	return tmp;
}

string SDRunoPlugin_TXLinkForm::GetValidRTCcurrentDelay()
{
	string tmp = "";
	int Val = 250;
	m_controller.GetConfigurationKey("TXLink.RTCcurrentDelay", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "300";								//default
		Val = 300;
	}
	try {
		Val = stoi(tmp);
		if (Val < 0 || Val >= 1000)
		{
			Val = 300;
			tmp = "300";
		}
	}
	catch (int errNum) {
		if (errNum) {};
		Val = 300;
		tmp = "300";
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.RTCcurrentDelay", tmp);
	return tmp;
}


string SDRunoPlugin_TXLinkForm::GetValidTempLimit()
{
	string tmp = "";
	int Val = 54;
	m_controller.GetConfigurationKey("TXLink.TempLimit", tmp);			//stored by settings panel
	if (tmp.empty()) {
		tmp = "54";								//default
		Val = 54;
	}
	try {
		Val = stoi(tmp);
		if (Val < 0 || Val >= 75)
		{
			Val = 54;
			tmp = "54";
		}
	}
	catch (int errNum) {
		if (errNum) {};
		Val = 54;
		tmp = "54";
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_controller.SetConfigurationKey("TXLink.TempLimit", tmp);
	return tmp;
}

bool SDRunoPlugin_TXLinkForm::GetRITchkbox()
{
	return RITchkbox.checked();
}

bool SDRunoPlugin_TXLinkForm::GetOFSchkbox()
{
	return OFSchkbox.checked();
}

void SDRunoPlugin_TXLinkForm::SettingsButton_Click()
{
	//Create a new settings dialog object
	SDRunoPlugin_TemplateSettingsDialog settingsDialog{ m_parent,m_controller };

	//disable this form so settings dialog retains top level focus
	this->enabled(false);

	//Attach a handler to the settings dialog close event
	settingsDialog.events().unload([&] { SettingsDialog_Closed(); });

	//Show the setttings dialog
	settingsDialog.Run();
}

void SDRunoPlugin_TXLinkForm::SettingsDialog_Closed()
{
	//DO NOT REMOVE THE FLLOWING CODE it is required for the proper operation of the settings dialog form

	this->enabled(true);
	this->focus();

	//TODO: Extra code goes here to be preformed when settings dialog form closes
	SendFreqCalToTX();
	SendTXhold(GetValidTXhold());
	SendFanHold(GetValidFanHold());
	SendFanLowPWM(GetValidFanLowPWM());
	SendFT8txDelay(GetValidFT8txDelay());
	RTCdelayOffset = stoi(GetValidRTCdelayOffset());
	RTCdelay = RTCcurrentDelay - RTCdelayOffset;
	RTCdelay < 50 ? RTCdelay = 50 : RTCdelay;
	SendTempLimit(GetValidTempLimit());
}


// Load X from the ini file (if exists)
// TODO: Change Template to plugin name
int SDRunoPlugin_TXLinkForm::LoadX()
{
	std::string tmp;
	m_controller.GetConfigurationKey("TXLink.X", tmp);
	if (tmp.empty())
	{
		return -1;
	}
	return stoi(tmp);
}

// Load Y from the ini file (if exists)
// TODO: Change Template to plugin name
int SDRunoPlugin_TXLinkForm::LoadY()
{
	std::string tmp;
	m_controller.GetConfigurationKey("TXLink.Y", tmp);
	if (tmp.empty())
	{
		return -1;
	}
	return stoi(tmp);
}

void SDRunoPlugin_TXLinkForm::SaveLocation()
{
	nana::point position = pos();
	if (position.x >= 0 && position.y >= 0)
	{
		m_controller.SetConfigurationKey("TXLink.X", std::to_string(position.x));
		m_controller.SetConfigurationKey("TXLink.Y", std::to_string(position.y));
	}

}

//Load TXlevel
string SDRunoPlugin_TXLinkForm::LoadTXlevel()
{
	std::string tmp = "";
	m_controller.GetConfigurationKey("TXLink.TXlevel", tmp);
	if (tmp.empty())
	{
		return "0";
	}
	return tmp;
}

//Load PAlevel
string SDRunoPlugin_TXLinkForm::LoadPAlevel()
{
	std::string tmp = "";
	m_controller.GetConfigurationKey("TXLink.PAlevel", tmp);
	if (tmp.empty())
	{
		return "0";
	}
	return tmp;
}

void SDRunoPlugin_TXLinkForm::SaveTXlevel(string strTXlevel)
{
	m_controller.SetConfigurationKey("TXLink.TXlevel", strTXlevel);
}

void SDRunoPlugin_TXLinkForm::SavePAlevel(string strPAlevel)
{
	m_controller.SetConfigurationKey("TXLink.PAlevel", strPAlevel);
}

string SDRunoPlugin_TXLinkForm::LoadRIT()
{
	std::string tmp = "";
	m_controller.GetConfigurationKey("TXLink.RIT", tmp);
	if (tmp.empty())
	{
		return "false";
	}
	return tmp;
}

string SDRunoPlugin_TXLinkForm::LoadOFS()
{
	std::string tmp = "";
	m_controller.GetConfigurationKey("TXLink.OFS", tmp);
	if (tmp.empty())
	{
		return "false";
	}
	return tmp;
}

void SDRunoPlugin_TXLinkForm::LoadRTCdelay()
{
	RTCdelayOffset = stoi(GetValidRTCdelayOffset());
	RTCcurrentDelay = stoi(GetValidRTCcurrentDelay());
	RTCdelay = RTCcurrentDelay - RTCdelayOffset;
	RTCdelay < 50 ? RTCdelay = 50 : RTCdelay;
}

void SDRunoPlugin_TXLinkForm::SaveRTCdelay()
{
	m_controller.SetConfigurationKey("TXLink.RTCdelayOffset", to_string(RTCdelayOffset));
	m_controller.SetConfigurationKey("TXLink.RTCcurrentDelay", to_string(RTCcurrentDelay));
}

void SDRunoPlugin_TXLinkForm::SaveRIT(string strRIT)
{
	m_controller.SetConfigurationKey("TXLink.RIT", strRIT);
}

void SDRunoPlugin_TXLinkForm::SaveOFS(string strOFS)
{
	m_controller.SetConfigurationKey("TXLink.OFS", strOFS);
}

void SDRunoPlugin_TXLinkForm::LoadSplitMode()
{
	std::string tmp = "";
	m_controller.GetConfigurationKey("TXLink.SplitMode", tmp);
	if (tmp.empty())
	{
		tmp = "0";
	}
	if (tmp == "0")
	{
		std::lock_guard<std::mutex> lock(g_splitModeMutex);
		SplitMode = SplitNone;				//no split
	}
	else if (tmp == "1")
	{
		std::lock_guard<std::mutex> lock(g_splitModeMutex);
		SplitMode = SplitTxRx;				//TX/RX split
	}
	else if (tmp == "2")
	{
		std::lock_guard<std::mutex> lock(g_splitModeMutex);
		SplitMode = SplitFT8;				//FT8 split
	}
}

void SDRunoPlugin_TXLinkForm::SaveSplitMode()
{
	m_controller.SetConfigurationKey("TXLink.SplitMode", std::to_string(SplitMode));
}


void SDRunoPlugin_TXLinkForm::SetTXtemp(string strTemp)
{
	strTXtemp = strTemp;
}

string SDRunoPlugin_TXLinkForm::GetTXtemp()
{
	return strTXtemp;
}

void SDRunoPlugin_TXLinkForm::SetStandbyModeClicked()
{
	SetStandbyMode(StandbyMode);
}

void SDRunoPlugin_TXLinkForm::FixFreqButtonClicked()
{
	if (TrackFreq)
	{
		SendRXfreq();
	}
}

void SDRunoPlugin_TXLinkForm::SaveIniData()
{
	std::cout << get_millis() << " SDRunoPlugin_TXLinkUi::SaveIniData called" << std::endl;
	//*
	SaveLocation();
	SaveCOM(GetCOM());
	SaveTXlevel(GetTXlevel());
	SavePAlevel(GetPAlevel());
	SaveRIT(GetRIT());
	SaveOFS(GetOFS());
	SaveSplitMode();
	SaveRTCdelay();
	//*/
	std::cout << get_millis() << " SDRunoPlugin_TXLinkUi::SaveIniData complete" << std::endl;
}

void SDRunoPlugin_TXLinkForm::InitiateFormUnload()		//called from top level TXLink class
{
	//std::cout << "SDRunoPlugin_TXLinkUi::InitiateFormUnload - stopTimer" << std::endl;
	//StopTimer();		//stop further processing in form

	std::cout << get_millis() << " SDRunoPlugin_TXLinkUi::InitiateFormUnload - SetStandbyMode" << std::endl;
	SetStandbyMode(true);			//send TXto standby mode while COM port is still open.

	//shutdowm other Form resources
	//std::cout << get_millis() << "SDRunoPlugin_TXLinkUi::InitiateFormUnload - DXspiderDisconnect" << std::endl;
	//DXspiderDisconnect();

	//std::cout << "SDRunoPlugin_TXLinkUi::InitiateFormUnload - CloseCOMPort" << std::endl;
	//CloseCOMPort();		

	std::cout << get_millis() << "SDRunoPlugin_TXLinkUi::InitiateFormUnload - InitiateFormUnload complete" << std::endl;
}
#include <sstream>

#include "SDRunoPlugin_TXLinkForm.h"
#include "DXspiderClient.h"


void SDRunoPlugin_TXLinkForm::DXspotBtnClicked()
{
	// Derive a DXspot message and send to DXspider
	// spot message format: "DX <frequency> <dx_call> <notes>"
	std::cout << get_millis() << "DXspotBtnClicked: Request DXspot" << std::endl;

	std::string strDXspot = "";
	if (CurrentWSJTXfreq > 0)
	{
		strDXspot += "dx " + std::to_string((int64_t)CurrentWSJTXfreq / 1000) + ".0";
	}
	else
	{
		std::cout << get_millis() << "DXspotBtnClicked: No TXfreq" << "\n";
		return;
	}

	if (m_lastUDPStatus.dxCall != "")
	{
		strDXspot += " " + m_lastUDPStatus.dxCall;

		//remove "<" and ">" from DXcall
		char charToRemove = '<';
		strDXspot.erase(std::remove(strDXspot.begin(), strDXspot.end(), charToRemove), strDXspot.end());
		charToRemove = '>';
		strDXspot.erase(std::remove(strDXspot.begin(), strDXspot.end(), charToRemove), strDXspot.end());
	}
	else
	{
		std::cout << get_millis() << "DXspotBtnClicked: No DXcall" << std::endl;
		return;
	}

	if (DigiTXmodeStr != "")
	{
		strDXspot += " " + DigiTXmodeStr;
	}
	else
	{
		std::cout << get_millis() << "DXspotBtnClicked: No DigiTXmodeStr" << "\n";
		return;
	}

	if (m_lastUDPStatus.report != "")
	{
		strDXspot += " " + m_lastUDPStatus.report + "dB";
	}
	else
	{
		std::cout << get_millis() << "DXspotBtnClicked: No Report" << "\n";
		//return;
	}

	if (m_lastUDPStatus.rxDF != 0)
	{
		strDXspot += " " + std::to_string(m_lastUDPStatus.rxDF) + "Hz";
	}
	else
	{
		std::cout << get_millis() << "DXspotBtnClicked: No RxDF" << "\n";
		return;
	}

	if (m_lastUDPStatus.dxGrid != "")
	{
		strDXspot += " Loc: " + m_lastUDPStatus.dxGrid;
	}
	else
	{
		std::cout << get_millis() << "DXspotBtnClicked: No DXgrid" << "\n";
	}

	strDXspot += " 73GL";

	std::cout << get_millis() << "DXspotBtnClicked: DXspot created: " << strDXspot << "\n";
	//DXspiderSpot("G4AHN-2", strDXspot);

	DXspot_t spot;
	spot.dxSpiderPort = GetSetConfigurationKey("TXLink.dxSpiderPort", "dxspider.co.uk:7300");			//"dxspider.co.uk:7300";
	spot.myCall = GetSetConfigurationKey("TXLink.myDXspotCall", "G4AHN-1");			//
	spot.DXspot = strDXspot;
	DXspiderSpotOutQ.push(spot);			//put spot on DXspider queue to send
}

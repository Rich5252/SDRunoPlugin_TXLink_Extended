#pragma once

#include <string>
#include <windows.h>

//---------------------------------------------------------------------------
// QRZLookup
//
// Performs a QRZ XML API lookup for a given callsign and extracts the
// Maidenhead grid locator, placing it on the Windows clipboard.
//
// Authentication uses credentials stored in Windows Credential Manager
// under the target name "MyCWLogger_QRZ_Auth" (matching the C# app).
//
// Session key is cached between calls and re-acquired automatically if
// it expires (QRZ returns a "Session Timeout" error).
//
// Threading: intended to be called from the detached QRZ browser-launch
// thread in WSJTX_Status_Processor, so it runs off the UDP thread and
// never blocks the Form or serial paths.
//
// Usage:
//   static QRZLookup qrz;      // one instance, persists session key
//   qrz.LookupAndCopyGrid("G4AHN");
//---------------------------------------------------------------------------
class QRZLookup
{
public:
    QRZLookup() = default;

    // Look up callsign via QRZ XML API, extract grid locator,
    // and place it on the Windows clipboard. Logs to std::cout.
    // fallbackGrid: WSJT-X's own grid for this station - used if QRZ
    // returns nothing or a result that disagrees on the first 4 chars.
    // Returns true if a grid was found and copied successfully.
    bool LookupAndCopyGrid(const std::string& callsign,
        const std::string& fallbackGrid = "");

private:
    std::string m_sessionKey;   // cached - re-acquired on expiry

    // Retrieve username/password from Windows Credential Manager.
    // Target name must match what the C# app stored: "MyCWLogger_QRZ_Auth"
    bool GetCredentials(std::string& username, std::string& password);

    // Perform an HTTPS GET to the QRZ XML endpoint and return the raw body.
    std::string HttpGet(const std::string& path);

    // Acquire a fresh session key using username/password.
    bool AcquireSessionKey(const std::string& username,
        const std::string& password);

    // Extract a named XML element value: <tag>value</tag>
    // Returns empty string if not found.
    std::string ExtractXmlValue(const std::string& xml,
        const std::string& tag);

    // Place text on the Windows clipboard.
    bool CopyToClipboard(const std::string& text);

    // Validate and normalise a Maidenhead grid locator.
    // Converts to uppercase, checks format: 2 letters + 2 digits + 2 letters.
    // Returns the uppercased grid if valid, empty string if not.
    // Accepts both 4-char (field+square) and 6-char (full) locators.
    std::string ValidateGrid(const std::string& grid);
};
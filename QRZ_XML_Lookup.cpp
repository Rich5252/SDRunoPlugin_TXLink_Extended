#include <iostream>
#include <string>
#include <windows.h>
#include <wincred.h>
#include <winhttp.h>
#include <sstream>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "winhttp.lib")

#include "QRZ_XML_Lookup.h"
#include "MessageBox.h"     // get_millis()

// QRZ XML API endpoint
static const wchar_t* QRZ_HOST = L"xmldata.qrz.com";
static const int      QRZ_PORT = 443;           // HTTPS
static const wchar_t* QRZ_PATH = L"/xml/current/";
static const char* CRED_TARGET = "MyCWLogger_QRZ_Auth";

//-----------------------------------------------------------------------------
// GetCredentials
// Reads username and password from Windows Credential Manager.
// Target name matches what the C# app stored.
//-----------------------------------------------------------------------------
bool QRZLookup::GetCredentials(std::string& username, std::string& password)
{
    PCREDENTIALA pCred = nullptr;
    if (!CredReadA(CRED_TARGET, CRED_TYPE_GENERIC, 0, &pCred))
    {
        std::cout << get_millis()
            << " QRZLookup: CredRead failed for target '"
            << CRED_TARGET << "' - error " << GetLastError()
            << ". Check Windows Credential Manager." << std::endl;
        return false;
    }

    username = pCred->UserName ? pCred->UserName : "";
    if (pCred->CredentialBlobSize > 0)
    {
        const wchar_t* wPass = reinterpret_cast<const wchar_t*>(pCred->CredentialBlob);
        int wLen = pCred->CredentialBlobSize / sizeof(wchar_t);
        int narrowLen = WideCharToMultiByte(CP_UTF8, 0, wPass, wLen,
            nullptr, 0, nullptr, nullptr);
        if (narrowLen > 0)
        {
            password.resize(narrowLen);
            WideCharToMultiByte(CP_UTF8, 0, wPass, wLen,
                &password[0], narrowLen, nullptr, nullptr);
        }
    }

    CredFree(pCred);

    if (username.empty() || password.empty())
    {
        std::cout << get_millis()
            << " QRZLookup: Credential target found but username/password empty."
            << std::endl;
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// HttpGet
// Performs an HTTPS GET to xmldata.qrz.com with the given path+query string.
// Returns the response body, or empty string on failure.
//-----------------------------------------------------------------------------
std::string QRZLookup::HttpGet(const std::string& query)
{
    // Build the full path: /xml/current/?<query>
    std::string fullPath = std::string("/xml/current/?") + query;
    std::wstring wPath(fullPath.begin(), fullPath.end());

    HINTERNET hSession = WinHttpOpen(
        L"TXLink QRZ Lookup/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession)
    {
        std::cout << get_millis()
            << " QRZLookup: WinHttpOpen failed - error " << GetLastError()
            << std::endl;
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(
        hSession, QRZ_HOST, QRZ_PORT, 0);
    if (!hConnect)
    {
        std::cout << get_millis()
            << " QRZLookup: WinHttpConnect failed - error " << GetLastError()
            << std::endl;
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        wPath.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);   // HTTPS
    if (!hRequest)
    {
        std::cout << get_millis()
            << " QRZLookup: WinHttpOpenRequest failed - error " << GetLastError()
            << std::endl;
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Send the request
    if (!WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        std::cout << get_millis()
            << " QRZLookup: WinHttpSendRequest failed - error " << GetLastError()
            << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    if (!WinHttpReceiveResponse(hRequest, NULL))
    {
        std::cout << get_millis()
            << " QRZLookup: WinHttpReceiveResponse failed - error " << GetLastError()
            << std::endl;
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // Read the response body
    std::string body;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead))
        {
            body.append(chunk, 0, bytesRead);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return body;
}

//-----------------------------------------------------------------------------
// AcquireSessionKey
// Authenticates with QRZ XML API and caches the session key.
//-----------------------------------------------------------------------------
bool QRZLookup::AcquireSessionKey(const std::string& username,
    const std::string& password)
{
    std::string query = "username=" + username
        + ";password=" + password
        + ";agent=TXLinkPlugin";

    std::string response = HttpGet(query);
    if (response.empty())
    {
        std::cout << get_millis()
            << " QRZLookup: No response from QRZ authentication."
            << std::endl;
        return false;
    }

    // Check for error in response
    std::string error = ExtractXmlValue(response, "Error");
    if (!error.empty())
    {
        std::cout << get_millis()
            << " QRZLookup: Authentication error from QRZ: " << error
            << std::endl;
        return false;
    }

    m_sessionKey = ExtractXmlValue(response, "Key");
    if (m_sessionKey.empty())
    {
        std::cout << get_millis()
            << " QRZLookup: No session key in QRZ response." << std::endl;
        return false;
    }

    std::cout << get_millis()
        << " QRZLookup: Session key acquired successfully." << std::endl;
    return true;
}

//-----------------------------------------------------------------------------
// ExtractXmlValue
// Minimal XML extraction: finds the first <tag>value</tag> and returns value.
// No dependency on an XML library - QRZ's response is simple enough.
//-----------------------------------------------------------------------------
std::string QRZLookup::ExtractXmlValue(const std::string& xml,
    const std::string& tag)
{
    std::string openTag = "<" + tag + ">";
    std::string closeTag = "</" + tag + ">";

    size_t start = xml.find(openTag);
    if (start == std::string::npos) return "";
    start += openTag.length();

    size_t end = xml.find(closeTag, start);
    if (end == std::string::npos) return "";

    return xml.substr(start, end - start);
}

//-----------------------------------------------------------------------------
// CopyToClipboard
// Places a UTF-8 string onto the Windows clipboard as CF_TEXT.
//-----------------------------------------------------------------------------
bool QRZLookup::CopyToClipboard(const std::string& text)
{
    if (!OpenClipboard(nullptr))
    {
        std::cout << get_millis()
            << " QRZLookup: OpenClipboard failed - error " << GetLastError()
            << std::endl;
        return false;
    }

    EmptyClipboard();

    // Allocate global memory for the text (+1 for null terminator)
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hMem)
    {
        CloseClipboard();
        std::cout << get_millis()
            << " QRZLookup: GlobalAlloc failed." << std::endl;
        return false;
    }

    memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_TEXT, hMem))
    {
        GlobalFree(hMem);
        CloseClipboard();
        std::cout << get_millis()
            << " QRZLookup: SetClipboardData failed - error " << GetLastError()
            << std::endl;
        return false;
    }

    CloseClipboard();
    // Note: do NOT GlobalFree(hMem) after a successful SetClipboardData -
    // the clipboard takes ownership of the handle.
    return true;
}

//-----------------------------------------------------------------------------
// ValidateGrid
// Validates a Maidenhead grid locator and returns it uppercased.
// Accepts 4-char (field+square only) or 6-char (full with subsquare).
// Format: 2 letters + 2 digits [+ 2 letters]
// Returns empty string if format is invalid.
//-----------------------------------------------------------------------------
std::string QRZLookup::ValidateGrid(const std::string& grid)
{
    if (grid.empty()) return "";

    // Uppercase a copy for comparison and output
    std::string g = grid;
    for (char& c : g) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

    // Must be exactly 4 or 6 characters
    if (g.size() != 4 && g.size() != 6) return "";

    // Positions 0,1 must be letters (field)
    if (!isalpha(static_cast<unsigned char>(g[0]))) return "";
    if (!isalpha(static_cast<unsigned char>(g[1]))) return "";

    // Positions 2,3 must be digits (square)
    if (!isdigit(static_cast<unsigned char>(g[2]))) return "";
    if (!isdigit(static_cast<unsigned char>(g[3]))) return "";

    // If 6 chars, positions 4,5 must be letters (subsquare)
    if (g.size() == 6)
    {
        if (!isalpha(static_cast<unsigned char>(g[4]))) return "";
        if (!isalpha(static_cast<unsigned char>(g[5]))) return "";
    }

    return g;   // valid - return uppercased
}

//-----------------------------------------------------------------------------
// LookupAndCopyGrid
// Main entry point. Looks up callsign, validates grid against fallback,
// copies best result to clipboard.
//-----------------------------------------------------------------------------
bool QRZLookup::LookupAndCopyGrid(const std::string& callsign,
    const std::string& fallbackGrid)
{
    std::cout << get_millis()
        << " QRZLookup: Looking up grid for " << callsign
        << (fallbackGrid.empty() ? "" : " (WSJTX fallback: " + fallbackGrid + ")")
        << std::endl;

    // Validate and normalise the WSJT-X fallback grid upfront
    std::string validFallback = ValidateGrid(fallbackGrid);

    // Get credentials from Windows Credential Manager
    std::string username, password;
    if (!GetCredentials(username, password))
        return false;

    // Acquire session key if we don't have one
    if (m_sessionKey.empty())
    {
        if (!AcquireSessionKey(username, password))
            return false;
    }

    // Perform the callsign lookup
    auto doLookup = [this, &callsign]() -> std::string
        {
            std::string query = "s=" + m_sessionKey + ";callsign=" + callsign;
            return HttpGet(query);
        };

    std::string response = doLookup();

    // If session expired, re-authenticate once and retry
    if (response.find("Session Timeout") != std::string::npos ||
        response.find("Invalid session key") != std::string::npos ||
        ExtractXmlValue(response, "Error").find("session") != std::string::npos)
    {
        std::cout << get_millis()
            << " QRZLookup: Session expired, re-authenticating..." << std::endl;
        m_sessionKey.clear();
        if (!AcquireSessionKey(username, password))
            return false;
        response = doLookup();
    }

    // --- Grid selection logic ---
    // 1. Validate QRZ result
    // 2. If valid AND first 4 chars match fallback -> use QRZ (more precise)
    // 3. Otherwise use fallback if it's valid
    // 4. Log and abort if neither is usable

    std::string qrzGrid;
    if (!response.empty())
    {
        std::string error = ExtractXmlValue(response, "Error");
        if (!error.empty())
        {
            std::cout << get_millis()
                << " QRZLookup: QRZ error for " << callsign
                << ": " << error << std::endl;
        }
        else
        {
            std::string raw = ExtractXmlValue(response, "grid");
            // Trim whitespace
            raw.erase(0, raw.find_first_not_of(" \t\r\n"));
            if (!raw.empty())
                raw.erase(raw.find_last_not_of(" \t\r\n") + 1);

            qrzGrid = ValidateGrid(raw);
            if (qrzGrid.empty())
            {
                std::cout << get_millis()
                    << " QRZLookup: QRZ grid '" << raw
                    << "' failed format validation for " << callsign
                    << std::endl;
            }
        }
    }
    else
    {
        std::cout << get_millis()
            << " QRZLookup: Empty response for " << callsign << std::endl;
    }

    // Determine which grid to use
    std::string finalGrid;

    if (!qrzGrid.empty() && !validFallback.empty())
    {
        // Both available - check first 4 chars agree
        if (qrzGrid.substr(0, 4) == validFallback.substr(0, 4))
        {
            // QRZ agrees with WSJT-X and is likely more precise (6 chars)
            finalGrid = qrzGrid;
            std::cout << get_millis()
                << " QRZLookup: Using QRZ grid " << finalGrid
                << " (agrees with WSJT-X " << validFallback << ")"
                << std::endl;
        }
        else
        {
            // QRZ disagrees - trust WSJT-X
            finalGrid = validFallback;
            std::cout << get_millis()
                << " QRZLookup: QRZ grid " << qrzGrid
                << " disagrees with WSJT-X " << validFallback
                << " on first 4 chars - using WSJT-X fallback" << std::endl;
        }
    }
    else if (!qrzGrid.empty())
    {
        // No fallback to compare against - use QRZ result directly
        finalGrid = qrzGrid;
        std::cout << get_millis()
            << " QRZLookup: Using QRZ grid " << finalGrid
            << " (no WSJT-X fallback to compare)" << std::endl;
    }
    else if (!validFallback.empty())
    {
        // QRZ gave nothing useful - fall back to WSJT-X
        finalGrid = validFallback;
        std::cout << get_millis()
            << " QRZLookup: QRZ lookup failed, using WSJT-X fallback "
            << finalGrid << std::endl;
    }
    else
    {
        std::cout << get_millis()
            << " QRZLookup: No valid grid available for "
            << callsign << " - clipboard not updated" << std::endl;
        return false;
    }

    if (!CopyToClipboard(finalGrid))
        return false;

    std::cout << get_millis()
        << " QRZLookup: Grid '" << finalGrid
        << "' copied to clipboard for " << callsign << std::endl;
    return true;
}
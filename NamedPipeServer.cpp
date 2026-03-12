#include <iostream>
#include <thread> // Added for std::this_thread::sleep_for
#include <chrono> // Added for std::chrono::milliseconds
#include <stdexcept>
#include <vector> // Add missing include for std::vector
#include <codecvt>

#include "NamedPipeServer.h"
#include "MessageBox.h"

// --- Class Method Implementations (Partial, assuming remaining methods are defined) ---

// NOTE: The full implementations of NamedPipeServer methods are kept here 
// for self-containment, but only createAndListen is shown in detail for clarity.

NamedPipeServer::NamedPipeServer(const std::string& pipeName, size_t bufferSize)
    : pipeName_(pipeName), bufferSize_(bufferSize), hPipe_(INVALID_HANDLE_VALUE),
    connected_(false), hConnectEvent_(NULL) {

    // Initialize overlapped structure
    ZeroMemory(&connectOverlap_, sizeof(OVERLAPPED));
}

NamedPipeServer::~NamedPipeServer() {
    close();
}

/**
 * @brief Creates the pipe and blocks until a client connects, or until the timeout elapses.
 * @param timeoutMs Timeout in milliseconds for the connection attempt. Defaults to infinite wait.
 * @return true if successful, false otherwise.
 */
bool NamedPipeServer::createAndListen(DWORD timeoutMs) {
    std::cout << get_millis() << "Creating pipe: " << pipeName_ << " with connection timeout: ";
    if (timeoutMs == NMPWAIT_WAIT_FOREVER) {
        std::cout << "Infinite" << std::endl;
    }
    else {
        std::cout << timeoutMs << "ms" << std::endl;
    }

    // Create an event handle for overlapped operations
    HANDLE hConnectEvent = CreateEvent(
        NULL,    // Default security attributes
        TRUE,    // Manual reset event
        FALSE,   // Initial state is non-signaled
        NULL);   // No name

    if (hConnectEvent == NULL) {
        std::cerr << get_millis() << "ERROR: Failed to create connection event. Code: " << GetLastError() << std::endl;
        return false;
    }

    OVERLAPPED overlap = { 0 };
    overlap.hEvent = hConnectEvent;

    // 1. Create the Named Pipe
    // We use FILE_FLAG_OVERLAPPED instead of PIPE_WAIT to enable asynchronous I/O.
    hPipe_ = CreateNamedPipeA(
        pipeName_.c_str(),            // Pipe name
        PIPE_ACCESS_DUPLEX |          // Read and write access
        FILE_FLAG_OVERLAPPED,         // *** USE OVERLAPPED I/O ***
        PIPE_TYPE_MESSAGE |           // Message type pipe
        PIPE_READMODE_MESSAGE,        // Message-read mode
        PIPE_UNLIMITED_INSTANCES,     // Max instances
        bufferSize_,                  // Output buffer size
        bufferSize_,                  // Input buffer size
        0,                            // Default timeout is irrelevant with overlapped I/O
        NULL);                        // Default security attributes

    if (hPipe_ == INVALID_HANDLE_VALUE) {
        std::cerr << get_millis() << "ERROR: Failed to create pipe. Code: " << GetLastError() << std::endl;
        CloseHandle(hConnectEvent);
        return false;
    }

    std::cout << get_millis() << "Pipe created. Waiting for VB6 client connection..." << std::endl;

    // 2. Start the asynchronous connection attempt
    // This call is guaranteed to return immediately.
    BOOL fConnected = ConnectNamedPipe(hPipe_, &overlap);

    if (fConnected) {
        // This case should not happen for a newly created pipe using overlapped, 
        // but if it does, it means a client connected instantly.
        std::cout << get_millis() << "SUCCESS: VB6 Client Connected instantly." << std::endl;
        connected_ = true;
        CloseHandle(hConnectEvent);
        return true;
    }

    DWORD dwError = GetLastError();
    if (dwError == ERROR_IO_PENDING) {
        // Connection is pending. Now we wait with the timeout.

        // 3. Wait for the connection event
        DWORD dwWaitResult = WaitForSingleObject(hConnectEvent, timeoutMs);

        if (dwWaitResult == WAIT_OBJECT_0) {
            // The connection succeeded!
            DWORD cbTransfer;
            // GetOverlappedResult confirms the I/O operation (connection) completed.
            if (GetOverlappedResult(hPipe_, &overlap, &cbTransfer, FALSE)) {
                connected_ = true;
                std::cout << get_millis() << "SUCCESS: VB6 Client Connected." << std::endl;
                CloseHandle(hConnectEvent);
                return true;
            }
            else {
                std::cerr << get_millis() << "ERROR: GetOverlappedResult failed after event signaled. Code: " << GetLastError() << std::endl;
            }
        }
        else if (dwWaitResult == WAIT_TIMEOUT) {
            // The timeout expired.
            std::cerr << get_millis() << "ERROR: Client connection timed out (" << timeoutMs << "ms)." << std::endl;
            // Cancel the pending connection and clean up.
            CancelIo(hPipe_);
        }
        else {
            // Other wait error.
            std::cerr << get_millis() << "ERROR: WaitForSingleObject failed. Code: " << dwWaitResult << std::endl;
        }

    }
    else if (dwError != ERROR_PIPE_CONNECTED) {
        // Fatal error during ConnectNamedPipe
        std::cerr << get_millis() << "ERROR: ConnectNamedPipe failed immediately. Code: " << dwError << std::endl;
    }

    // Cleanup on failure/timeout
    CloseHandle(hConnectEvent);
    CloseHandle(hPipe_);
    hPipe_ = INVALID_HANDLE_VALUE;
    connected_ = false;
    return false;
}

/**
 * @brief Reads a single message from the connected client with an optional timeout.
 * @param timeoutMs Timeout in milliseconds. Use 0 for no timeout (infinite wait).
 * @return The received message as a std::string. Empty string on timeout.
 * @throws std::runtime_error on failure or disconnection.
 */
std::string NamedPipeServer::readMessage(DWORD timeoutMs) {
    if (!connected_ || hPipe_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Pipe is not connected or handle is invalid.");
    }

    // Use a dedicated event handle for the read operation
    HANDLE hReadEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (hReadEvent == NULL) {
        throw std::runtime_error("Failed to create read event.");
    }

    OVERLAPPED overlap = { 0 };
    overlap.hEvent = hReadEvent;

    // Use a vector to act as the byte buffer
    std::vector<char> buffer(bufferSize_);
    DWORD cbRead = 0;

    // 1. Start the asynchronous read operation
    // ReadFile will immediately return false with ERROR_IO_PENDING because of FILE_FLAG_OVERLAPPED.
    BOOL fSuccess = ReadFile(
        hPipe_,
        buffer.data(),
        bufferSize_ - 1, // Read up to bufferSize-1 to ensure room for null terminator
        &cbRead,
        &overlap); // Pass OVERLAPPED structure

    // 2. Check read status
    if (!fSuccess) {
        DWORD dwError = GetLastError();
        if (dwError == ERROR_IO_PENDING) {
            // Read is pending. Wait for the result with the specified timeout.
            DWORD dwWaitResult = WaitForSingleObject(hReadEvent, timeoutMs);

            if (dwWaitResult == WAIT_OBJECT_0) {
                // Read operation completed within the timeout.
                // GetOverlappedResult confirms the read and gets the byte count.
                if (!GetOverlappedResult(hPipe_, &overlap, &cbRead, FALSE)) {
                    // Check specifically for broken pipe during result check
                    if (GetLastError() == ERROR_BROKEN_PIPE) {
                        connected_ = false;
                        CloseHandle(hReadEvent);
                        throw std::runtime_error("Client disconnected (Broken Pipe).");
                    }
                    CloseHandle(hReadEvent);
                    throw std::runtime_error("GetOverlappedResult failed during read. Code: " + std::to_string(GetLastError()));
                }
            }
            else if (dwWaitResult == WAIT_TIMEOUT) {
                // Timeout occurred. Cancel the pending read operation.
                CancelIo(hPipe_);
                CloseHandle(hReadEvent);
                return ""; // Return empty string on timeout
            }
            else {
                // WaitForSingleObject failed.
                CloseHandle(hReadEvent);
                throw std::runtime_error("WaitForSingleObject failed during read wait. Code: " + std::to_string(dwWaitResult));
            }
        }
        else if (dwError == ERROR_BROKEN_PIPE) {
            connected_ = false;
            CloseHandle(hReadEvent);
            throw std::runtime_error("Client disconnected (Broken Pipe).");
        }
        else {
            // Other ReadFile error
            CloseHandle(hReadEvent);
            throw std::runtime_error("Error initiating read from pipe. Code: " + std::to_string(dwError));
        }
    }
    // If fSuccess was true here, it means the read completed instantly (rare for a pipe read).

    // 3. Process successful read

    // Copy the exact number of bytes read into a std::string, filtering out null bytes.
    std::string receivedData;
    receivedData.reserve(cbRead);

    // *** FIX: Filter out null bytes (0x00) to clean up wide-character streams ***
    for (DWORD i = 0; i < cbRead; ++i) {
        if (buffer[i] != '\0') {
            receivedData += buffer[i];
        }
    }

    CloseHandle(hReadEvent);
    return receivedData;
}

bool NamedPipeServer::writeMessage(const std::string& message) {
    if (!connected_ || hPipe_ == INVALID_HANDLE_VALUE) {
        std::cerr << get_millis() << "WARNING: Cannot write, pipe is not connected." << std::endl;
        return false;
    }

    // --- 1. CONVERT 8-bit std::string (UTF-8/ASCII) to 16-bit std::wstring (UTF-16/Unicode) ---
    // Use the standard C++ converter for wide strings.
    // NOTE: std::codecvt_utf8 is deprecated in C++17, but is widely used in older projects.
#if __cplusplus < 201703L
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wdataToWrite = converter.from_bytes(message);
#else
// For C++17 and later, this conversion requires more modern (or OS-specific) methods.
// For simplicity and commonality in older engines, we assume pre-C++17 or platform conversion utilities.
    std::wstring wdataToWrite(message.begin(), message.end()); // Simple ASCII/char to wchar_t conversion
#endif

    // 2. Append a WIDE null terminator (L'\0') for robustness on the client side.
    wdataToWrite += L'\0';

    // 3. Calculate the number of BYTES to write. Each wchar_t is 2 bytes.
    DWORD cbToWrite = wdataToWrite.length() * sizeof(wchar_t);
    DWORD cbWritten = 0;

    // --- 4. Write the 16-bit data ---
    BOOL fSuccess = WriteFile(
        hPipe_,
        wdataToWrite.c_str(), // Use c_str() to get the wide character pointer (wchar_t*)
        cbToWrite,            // Write the total number of bytes
        &cbWritten,
        NULL);

    if (!fSuccess) {
        std::cerr << get_millis() << "ERROR: Failed to write to pipe. Code: " << GetLastError() << std::endl;
        return false;
    }

    // Check if the expected number of bytes were written
    if (cbWritten != cbToWrite) {
        std::cerr << get_millis() << "WARNING: Only wrote " << cbWritten << " of " << cbToWrite << " bytes." << std::endl;
    }

    return true;
}


void NamedPipeServer::close() {
    if (hPipe_ != INVALID_HANDLE_VALUE) {

        // 1. Swap the handle out early to prevent race conditions and re-use.
        // Use a temporary handle for all operations.
        HANDLE tempPipeHandle = hPipe_;
        hPipe_ = INVALID_HANDLE_VALUE;

        // 2. CANCEL ALL PENDING I/O
        // CancelIoEx is preferred over CancelIo for modern Windows.
        // Passing NULL cancels ALL pending I/O operations issued by the calling thread.
        // It is CRUCIAL that the thread blocked on I/O is the one that started it.
        // If the close() function is called from a DIFFERENT thread than the one
        // blocked on ConnectNamedPipe/ReadFile, you MUST use CancelIoEx.
        if (CancelIoEx(tempPipeHandle, NULL) == 0) {
            // In a client crash scenario, CancelIoEx might fail if no I/O is pending.
            // We can ignore the error if it's NOT a critical error (like ERROR_INVALID_HANDLE).
            // std::cerr << "CancelIoEx failed (may be harmless), error: " << GetLastError() << std::endl;
        }

        // 3. CLEANUP AND DISCONNECT (Optional but good practice)
        if (connected_) {
            // Flush is often ineffective if the client is crashed, but we try.
            FlushFileBuffers(tempPipeHandle);

            // This attempts a clean shutdown but is less reliable than CloseHandle.
            // It will unblock a waiting client, but if the client crashed, it may briefly hang.
            DisconnectNamedPipe(tempPipeHandle);
            connected_ = false;
            // std::cout << "NamedPipeServer::close - Pipe disconnected." << std::endl;
        }

        // 4. FORCIBLY CLOSE THE HANDLE
        // This is the definitive action. It forces the end of all I/O and terminates
        // any thread blocked on this specific handle (tempPipeHandle).
        std::cout << get_millis() << "NamedPipeServer::close - Handle closed." << std::endl;
        CloseHandle(tempPipeHandle);
    }
}

/**
 * @brief Checks if the pipe is currently connected to a client.
 */
bool NamedPipeServer::isConnected() const {
    return connected_;
}


// --- Additional Methods for Asynchronous Operation (Non-blocking) ---
// ASYNC methods for UDP
// --------------------------------------------------------------------


/**
 * @brief Creates the pipe and starts an asynchronous connection attempt.
 * Returns immediately so your UDP loop can keep running.
 */
bool NamedPipeServer::createAndListenAsync() {
    if (hPipe_ != INVALID_HANDLE_VALUE) return true;

    // Create the persistent event for polling
    if (hConnectEvent_ == NULL) {
        hConnectEvent_ = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    connectOverlap_.hEvent = hConnectEvent_;

    hPipe_ = CreateNamedPipeA(
        pipeName_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, // Overlapped for non-blocking
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
        PIPE_UNLIMITED_INSTANCES,
        bufferSize_, bufferSize_, 0, NULL);

    if (hPipe_ == INVALID_HANDLE_VALUE) return false;

    // Start the connection attempt
    if (!ConnectNamedPipe(hPipe_, &connectOverlap_)) {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING && dwError != ERROR_PIPE_CONNECTED) {
            close();
            return false;
        }
    }

    // If we reach here, the pipe is waiting for a client in the background
    return true;
}

/**
 * @brief Polling method to check if a client has connected.
 * Use this in your loop to see if it's time to start reading.
 */
bool NamedPipeServer::pollConnection() {
    if (connected_) return true;
    if (hPipe_ == INVALID_HANDLE_VALUE) return false;

    // Check event status with 0ms timeout
    if (WaitForSingleObject(hConnectEvent_, 0) == WAIT_OBJECT_0) {
        DWORD cbTransfer;
        if (GetOverlappedResult(hPipe_, &connectOverlap_, &cbTransfer, FALSE)) {
            connected_ = true;
            return true;
        }
    }
    return false;
}

/**
 * @brief Non-blocking data check using PeekNamedPipe.
 * Perfect for a high-frequency UDP loop.
 */
bool NamedPipeServer::pollData(std::string& outMessage) {
    if (!connected_) return false;

    DWORD bytesAvailable = 0;
    // Check if data is waiting without removing it from the buffer
    if (PeekNamedPipe(hPipe_, NULL, 0, NULL, &bytesAvailable, NULL)) {
        if (bytesAvailable > 0) {
            // Existing readMessage uses overlapped I/O, so with data 
            // already "peeked", it will return immediately.
            try {
                outMessage = readMessage(0);
                return !outMessage.empty();
            }
            catch (...) {
                return false;
            }
        }
    }
    else if (GetLastError() == ERROR_BROKEN_PIPE) {
        // Handle client disconnection (RTCupdate closed)
        resetPipe();
    }
    return false;
}

/**
 * @brief Resets the pipe to a listening state after a client disconnects.
 */
void NamedPipeServer::resetPipe() {
    connected_ = false;
    if (hPipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(hPipe_);
        ResetEvent(hConnectEvent_);
        ConnectNamedPipe(hPipe_, &connectOverlap_);
    }
}


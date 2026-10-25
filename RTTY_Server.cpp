#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>

#include "SDRunoSDK.h"
#include "SDRunoPlugin_TXLink.h"
#include "SDRunoPlugin_TXLinkUi.h"
#include "SDRunoPlugin_TXLinkForm.h"
#include "NamedPipeServer.h"
#include "MessageQueue.h"
#include "MessageBox.h"


std::atomic<bool> RTTYExitRequest;		//break out of loop and tidy up on app close

// Pipe name MUST match the VB6 client exactly!
#define PIPE_NAME "\\\\.\\pipe\\TXLinkCommandPipe"
#define BUFSIZE 512
#define PipeTimeout 5000		//time to wait for client to connect
#define ReadTimeout 100			//time to wait for read message

NamedPipeServer RTTYpipe(PIPE_NAME, BUFSIZE);					//get messages from RTTYcontroller
ThreadSafeMessageQueue<std::string> RTTYmsgOutQ;				//data from plugin to VB6 RTTYcontroller client


//started in a seperate thread from SDRunoPlugin_TXLink.cpp
void RTTY_Server_Loop(IUnoPluginController& _controller, SDRunoPlugin_TXLink& _parent)
{
	//Connect to RTTY client app (VB6) via Windows messages using the NamedPipeClient class.
	//Loop will retry until VB6 app is available and then monitor for TX requests
	//Requests are sent to the TXLink Arduino via the existing serial link by calling thread safe method in SDRunoPlugin_TXLinkForm
	
	while (!RTTYExitRequest) 
	{
		while (!RTTYpipe.isConnected() && !RTTYExitRequest)
		{
			std::cout << get_millis() << "RTTY Server waiting for client connection..." << std::endl;
			if (!RTTYpipe.createAndListen(PipeTimeout)) {
				std::cout << get_millis() << "RTTY Server failed to connect to client within timeout, retrying..." << std::endl;
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));		//wait a second and try again
			}
		}
		if (!RTTYExitRequest) { std::cout << get_millis() << "RTTY Server connected to client." << std::endl; }
				
		while (RTTYpipe.isConnected() && !RTTYExitRequest)
		{
			try {
				// Read with a 100 millisecond timeout
				std::string message = RTTYpipe.readMessage(ReadTimeout);
				if (!message.empty()) {
					// --- CORE APPLICATION LOGIC HERE ---
					std::cout << get_millis() << " RTTY Server Received Command: " << message << std::endl;
					// Example: Respond to a command
					if (message == "GET_STATUS") {
						std::cout << get_millis() << " RTTY Server Sending status back..." << std::endl;
						//RTTYpipe.writeMessage("STATUS:READY");
					}
					// Example: Implement RTTY command execution
					else if (message.rfind("RTTY:", 0) == 0) { // Command starts with "RTTY:"
						std::string rttyText = message.substr(5); // Extract text after "RTTY:"
						//std::cout << get_millis() << " RTTY Server Sending RTTY text to Arduino: " << rttyText << std::endl;
						// Call thread-safe method in SDRunoPlugin_TXLinkForm to send to Arduino
						_parent.m_Ui->m_form->SendSerialMsg(message);
						//RTTYpipe.writeMessage("RTTY:SENT");
					}
					else if (message.rfind("JTTY:", 0) == 0) {				//JTTY messages
						_parent.m_Ui->m_form->SendSerialMsg(message);		//process in Form
					}
					else {
						std::cout << get_millis() << " RTTY Server Unknown command received: " << message << std::endl;
						//RTTYpipe.writeMessage("ERROR:UNKNOWN_COMMAND");
					}
				}
			}
			catch (const std::runtime_error& e) {
				std::cerr << get_millis() << " RTTY Server Runtime error: " << e.what() << std::endl;
				// Assume pipe is broken/disconnected, break to outer loop to reconnect
				break;
			}

			// Check for outgoing messages to send to VB6 client from other threads
			// This is a non-blocking check
			while (!RTTYmsgOutQ.empty()) {
				std::string outMsg = RTTYmsgOutQ.waitAndPop();

				// If this is a frequency message, look ahead and collapse any
				// immediately-following run of {F...} messages down to just the last one -
				// intermediate frequencies in a fast-tuning burst are superseded before
				// they'd even finish sending, so there's no point transmitting them.
				if (outMsg.rfind("{F", 0) == 0) {
					while (!RTTYmsgOutQ.empty()) {
						std::string peeked = RTTYmsgOutQ.waitAndPop();
						if (peeked.rfind("{F", 0) == 0) {
							outMsg = peeked;   // newer freq supersedes the one we were holding
							continue;          // keep looking for an even newer one
						}
						else {
							// Next message isn't a frequency update - send the freq we're
							// holding now, then fall through to send `peeked` via the
							// normal path below by NOT discarding it.
							if (!outMsg.empty()) {
								std::cout << get_millis() << " RTTY Server Sending outgoing message to client: " << outMsg << std::endl;
								if (!RTTYpipe.writeMessage(outMsg)) {
									std::cerr << get_millis() << " RTTY Server Failed to send message to client." << std::endl;
									break;
								}
							}
							outMsg = peeked;   // now send this non-freq message via the normal path below
							break;
						}
					}
				}

				if (!outMsg.empty()) {
					std::cout << get_millis() << " RTTY Server Sending outgoing message to client: " << outMsg << std::endl;
					if (!RTTYpipe.writeMessage(outMsg)) {
						std::cerr << get_millis() << " RTTY Server Failed to send message to client." << std::endl;
						// If write fails, assume pipe is broken/disconnected
						break;
					}
				}
			}
		}
	}

	std::cout << get_millis() << "RTTY Server attempting to close pipe" << std::endl;
	RTTYpipe.close();
	RTTYExitRequest = false;
	std::cout << get_millis() << "RTTY Server closed and thread exiting" << std::endl;
}
#pragma once

namespace nana {
	namespace detail {
		class bedrock;
	}
}
nana::detail::bedrock& get_bedrock_instance();

#include <unoevent.h>
#include <future>
#include <functional>

#include "SDRunoSDK.h"
//#include "SDRunoPlugin_TXLinkForm.h"
class SDRunoPlugin_TXLinkForm;

#define WM_UDP_DATA_READY (WM_USER + 1)
#define WM_FREQ_DATA_READY (WM_USER + 2)

// Forward reference
class SDRunoPlugin_TXLink;

class SDRunoPlugin_TXLinkUi
{

public:
	SDRunoPlugin_TXLinkUi(SDRunoPlugin_TXLink& parent, IUnoPluginController& controller);
	~SDRunoPlugin_TXLinkUi();

	void UiHandleEvent(const UnoEvent& evt);
	void RequestDllUnload();

	void StartFormThread();			//starts the form thread if not already started
	void RunForm();				// Show and execute the form on its own thread

	void _updateTX_AnnoEntry(std::string tag, std::string tim, long long freq);
	void _updateRX_AnnoEntry(std::string tag, std::string tim, long long freq);

	std::shared_ptr<SDRunoPlugin_TXLinkForm> m_form;

	std::atomic<bool> FormPrepareUnloadRequest = false;		//used to signal form ops for closure
	std::atomic<bool> FormExitAllRequest = false;		//used to signal form ops for closure
	void ShutdownFormThread();
	std::thread m_thread;

	// Member function to get the future.
	std::future<void> get_exit_future() {
		return m_exitPromise.get_future();
	}
	std::promise<void> m_exitPromise;

	void PostUDPevent();

private:
	
	SDRunoPlugin_TXLink & m_parent;

	std::function<void()> m_cleanup_task;

	bool m_started;

	std::mutex m_lock;
	std::mutex m_lock1;

	IUnoPluginController & m_controller;

	std::future<void> m_exitFuture;

	std::string UnoEventTypeToString(UnoEvent::UnoEventType eventType);
};

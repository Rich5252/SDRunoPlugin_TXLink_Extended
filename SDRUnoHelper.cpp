#include "SDRUnoHelper.h"



std::mutex g_controllerFreqMutex;


double GetVfoFrequencySafe(IUnoPluginController& controller, int vfo, int maxRetries)
{
    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        try
        {
            {
                std::lock_guard<std::mutex> lock(g_controllerFreqMutex);
                return controller.GetVfoFrequency(vfo);
            }
        }
        catch (const std::system_error& e)
        {
            if (e.code().value() == 16 /* EBUSY */ && attempt < maxRetries - 1)
            {
                std::cout << get_millis() << " GetVfoFrequencySafe: EBUSY, retrying (" << attempt + 1 << ")" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            throw;   // not EBUSY, or out of retries - let it propagate
        }
    }
    throw std::runtime_error("GetVfoFrequencySafe: exhausted retries");
}

double SetVfoFrequencySafe(IUnoPluginController& controller, int vfo, double frequency, int maxRetries)
{
    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        try
        {
            {
                std::lock_guard<std::mutex> lock(g_controllerFreqMutex);
                return controller.SetVfoFrequency(vfo, frequency);
            }
        }
        catch (const std::system_error& e)
        {
            if (e.code().value() == 16 /* EBUSY */ && attempt < maxRetries - 1)
            {
                std::cout << get_millis() << " SetVfoFrequencySafe: EBUSY, retrying (" << attempt + 1 << ")" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            throw;   // not EBUSY, or out of retries - let it propagate
        }
    }
    throw std::runtime_error("SetVfoFrequencySafe: exhausted retries");
}
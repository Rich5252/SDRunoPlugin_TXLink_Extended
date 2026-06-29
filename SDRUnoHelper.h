#pragma once

#include <iostream>
#include <iunoplugincontroller.h>
#include <chrono>
#include <thread>
#include <mutex>

#include "MessageBox.h"         //getmillis()



double GetVfoFrequencySafe(IUnoPluginController& controller, int vfo, int maxRetries = 10);
double SetVfoFrequencySafe(IUnoPluginController& controller, int vfo, double frequency, int maxRetries = 10);

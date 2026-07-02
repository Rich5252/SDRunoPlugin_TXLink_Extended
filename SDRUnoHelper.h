#pragma once

#include "SDRunoSDK.h"

double GetVfoFrequencySafe(IUnoPluginController& controller, int vfo, int maxRetries = 10);
double SetVfoFrequencySafe(IUnoPluginController& controller, int vfo, double frequency, int maxRetries = 10);

// SDRunoSDK.h  put all required SDK files and extras that they need to compile in one place for convenience

#pragma once

#include <string>      // required by iunoplugincontroller.h before it's included else it throws compile errors
// ... any other prerequisites iunoplugincontroller.h silently needs

#include <iunoplugincontroller.h>
#include <iunoplugin.h>
#include <iunoannotator.h>
// ... any other SDRUno SDK headers that have the same issue

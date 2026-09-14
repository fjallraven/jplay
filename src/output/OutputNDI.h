#pragma once

#include "Output.h"

#include <memory>

// NDI output backend. Compiled only when JPLAY2_ENABLE_NDI is defined (the NDI
// SDK headers were found at configure time). The NDI runtime DLL is resolved and
// loaded at run time, so the app still launches on machines without it —
// ndiRuntimeAvailable() reports false there and the backend can't be selected.

// True if the NDI runtime library could be located and loaded.
bool ndiRuntimeAvailable();

// A fresh, unopened NDI sender device.
std::unique_ptr<OutputDevice> createNdiOutput();

#pragma once
#include <pybind11/pytypes.h>

#include <string>
#include <unordered_map>

// Host callbacks the Python startup scripts registered, keyed by name (e.g.
// "get_versions"). Populated by PythonStartup.cpp's embedded jplay module
// (jplay.register_callback(name, fn)); looked up by the jplay* bridge
// functions in PythonBridge.cpp.
//
// Every access (registration and call) happens while holding the GIL, so the
// GIL itself serialises the map — no separate mutex is needed. Backed by a
// function-local static so the py::object values outlive runStartup(); the
// interpreter is never finalised, so they are valid for the process lifetime.
std::unordered_map<std::string, pybind11::object>& callbackRegistry();

#pragma once
#include <thread>

// Spawns a background thread that initialises the embedded Python interpreter
// and executes any jplay_init.py files found on sys.path, falling back to
// "<exe dir>/python/jplay_init.py" when sys.path holds none. Those scripts
// register their lookups with the host via jplay.register_callback(name, fn).
// The thread must be joined (or detached) before the process exits.
std::thread spawnPythonStartup();

// True once the interpreter has finished startup and callbacks are usable. All
// the jplay* query functions in PythonBridge.h return false until this flips.
// Used to defer startup-time work (e.g. command-line audio pairing) that needs
// Python ready.
bool jplayPythonReady();

// Enable or disable Python-side debug logging (inputs/outputs of each callback).
// Safe to call at any time; takes effect on the next callback invocation.
void jplaySetPythonDebug(bool enabled);

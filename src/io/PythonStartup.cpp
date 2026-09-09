#include "PythonStartup.h"
#include "PythonCallbackRegistry.h"
#include "Progress.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace py = pybind11;

// Set true once the interpreter is initialised, the startup scripts have run,
// and the GIL has been released so other threads may call in. Read by the
// jplay* bridge helpers before they touch Python.
static std::atomic<bool> g_pythonReady{ false };

// When true, Python callbacks log their inputs and outputs via jplay.log.info.
// Written by jplaySetPythonDebug(); read by jplay.is_debug() from Python.
static std::atomic<bool> g_pythonDebug{ false };

std::unordered_map<std::string, py::object>& callbackRegistry() {
    static std::unordered_map<std::string, py::object> registry;
    return registry;
}

// The "jplay" host API exposed to the embedded interpreter. jplay is a WIN32
// (GUI subsystem) executable, so it has no stdout/stderr console — Python's
// print() output is lost. Route logging through SDL's logger instead, which on
// Windows goes to the debugger (OutputDebugString) and is the same channel the
// rest of the app logs through. Startup scripts register their lookups here:
//   import jplay
//   jplay.register_callback("describe_pickers", describe_pickers)
//   @jplay.callback("resolve_path")   # decorator form
//   def resolve_path(...): ...
PYBIND11_EMBEDDED_MODULE(jplay, m) {
    m.def("register_callback", [](const std::string& name, py::object fn) {
        callbackRegistry()[name] = std::move(fn);
    }, py::arg("name"), py::arg("fn"),
       "Register a host callback under the given name (last registration wins).");

    m.def("callback", [](const std::string& name) {
        // Decorator factory: @jplay.callback("x") registers and returns the
        // function unchanged so the name stays usable in Python too.
        return py::cpp_function([name](py::object fn) {
            callbackRegistry()[name] = fn;
            return fn;
        });
    }, py::arg("name"),
       "Decorator form of register_callback: @jplay.callback('name').");

    m.def("is_debug", []() {
        return g_pythonDebug.load(std::memory_order_relaxed);
    }, "Return True when Python debug logging is enabled from the Settings panel.");

    // Host progress channel handed to long-running callbacks (e.g.
    // create_from_directory(root, progress=...)). The host owns the
    // ProgressReporter and passes it by reference; Python calls update() as it
    // works and polls cancelled() at its checkpoints. Non-owning: py::nodelete
    // keeps pybind11 from deleting the host's object when the wrapper is GC'd.
    py::class_<ProgressReporter, std::unique_ptr<ProgressReporter, py::nodelete>>(m, "Progress")
        .def("update", &ProgressReporter::update,
             py::arg("fraction"), py::arg("message") = std::string(),
             "Report progress: fraction in [0,1], negative = indeterminate; an "
             "empty message leaves the current one unchanged.")
        .def("set_message", &ProgressReporter::setMessage, py::arg("message"))
        .def("cancelled", &ProgressReporter::cancelled,
             "True once the host has requested cancellation; bail out when set.");

    py::module_ log = m.def_submodule("log", "Logging bridge to the host (SDL log).");
    log.def("info", [](const std::string& s) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", s.c_str());
    });
    log.def("error", [](const std::string& s) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", s.c_str());
    });
}

// Where the interpreter should look for its standard library, or empty to let
// libpython work that out from its own compiled-in prefix:
//   1. Prefer the runtime bundled next to the exe ("<exe dir>/pyhome"), so a
//      packaged build is self-contained on machines without Python.
//   2. Fall back to the build-machine Python home for local dev.
static std::string resolvePythonHome() {
    std::string home;
#if defined(JPLAY_PYTHON_BUNDLE_NAME)
    if (const char* base = SDL_GetBasePath()) { // exe dir, trailing sep; do not free
        std::string candidate = std::string(base) + JPLAY_PYTHON_BUNDLE_NAME;
        // Only trust the bundle if the stdlib is actually present there.
        // JPLAY_PYTHON_STDLIB_REL is the interpreter's own layout: "Lib" on
        // Windows, "lib/pythonX.Y" elsewhere.
        std::string marker = candidate + "/" JPLAY_PYTHON_STDLIB_REL "/os.py";
        if (FILE* f = std::fopen(marker.c_str(), "rb")) {
            std::fclose(f);
            home = candidate;
        }
    }
#endif
#if defined(JPLAY_DEV_PYTHONHOME) && defined(_WIN32)
    // Dev fallback: on Windows dirname(python.exe) IS the interpreter home
    // (it contains Lib/ and DLLs/). On POSIX the executable lives in <prefix>/bin,
    // so that path is NOT a valid home — leave it unset there and let libpython
    // locate its own prefix.
    if (home.empty())
        home = JPLAY_DEV_PYTHONHOME;
#endif
    return home;
}

// PyConfig paths are wchar_t. SDL hands us UTF-8, so widen it ourselves rather
// than going through PyConfig_SetBytesString, which decodes with the locale
// encoding and would mangle a non-ASCII install path on Windows.
static PyStatus setConfigPath(PyConfig* config, wchar_t** target, const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = std::filesystem::u8path(path).wstring();
    return PyConfig_SetString(config, target, wide.c_str());
#else
    return PyConfig_SetBytesString(config, target, path.c_str());
#endif
}

static void runStartup() {
    // Configure the interpreter through PyConfig rather than by setting PYTHON*
    // environment variables: this process is already running, so a putenv here
    // races with every other thread that reads the environment, and the settings
    // would be inherited by anything jplay spawns.
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;               // we pass no argv (pybind11 PR #4473)
    config.install_signal_handlers = 0;  // we are on a non-main OS thread; Python
                                         // must not install SIGINT etc. here
    config.write_bytecode = 0;           // the startup scripts live in site-owned
                                         // (often read-only or shared) directories,
                                         // where __pycache__ dirs are noise at best

    // The embedded interpreter needs to find its standard library (the
    // 'encodings' module in particular, or init fails with "failed to get the
    // Python codec of the filesystem encoding"). A site pointing PYTHONHOME at
    // its own interpreter still wins: PyConfig_InitPythonConfig reads the
    // environment, so only fill in a home of our own when nobody else set one.
    if (!std::getenv("PYTHONHOME")) {
        const std::string home = resolvePythonHome();
        if (!home.empty()) {
            const PyStatus status = setConfigPath(&config, &config.home, home);
            if (PyStatus_Exception(status) != 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[jplay] python home rejected: %s", home.c_str());
                PyConfig_Clear(&config);
                return;
            }
        }
    }

    // Takes the config over, and clears it whether or not it succeeds.
    try {
        py::initialize_interpreter(&config);
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] python init failed: %s", e.what());
        return;
    }

    try {
        // Hand the exe directory to the script below so it can fall back to the
        // startup files deployed next to the binary. Empty if SDL can't resolve it.
        py::module_::import("__main__").attr("__dict__")["_jplay_exe_dir"] =
            [] { const char* base = SDL_GetBasePath(); // do not free
                 return base ? std::string(base) : std::string(); }();

        // Redirect stdout/stderr to SDL's logger so print() from startup
        // scripts is visible, then scan every directory in sys.path for
        // jplay_init.py and exec each one found. All occurrences are run
        // (not just the first), so site-wide and project-local startup files
        // can coexist. Only if the scan comes up empty do we fall back to
        // "<exe dir>/python".
        py::exec(R"(
import sys, os, importlib.util, traceback
import jplay

class _LogWriter:
    def __init__(self, sink, prefix):
        self._sink = sink
        self._prefix = prefix
        self._buf = ''
    def write(self, s):
        self._buf += s
        while '\n' in self._buf:
            line, self._buf = self._buf.split('\n', 1)
            self._sink(self._prefix + line)
        return len(s)
    def flush(self):
        if self._buf:
            self._sink(self._prefix + self._buf)
            self._buf = ''

sys.stdout = _LogWriter(jplay.log.info, '[py] ')
sys.stderr = _LogWriter(jplay.log.error, '[py] ')

def _log(msg):
    jplay.log.info('[jplay] ' + msg)

def _run_init(path):
    try:
        _spec = importlib.util.spec_from_file_location('jplay_init', path)
        _mod  = importlib.util.module_from_spec(_spec)
        _spec.loader.exec_module(_mod)
        # Keep it importable by name so a startup file can be re-imported
        # without re-executing; callbacks reach the host via
        # jplay.register_callback(), not by name lookup on this module.
        sys.modules['jplay_init'] = _mod
        _log('executed: ' + path)
    except Exception:
        _log('error executing: ' + path)
        traceback.print_exc()

_found = 0
for _dir in sys.path:
    if not _dir:
        continue
    _candidate = os.path.join(_dir, 'jplay_init.py')
    if os.path.isfile(_candidate):
        _found += 1
        _run_init(_candidate)

if _found == 0:
    # Nothing on sys.path: try the startup files shipped next to the exe. That
    # directory is not on sys.path, and jplay_init.py imports its siblings
    # (jplay_naming_core etc.) by name, so it has to be added before exec.
    _bundled = os.path.join(_jplay_exe_dir, 'python') if _jplay_exe_dir else ''
    _candidate = os.path.join(_bundled, 'jplay_init.py') if _bundled else ''
    if _candidate and os.path.isfile(_candidate):
        _found += 1
        if _bundled not in sys.path:
            sys.path.insert(0, _bundled)
        _run_init(_candidate)

if _found == 0:
    _log('no jplay_init.py found on sys.path or in ' +
         (os.path.join(_jplay_exe_dir, 'python') if _jplay_exe_dir else '<exe dir>/python'))
)");
        // Report what the startup scripts registered, so a missing callback is
        // obvious in the log rather than surfacing later as a silent no-op. Debug
        // logging only — on a healthy launch this is one line per callback of noise.
        if (g_pythonDebug.load(std::memory_order_relaxed)) {
            const auto& registry = callbackRegistry();
            if (registry.empty()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[jplay] no callbacks registered");
            } else {
                for (const auto& entry : registry)
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[jplay] registered callback: %s", entry.first.c_str());
            }
        }
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] python startup error: %s", e.what());
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] python startup error: %s", e.what());
    }
    // Interpreter is left alive for the process lifetime so that any global
    // state set up by the startup modules (callbacks, registrations) persists.
    // Release the GIL held by this (soon-to-exit) thread so other threads can
    // call into Python via PyGILState_Ensure (pybind11's gil_scoped_acquire).
    g_pythonReady.store(true, std::memory_order_release);
    PyEval_SaveThread();
}

std::thread spawnPythonStartup() {
    return std::thread(runStartup);
}

bool jplayPythonReady() {
    return g_pythonReady.load(std::memory_order_acquire);
}

void jplaySetPythonDebug(bool enabled) {
    g_pythonDebug.store(enabled, std::memory_order_relaxed);
}

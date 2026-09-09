#pragma once

#include <string>

// Reads jplay_preferences.conf, the app-wide preferences file (INI-style:
// [section] headers with `key = value` options). The file is loaded once and
// cached on first access. Resolution order (first existing file wins):
//   1. $JPLAY_PREFERENCES                 explicit override
//   2. ~/.jplay/jplay_preferences.conf    deployed user override
//   3. <exe dir>/jplay_preferences.conf   shipped default
namespace Preferences {

// Value of `key` under `[section]`, or an empty string if the section/key is
// absent or no config file exists.
std::string get(const std::string& section, const std::string& key);

// Boolean option: 1/true/yes/on (case-insensitive) are true, 0/false/no/off are
// false, and anything else — including a missing key — leaves `def` in force.
bool getBool(const std::string& section, const std::string& key, bool def);

} // namespace Preferences

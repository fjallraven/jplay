#pragma once

#include <string>

// Reads jplay_preferences.conf, the app-wide preferences file (INI-style:
// [section] headers with `key = value` options). Loaded once and cached on first
// access. All three tiers below are read and merged option by option, weakest
// first, so a higher tier overrides only the options it actually names:
//   1. <exe dir>/jplay_preferences.conf   shipped default (weakest)
//   2. ~/.jplay/jplay_preferences.conf    deployed user override
//   3. $JPLAY_PREFERENCES                 explicit override (strongest)
// A user file therefore need only carry the options it changes; everything it
// omits keeps the shipped value. Missing files are skipped, so a fresh install
// with no user file behaves exactly as the shipped default alone.
namespace Preferences {

// Value of `key` under `[section]`, or an empty string if the section/key is
// absent or no config file exists.
std::string get(const std::string& section, const std::string& key);

// Boolean option: 1/true/yes/on (case-insensitive) are true, 0/false/no/off are
// false, and anything else — including a missing key — leaves `def` in force.
bool getBool(const std::string& section, const std::string& key, bool def);

} // namespace Preferences

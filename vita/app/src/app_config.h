// app_config.h - ux0:data/infected/config.txt.
//
// The Vita has no environment, but the runtime is configured through INFECTED_* (and PSPRECOMP_*) environment variables read with
// getenv. newlib's setenv works, so every KEY=VALUE line of config.txt is exported to the process environment before the runtime
// starts and the code that reads INFECTED_<NAME> needs no change. A key that already starts with INFECTED_, PSPRECOMP_,
// PORTABLEKIT_ or LEAN_ (the lean library reads LEAN_NO_HOOKS) is exported as it is; any other key becomes INFECTED_<KEY upper-cased>. Lines starting with '#' and blank lines
// are ignored; keys are case-insensitive; a value may be quoted with "...". The application's own keys (bench, iso, ...) are
// read from the same table with cfg_*().
#pragma once

#include <cstdint>
#include <string>

namespace app {

// Reads the file (relative to the data root; the platform layer must be initialised) and exports it. Returns the number of
// lines accepted; -1 when the file is missing (defaults apply, logged).
int config_load(const char* rel_path);

// Exports one default unless the file (or the real environment) already set it.
void config_default(const char* key, const char* value);

// Application keys (the INFECTED_ prefix is added): nullptr / default when unset.
const char* cfg_get(const char* key);
std::string cfg_str(const char* key, const char* fallback);
long cfg_int(const char* key, long fallback);
bool cfg_bool(const char* key, bool fallback);

// Everything read, for the log (one "KEY=VALUE" per line, after the prefix was applied).
std::string config_dump();

}  // namespace app

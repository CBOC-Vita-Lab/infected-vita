#include "app_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <map>

#include "platform.h"

namespace app {
namespace {

std::map<std::string, std::string>& table() {
    static std::map<std::string, std::string> t;
    return t;
}

std::string full_name(const std::string& key) {
    std::string k;
    for (char c : key) k.push_back((char)toupper((unsigned char)c));
    if (k.rfind("INFECTED_", 0) == 0 || k.rfind("PSPRECOMP_", 0) == 0 || k.rfind("PORTABLEKIT_", 0) == 0 || k.rfind("LEAN_", 0) == 0) return k;
    return "INFECTED_" + k;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

void put(const std::string& key, const std::string& value) {
    const std::string name = full_name(key);
    table()[name] = value;
    setenv(name.c_str(), value.c_str(), 1);
}

}  // namespace

int config_load(const char* rel_path) {
    void* data = nullptr;
    size_t size = 0;
    if (!plat::file_read_all(plat::Root::Data, rel_path, &data, &size)) {
        plat::logf("config: %s not found, built-in defaults apply", rel_path);
        return -1;
    }
    std::string text((const char*)data, size);
    free(data);
    int accepted = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            plat::logf("config: ignoring line without '=': %s", line.c_str());
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (key.empty()) continue;
        put(key, value);
        accepted++;
    }
    plat::logf("config: %d settings from %s", accepted, rel_path);
    return accepted;
}

void config_default(const char* key, const char* value) {
    const std::string name = full_name(key);
    if (table().count(name) || getenv(name.c_str())) return;
    put(key, value);
}

const char* cfg_get(const char* key) {
    const std::string name = full_name(key);
    auto it = table().find(name);
    if (it != table().end()) return it->second.c_str();
    return getenv(name.c_str());
}

std::string cfg_str(const char* key, const char* fallback) {
    const char* v = cfg_get(key);
    return v ? v : fallback;
}

long cfg_int(const char* key, long fallback) {
    const char* v = cfg_get(key);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long r = strtol(v, &end, 0);
    return end == v ? fallback : r;
}

bool cfg_bool(const char* key, bool fallback) {
    const char* v = cfg_get(key);
    if (!v || !*v) return fallback;
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) return false;
    return true;
}

std::string config_dump() {
    std::string out;
    for (auto& kv : table()) out += kv.first + "=" + kv.second + "\n";
    return out;
}

}  // namespace app

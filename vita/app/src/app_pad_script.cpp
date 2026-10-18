#include "app_pad_script.h"

#include <stdlib.h>
#include <string.h>

#include <string>

#include "app_config.h"
#include "platform.h"

namespace app {

void pad_script_setup() {
    const std::string file = cfg_str("pad_script_file", "");
    if (file.empty()) {
        if (const char* inline_script = cfg_get("pad_script"); inline_script && *inline_script)
            plat::logf("pad script: inline, %u bytes", (unsigned)strlen(inline_script));
        return;
    }
    void* data = nullptr;
    size_t size = 0;
    if (!plat::file_read_all(plat::Root::Data, file.c_str(), &data, &size)) {
        plat::logf("pad script: cannot read %s (the run has no scripted input)", file.c_str());
        return;
    }
    std::string text, line;
    const char* p = (const char*)data;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || p[i] == '\n' || p[i] == '\r') {
            size_t a = 0;
            while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) a++;
            if (a < line.size() && line[a] != '#') {
                if (!text.empty() && text.back() != ',') text += ',';
                text += line.substr(a);
            }
            line.clear();
        } else {
            line.push_back(p[i]);
        }
    }
    free(data);
    while (!text.empty() && (text.back() == ',' || text.back() == ' ')) text.pop_back();
    setenv("INFECTED_PAD_SCRIPT", text.c_str(), 1);
    size_t steps = 1;
    for (char c : text)
        if (c == ',') steps++;
    plat::logf("pad script: %s, %u steps", file.c_str(), (unsigned)steps);
}

}  // namespace app

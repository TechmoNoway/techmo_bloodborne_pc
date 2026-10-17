// SPDX-License-Identifier: GPL-2.0-or-later
// Device-free fixtures retain real assertion code and use a deterministic log sink.
#include "common/logging/log.h"
#include <cstdio>
namespace Common::Log {
bool ShouldLog(Level) { return true; }
void Write(std::string_view,Level,const char*,int,const char*,std::string&& message) {
    std::fprintf(stderr,"%s\n",message.c_str());
}
}

// OSReport natively: guest printf-style output goes to the host log.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "funcs.h"
#include "runtime/runtime.h"

namespace {

std::string guest_string(uint32_t a, size_t max = 512) {
    std::string out;
    for (size_t k = 0; k < max; k++) {
        uint8_t* p = ppc_host(a + (uint32_t)k);
        if (!p || !*p) break;
        out.push_back((char)*p);
    }
    return out;
}

// PowerPC EABI varargs: integers in r4..r10, doubles in f1..f8, then the
// caller's parameter save area at r1 + 8.
struct VaList {
    PPCState* s;
    int gpr = 4, fpr = 1;
    uint32_t stack;
    explicit VaList(PPCState* st) : s(st), stack(st->r[1] + 8) {}
    uint32_t next_int() {
        if (gpr <= 10) return s->r[gpr++];
        uint32_t v = rd32(stack);
        stack += 4;
        return v;
    }
    double next_double() {
        if (fpr <= 8) return s->f[fpr++].d;
        stack = (stack + 7) & ~7u;
        double v = rdf64(stack);
        stack += 8;
        return v;
    }
};

}  // namespace

std::string guest_format(PPCState* s, uint32_t fmt_addr, int first_gpr) {
    std::string fmt = guest_string(fmt_addr), out;
    VaList va(s);
    va.gpr = first_gpr;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0123456789.lhz", fmt[j])) j++;
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i + 1);
        char c = fmt[j], buf[512];
        spec.erase(std::remove(spec.begin(), spec.end(), 'l'), spec.end());
        spec.erase(std::remove(spec.begin(), spec.end(), 'h'), spec.end());
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(buf, sizeof buf, spec.c_str(), va.next_int()); break;
        case 'p': snprintf(buf, sizeof buf, "0x%08X", va.next_int()); break;
        case 's': snprintf(buf, sizeof buf, spec.c_str(), guest_string(va.next_int()).c_str()); break;
        case 'f': case 'g': case 'e': case 'F': case 'G': case 'E':
            snprintf(buf, sizeof buf, spec.c_str(), va.next_double()); break;
        case '%': snprintf(buf, sizeof buf, "%%"); break;
        default: snprintf(buf, sizeof buf, "%s", spec.c_str()); break;
        }
        out += buf;
        i = j;
    }
    return out;
}

void hle_OSReport(PPCState* s) {
    std::string msg = guest_format(s, s->r[3], 4);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    rt_log("OSReport: %s", msg.c_str());
}

// DVDConvertPathToEntrynum natively: resolve a path against the disc's FST
// (as loaded by the SDK into guest memory), relative to the DVD current
// directory, case-insensitively. Unresolved paths are logged, which is also
// where asset remapping for other platforms' discs will hook in.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "funcs.h"
#include "runtime/runtime.h"

namespace {

// dvdfs.c statics (see config/G3FE69/symbols.txt)
constexpr uint32_t kFstStart = 0x80613164, kFstStringStart = 0x80613168, kMaxEntryNum = 0x8061316C,
                   kCurrentDirectory = 0x80613170;

std::string guest_cstr(uint32_t a) {
    std::string s;
    for (uint8_t* p; (p = ppc_host(a)) && *p && s.size() < 512; a++) s.push_back((char)*p);
    return s;
}

bool same(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); k++)
        if (tolower((unsigned char)a[k]) != tolower((unsigned char)b[k])) return false;
    return true;
}

int32_t resolve(const std::string& path) {
    uint32_t fst = rd32(kFstStart), strings = rd32(kFstStringStart), max = rd32(kMaxEntryNum);
    auto is_dir = [&](uint32_t e) { return (rd32(fst + 12 * e) >> 24) != 0; };
    auto name = [&](uint32_t e) { return guest_cstr(strings + (rd32(fst + 12 * e) & 0x00FFFFFF)); };
    auto parent = [&](uint32_t e) { return rd32(fst + 12 * e + 4); };
    auto next = [&](uint32_t e) { return rd32(fst + 12 * e + 8); };

    uint32_t dir = rd32(kCurrentDirectory);
    size_t pos = 0;
    if (!path.empty() && path[0] == '/') { dir = 0; pos = 1; }
    while (pos <= path.size()) {
        size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty() || part == ".") {
            if (end == path.size()) return (int32_t)dir;
            continue;
        }
        if (part == "..") { dir = parent(dir); continue; }
        bool last = end == path.size();
        int32_t found = -1;
        for (uint32_t e = dir + 1; e < next(dir) && e < max;) {
            if (same(name(e), part)) { found = (int32_t)e; break; }
            e = is_dir(e) ? next(e) : e + 1;
        }
        if (found < 0) return -1;
        if (last) return found;
        if (!is_dir((uint32_t)found)) return -1;
        dir = (uint32_t)found;
    }
    return (int32_t)dir;
}

}  // namespace

void hle_DVDConvertPathToEntrynum(PPCState* s) {
    std::string path = guest_cstr(s->r[3]);
    int32_t e = resolve(path);
    if (e < 0) {
        const char* rt_name_of(uint32_t);
        bool printable = !path.empty();
        for (char c : path) printable &= c >= 0x20 && c < 0x7F;
        if (printable) rt_log("DVD: \"%s\" not found on disc", path.c_str());
        else rt_log("DVD: unprintable path at 0x%08X (%02X %02X %02X %02X) from 0x%08X %s", s->r[3], rd8(s->r[3]), rd8(s->r[3] + 1),
                    rd8(s->r[3] + 2), rd8(s->r[3] + 3), s->lr, rt_name_of(s->lr));
    }
    s->r[3] = (uint32_t)e;
}


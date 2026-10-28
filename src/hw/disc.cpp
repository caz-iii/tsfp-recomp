#include "hw/disc.h"

#include <algorithm>
#include <cstring>

#include "runtime/runtime.h"

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

std::vector<uint8_t> slurp(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        out.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        out.resize(fread(out.data(), 1, out.size(), f));
        fclose(f);
    }
    return out;
}

}  // namespace

Disc::~Disc() {
    for (auto& kv : open_) fclose(kv.second);
}

FILE* Disc::handle(const std::string& path) {
    auto it = open_.find(path);
    if (it != open_.end()) return it->second;
    FILE* f = fopen(path.c_str(), "rb");
    open_[path] = f;
    return f;
}

bool Disc::open(const std::string& files_root, const std::string& sys_root) {
    std::vector<uint8_t> boot = slurp(sys_root + "/boot.bin");
    fst_ = slurp(sys_root + "/fst.bin");
    if (boot.size() < 0x440 || fst_.size() < 12) return false;

    // System area: boot.bin, bi2.bin, apploader, main.dol, fst.bin at their disc offsets.
    uint32_t dol_off = be32(&boot[0x420]), fst_off = be32(&boot[0x424]);
    extents_.push_back({0, 0x440, sys_root + "/boot.bin"});
    extents_.push_back({0x440, 0x2000, sys_root + "/bi2.bin"});
    extents_.push_back({0x2440, (uint32_t)slurp(sys_root + "/apploader.img").size(), sys_root + "/apploader.img"});
    extents_.push_back({dol_off, (uint32_t)slurp(sys_root + "/main.dol").size(), sys_root + "/main.dol"});
    extents_.push_back({fst_off, (uint32_t)fst_.size(), sys_root + "/fst.bin"});

    // FST: 12-byte entries, then the string table. Directory entries hold
    // (parent, next) so a stack of (end index, path) rebuilds full paths.
    uint32_t count = be32(&fst_[8]);
    const char* names = (const char*)&fst_[count * 12];
    std::vector<std::pair<uint32_t, std::string>> dirs{{count, files_root}};
    for (uint32_t i = 1; i < count; i++) {
        while (!dirs.empty() && i >= dirs.back().first) dirs.pop_back();
        const uint8_t* e = &fst_[i * 12];
        bool is_dir = e[0] != 0;
        std::string path = dirs.back().second + "/" + (names + (be32(e) & 0x00FFFFFF));
        if (is_dir) {
            dirs.push_back({be32(e + 8), path});
        } else {
            extents_.push_back({be32(e + 4), be32(e + 8), path});
        }
    }
    std::sort(extents_.begin(), extents_.end(), [](auto& a, auto& b) { return a.offset < b.offset; });
    rt_log("DI: virtual disc with %zu files", extents_.size());
    return true;
}

bool Disc::read(uint64_t offset, uint8_t* dst, uint32_t len) {
    bool ok = true;
    while (len) {
        auto it = std::upper_bound(extents_.begin(), extents_.end(), offset,
                                   [](uint64_t v, const Extent& e) { return v < e.offset; });
        if (it == extents_.begin()) { memset(dst, 0, len); return false; }
        --it;
        uint64_t rel = offset - it->offset;
        if (rel >= it->size) {  // gap between files: padding
            uint64_t next = (it + 1) != extents_.end() ? (it + 1)->offset : offset + len;
            uint32_t n = (uint32_t)std::min<uint64_t>(len, next - offset);
            memset(dst, 0, n);
            dst += n; offset += n; len -= n;
            if (it + 1 == extents_.end()) ok = false;
            continue;
        }
        uint32_t n = (uint32_t)std::min<uint64_t>(len, it->size - rel);
        FILE* f = handle(it->path);
        if (!f || _fseeki64(f, (long long)rel, SEEK_SET) != 0 || fread(dst, 1, n, f) != n) {
            memset(dst, 0, n);
            ok = false;
        }
        dst += n; offset += n; len -= n;
    }
    return ok;
}

// Virtual GameCube disc assembled from an extracted file tree (sys/ + files/).
// The disc's FST maps every file to its original disc offset, so the SDK's
// own DVD code reads exactly what it would from the real disc.
#ifndef TSFP_DISC_H
#define TSFP_DISC_H

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

class Disc {
public:
    ~Disc();
    bool open(const std::string& files_root, const std::string& sys_root);
    bool read(uint64_t offset, uint8_t* dst, uint32_t len);
    const std::vector<uint8_t>& fst() const { return fst_; }

private:
    struct Extent {
        uint64_t offset;
        uint32_t size;
        std::string path;
    };
    FILE* handle(const std::string& path);

    std::vector<Extent> extents_;  // sorted by offset
    std::vector<uint8_t> fst_;
    std::map<std::string, FILE*> open_;
};

#endif  // TSFP_DISC_H

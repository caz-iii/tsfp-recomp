// ppcrecomp: static recompiler from a GameCube DOL (Gekko PowerPC) to C++.
//
//   ppcrecomp <main.dol> <symbols.txt> <out-dir> [--hle <names.txt>] [--files N]
//
// Function boundaries come from decomp-toolkit's symbols.txt. Each guest function
// becomes `void fn_XXXXXXXX(PPCState* s)`; branches inside it become gotos, calls
// become C calls, jump tables become switches. Functions listed in the HLE file
// are emitted as forwarding stubs to `hle_<name>(PPCState*)`.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Segment {
    uint32_t addr;
    std::vector<uint8_t> data;
    bool text;
};

struct Function {
    uint32_t addr, size;
    std::string name;
};

struct Image {
    std::vector<Segment> segs;
    uint32_t entry = 0;

    const uint8_t* at(uint32_t a, uint32_t n) const {
        for (auto& s : segs)
            if (a >= s.addr && a + n <= s.addr + s.data.size()) return &s.data[a - s.addr];
        return nullptr;
    }
    uint32_t word(uint32_t a) const {
        const uint8_t* p = at(a, 4);
        return p ? (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3] : 0;
    }
};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool load_dol(const char* path, Image& img) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 0x100) return false;
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(&d[i * 4]), addr = be32(&d[0x48 + i * 4]), size = be32(&d[0x90 + i * 4]);
        if (!size) continue;
        img.segs.push_back({addr, std::vector<uint8_t>(d.begin() + off, d.begin() + off + size), i < 7});
    }
    img.entry = be32(&d[0xE0]);
    return true;
}

// symbols.txt lines: `name = .text:0x80003100; // type:function size:0x30 scope:global`
void load_symbols(const char* path, std::vector<Function>& funcs, std::vector<std::pair<uint32_t, uint32_t>>& jumptables) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find(" = ");
        auto colon = line.find(":0x");
        if (eq == std::string::npos || colon == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        uint32_t addr = (uint32_t)strtoul(line.c_str() + colon + 3, nullptr, 16);
        auto sz = line.find("size:0x");
        uint32_t size = sz == std::string::npos ? 0 : (uint32_t)strtoul(line.c_str() + sz + 7, nullptr, 16);
        if (line.find("type:function") != std::string::npos && size)
            funcs.push_back({addr, size, name});
        else if (name.rfind("jumptable_", 0) == 0 && size)
            jumptables.push_back({addr, size});
    }
    std::sort(funcs.begin(), funcs.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
}

// ------------------------------------------------------------------ fields
inline uint32_t OP(uint32_t i) { return i >> 26; }
inline uint32_t RD(uint32_t i) { return (i >> 21) & 31; }
inline uint32_t RA(uint32_t i) { return (i >> 16) & 31; }
inline uint32_t RB(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t RC(uint32_t i) { return (i >> 6) & 31; }
inline int32_t SIMM(uint32_t i) { return (int16_t)(i & 0xFFFF); }
inline uint32_t UIMM(uint32_t i) { return i & 0xFFFF; }
inline uint32_t XO10(uint32_t i) { return (i >> 1) & 0x3FF; }
inline uint32_t XO5(uint32_t i) { return (i >> 1) & 0x1F; }
inline bool RCB(uint32_t i) { return i & 1; }
inline uint32_t SPR(uint32_t i) { return ((i >> 16) & 31) | (((i >> 11) & 31) << 5); }

uint32_t rot_mask(uint32_t mb, uint32_t me) {
    uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}

std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}
std::string fname(uint32_t a) {
    char b[16];
    snprintf(b, sizeof b, "fn_%08X", a);
    return b;
}
std::string label(uint32_t a) {
    char b[16];
    snprintf(b, sizeof b, "L_%08X", a);
    return b;
}

// Effective address `(rA|0) + d`
std::string ea_d(uint32_t i) {
    std::string base = RA(i) ? "s->r[" + std::to_string(RA(i)) + "]" : "0";
    return "(uint32_t)(" + base + " + " + std::to_string(SIMM(i)) + ")";
}
std::string ea_x(uint32_t i) {
    std::string base = RA(i) ? "s->r[" + std::to_string(RA(i)) + "] + " : "";
    return "(uint32_t)(" + base + "s->r[" + std::to_string(RB(i)) + "])";
}
std::string R(uint32_t n) { return "s->r[" + std::to_string(n) + "]"; }
std::string F(uint32_t n) { return "s->f[" + std::to_string(n) + "].d"; }
std::string FU(uint32_t n) { return "s->f[" + std::to_string(n) + "].u"; }
std::string P1(uint32_t n) { return "s->ps1[" + std::to_string(n) + "]"; }

// ------------------------------------------------------------------ emitter
class Emitter {
public:
    Emitter(const Image& img, const std::vector<Function>& funcs,
            const std::map<uint32_t, std::vector<uint32_t>>& jt_by_func, const std::set<uint32_t>& starts,
            uint32_t save_context)
        : img_(img), funcs_(funcs), jt_by_func_(jt_by_func), starts_(starts), save_context_(save_context) {}

    int unknown = 0;

    std::string function(const Function& fn) {
        cur_ = &fn;
        out_.str("");
        labels_.clear();
        loops_.clear();
        uint32_t end = fn.addr + fn.size;
        // Pass 1: branch targets inside the function.
        for (uint32_t pc = fn.addr; pc < end; pc += 4) {
            uint32_t i = img_.word(pc);
            if (OP(i) == 18 || OP(i) == 16) {
                uint32_t t = target(i, pc);
                if (t >= fn.addr && t < end) {
                    labels_.insert(t);
                    if (t <= pc) loops_.insert(t);
                }
            }
        }
        auto jt = jt_by_func_.find(fn.addr);
        if (jt != jt_by_func_.end())
            for (uint32_t t : jt->second) labels_.insert(t);
        // Jump tables found from the code itself (names in symbols.txt change as
        // the decomp progresses): lis/addi -> lwzx -> mtctr -> bctr.
        switch_.clear();
        for (uint32_t pc = fn.addr; pc < end; pc += 4) {
            if (img_.word(pc) != 0x4E800420) continue;  // bctr
            std::vector<uint32_t> targets = find_table(pc, fn.addr, end);
            for (uint32_t t : targets) labels_.insert(t);
            if (!targets.empty()) switch_[pc] = targets;
        }

        out_ << "// " << fn.name << "\nvoid " << fname(fn.addr) << "(PPCState* s) {\n";
        line("PPC_TRACE(" + hex(fn.addr) + ");");
        if (traced_.count(fn.name)) line("ppc_trace_call(s, " + hex(fn.addr) + ");");
        for (uint32_t pc = fn.addr; pc < end; pc += 4) {
            if (labels_.count(pc)) out_ << label(pc) << ":\n";
            if (loops_.count(pc)) line("PPC_POLL(s);");
            insn(img_.word(pc), pc);
        }
        out_ << "}\n\n";
        return out_.str();
    }

private:
    const Image& img_;
    const std::vector<Function>& funcs_;
    const std::map<uint32_t, std::vector<uint32_t>>& jt_by_func_;
    const std::set<uint32_t>& starts_;
    const Function* cur_ = nullptr;
public:
    std::set<std::string> traced_;
private:
    uint32_t save_context_;  // OSSaveContext: the guest's setjmp, emitted inline
    std::set<uint32_t> labels_;
    std::set<uint32_t> loops_;  // backward-branch targets: interrupt poll points
    std::map<uint32_t, std::vector<uint32_t>> switch_;  // bctr pc -> jump table targets

    // Walk back from a bctr: mtctr rC <- lwzx rC, rA, rB <- addi rA, rA, lo <- lis rA, hi.
    std::vector<uint32_t> find_table(uint32_t bctr, uint32_t fstart, uint32_t fend) const {
        int rc = -1, ra = -1;
        uint32_t lo = 0, table = 0;
        bool have_lo = false;
        for (uint32_t pc = bctr - 4; pc >= fstart && pc + 40 >= bctr; pc -= 4) {
            uint32_t i = img_.word(pc);
            if (rc < 0 && OP(i) == 31 && XO10(i) == 467 && SPR(i) == 9) rc = (int)RD(i);                 // mtctr
            else if (rc >= 0 && ra < 0 && OP(i) == 31 && XO10(i) == 23 && (int)RD(i) == rc) ra = (int)RA(i);  // lwzx
            else if (ra >= 0 && !have_lo && OP(i) == 14 && (int)RD(i) == ra && (int)RA(i) == ra) { lo = i & 0xFFFF; have_lo = true; }
            else if (have_lo && OP(i) == 15 && (int)RD(i) == ra) { table = (UIMM(i) << 16) + (uint32_t)(int16_t)lo; break; }
            if (pc == fstart) break;
        }
        std::vector<uint32_t> out;
        if (!table) return out;
        for (uint32_t k = 0; k < 1024; k++) {
            const uint8_t* p = img_.at(table + 4 * k, 4);
            if (!p) break;
            uint32_t t = be32(p);
            if (t < fstart || t >= fend || (t & 3)) break;
            out.push_back(t);
        }
        return out;
    }
    std::ostringstream out_;

    static uint32_t target(uint32_t i, uint32_t pc) {
        if (OP(i) == 18) {
            int32_t li = (int32_t)((i & 0x03FFFFFC) << 6) >> 6;
            return (i & 2) ? (uint32_t)li : pc + li;
        }
        int32_t bd = (int16_t)(i & 0xFFFC);
        return (i & 2) ? (uint32_t)bd : pc + bd;
    }
    bool inside(uint32_t a) const { return a >= cur_->addr && a < cur_->addr + cur_->size; }

    void line(const std::string& s) { out_ << "    " << s << "\n"; }
    void cr0(const std::string& v) { line("s->cr[0] = cmp_s((int32_t)(" + v + "), 0, s->xer_so);"); }
    void bad(uint32_t i, uint32_t pc) {
        unknown++;
        line("ppc_trap(s, " + hex(pc) + "); /* unhandled " + hex(i) + " */");
    }

    // Jump to `t` from the end of a path: local goto or tail call.
    std::string jump(uint32_t t) {
        if (inside(t)) return "goto " + label(t) + ";";
        if (starts_.count(t)) return "{ " + fname(t) + "(s); return; }";
        return "{ ppc_call(s, " + hex(t) + "); return; }";
    }
    std::string call(uint32_t t) {
        if (starts_.count(t)) return fname(t) + "(s);";
        return "ppc_call(s, " + hex(t) + ");";
    }

    // Branch condition from BO/BI; may decrement CTR.
    std::string cond(uint32_t bo, uint32_t bi) {
        std::vector<std::string> parts;
        if (!(bo & 4)) {
            line("s->ctr--;");
            parts.push_back((bo & 2) ? "s->ctr == 0" : "s->ctr != 0");
        }
        if (!(bo & 0x10)) {
            std::string bit = "(s->cr[" + std::to_string(bi >> 2) + "] & " + std::to_string(8 >> (bi & 3)) + ")";
            parts.push_back((bo & 8) ? bit : "!" + bit);
        }
        if (parts.empty()) return "";
        std::string c = parts[0];
        for (size_t k = 1; k < parts.size(); k++) c += " && " + parts[k];
        return c;
    }

    void insn(uint32_t i, uint32_t pc) {
        uint32_t d = RD(i), a = RA(i), b = RB(i);
        std::string next = hex(pc + 4);
        switch (OP(i)) {
        case 3: line("if (0) {} /* twi */ ppc_trap(s, " + hex(pc) + ");"); return;
        case 4: ps(i, pc); return;
        case 7: line(R(d) + " = (uint32_t)((int32_t)" + R(a) + " * " + std::to_string(SIMM(i)) + ");"); return;
        case 8:  // subfic: rD = ~rA + SIMM + 1, CA = carry out
            line("{ uint64_t t = (uint64_t)(uint32_t)~" + R(a) + " + (uint32_t)" + std::to_string(SIMM(i)) + " + 1; " + R(d) +
                 " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }");
            return;
        case 10:
            line("s->cr[" + std::to_string(d >> 2) + "] = cmp_u(" + R(a) + ", " + std::to_string(UIMM(i)) + "u, s->xer_so);");
            return;
        case 11:
            line("s->cr[" + std::to_string(d >> 2) + "] = cmp_s((int32_t)" + R(a) + ", " + std::to_string(SIMM(i)) + ", s->xer_so);");
            return;
        case 12:
        case 13:
            line("{ uint32_t t = " + R(a) + "; " + R(d) + " = t + (uint32_t)" + std::to_string(SIMM(i)) + "; s->xer_ca = " + R(d) + " < t; }");
            if (OP(i) == 13) cr0(R(d));
            return;
        case 14:
            line(R(d) + " = " + (a ? R(a) + " + " : std::string()) + "(uint32_t)" + std::to_string(SIMM(i)) + ";");
            return;
        case 15:
            line(R(d) + " = " + (a ? R(a) + " + " : std::string()) + hex(UIMM(i) << 16) + "u;");
            return;
        case 16: {
            uint32_t t = target(i, pc);
            std::string c = cond(RD(i), RA(i));
            if (i & 1) {  // bcl
                line("if (" + (c.empty() ? std::string("1") : c) + ") { s->lr = " + next + "; " + call(t) + " }");
            } else {
                line((c.empty() ? std::string() : "if (" + c + ") ") + jump(t));
            }
            return;
        }
        case 17: line("ppc_syscall(s, " + hex(pc) + ");"); return;
        case 18: {
            uint32_t t = target(i, pc);
            if ((i & 1) && t == save_context_) {
                // setjmp must run in this frame so OSLoadContext can longjmp back here.
                line("s->lr = " + next + "; if (setjmp(*ppc_context_jmpbuf(s->r[3]))) {} else { ppc_context_saved(s); s->r[3] = 0; }");
            } else if (i & 1) {
                line("s->lr = " + next + "; " + call(t));
            } else {
                line(jump(t));
            }
            return;
        }
        case 19: op19(i, pc); return;
        case 20: {
            uint32_t m = rot_mask(RC(i), (i >> 1) & 31);
            line(R(a) + " = (rotl32(" + R(d) + ", " + std::to_string(b) + ") & " + hex(m) + "u) | (" + R(a) + " & " + hex(~m) + "u);");
            if (RCB(i)) cr0(R(a));
            return;
        }
        case 21: {
            uint32_t m = rot_mask(RC(i), (i >> 1) & 31);
            line(R(a) + " = rotl32(" + R(d) + ", " + std::to_string(b) + ") & " + hex(m) + "u;");
            if (RCB(i)) cr0(R(a));
            return;
        }
        case 23: {
            uint32_t m = rot_mask(RC(i), (i >> 1) & 31);
            line(R(a) + " = rotl32(" + R(d) + ", " + R(b) + ") & " + hex(m) + "u;");
            if (RCB(i)) cr0(R(a));
            return;
        }
        case 24: if (i) line(R(a) + " = " + R(d) + " | " + hex(UIMM(i)) + "u;"); return;  // 0x60000000 = nop
        case 25: line(R(a) + " = " + R(d) + " | " + hex(UIMM(i) << 16) + "u;"); return;
        case 26: line(R(a) + " = " + R(d) + " ^ " + hex(UIMM(i)) + "u;"); return;
        case 27: line(R(a) + " = " + R(d) + " ^ " + hex(UIMM(i) << 16) + "u;"); return;
        case 28: line(R(a) + " = " + R(d) + " & " + hex(UIMM(i)) + "u;"); cr0(R(a)); return;
        case 29: line(R(a) + " = " + R(d) + " & " + hex(UIMM(i) << 16) + "u;"); cr0(R(a)); return;
        case 31: op31(i, pc); return;
        // ---- integer loads/stores, D-form
        case 32: case 33: load_d(i, "rd32", "", OP(i) == 33); return;
        case 34: case 35: load_d(i, "rd8", "", OP(i) == 35); return;
        case 40: case 41: load_d(i, "rd16", "", OP(i) == 41); return;
        case 42: case 43: load_d(i, "rd16", "(uint32_t)(int32_t)(int16_t)", OP(i) == 43); return;
        case 36: case 37: store_d(i, "wr32", "", OP(i) == 37); return;
        case 38: case 39: store_d(i, "wr8", "(uint8_t)", OP(i) == 39); return;
        case 44: case 45: store_d(i, "wr16", "(uint16_t)", OP(i) == 45); return;
        case 46:
            line("{ uint32_t ea = " + ea_d(i) + "; for (int n = " + std::to_string(d) + "; n < 32; n++, ea += 4) s->r[n] = rd32(ea); }");
            return;
        case 47:
            line("{ uint32_t ea = " + ea_d(i) + "; for (int n = " + std::to_string(d) + "; n < 32; n++, ea += 4) wr32(ea, s->r[n]); }");
            return;
        // ---- float loads/stores
        case 48: case 49:
            line("{ uint32_t ea = " + ea_d(i) + "; " + F(d) + " = rdf32(ea); " + P1(d) + " = " + F(d) + ";" +
                 (OP(i) == 49 ? " " + R(a) + " = ea;" : "") + " }");
            return;
        case 50: case 51:
            line("{ uint32_t ea = " + ea_d(i) + "; " + FU(d) + " = rd64(ea);" + (OP(i) == 51 ? " " + R(a) + " = ea;" : "") + " }");
            return;
        case 52: case 53:
            line("{ uint32_t ea = " + ea_d(i) + "; wrf32(ea, " + F(d) + ");" + (OP(i) == 53 ? " " + R(a) + " = ea;" : "") + " }");
            return;
        case 54: case 55:
            line("{ uint32_t ea = " + ea_d(i) + "; wr64(ea, " + FU(d) + ");" + (OP(i) == 55 ? " " + R(a) + " = ea;" : "") + " }");
            return;
        case 56: case 57: case 60: case 61: {
            int32_t off = (int32_t)(i << 20) >> 20;
            std::string base = a ? R(a) : "0";
            std::string ea = "(uint32_t)(" + base + " + " + std::to_string(off) + ")";
            std::string w = std::to_string((i >> 15) & 1), q = std::to_string((i >> 12) & 7);
            bool upd = OP(i) == 57 || OP(i) == 61;
            const char* fn = OP(i) < 60 ? "psq_load" : "psq_store";
            line(std::string("{ uint32_t ea = ") + ea + "; " + fn + "(s, " + std::to_string(d) + ", ea, " + w + ", " + q + ");" +
                 (upd ? " " + R(a) + " = ea;" : "") + " }");
            return;
        }
        case 59: fp59(i, pc); return;
        case 63: fp63(i, pc); return;
        default: bad(i, pc); return;
        }
    }

    void load_d(uint32_t i, const char* fn, const char* cast, bool update) {
        line("{ uint32_t ea = " + ea_d(i) + "; " + R(RD(i)) + " = " + cast + fn + "(ea);" + (update ? " " + R(RA(i)) + " = ea;" : "") + " }");
    }
    void store_d(uint32_t i, const char* fn, const char* cast, bool update) {
        line("{ uint32_t ea = " + ea_d(i) + "; " + fn + "(ea, " + cast + R(RD(i)) + ");" + (update ? " " + R(RA(i)) + " = ea;" : "") + " }");
    }
    void load_x(uint32_t i, const char* fn, const char* cast, bool update) {
        line("{ uint32_t ea = " + ea_x(i) + "; " + R(RD(i)) + " = " + cast + fn + "(ea);" + (update ? " " + R(RA(i)) + " = ea;" : "") + " }");
    }
    void store_x(uint32_t i, const char* fn, const char* cast, bool update) {
        line("{ uint32_t ea = " + ea_x(i) + "; " + fn + "(ea, " + cast + R(RD(i)) + ");" + (update ? " " + R(RA(i)) + " = ea;" : "") + " }");
    }

    void op19(uint32_t i, uint32_t pc) {
        uint32_t bo = RD(i), bi = RA(i);
        std::string next = hex(pc + 4);
        auto crbit = [](uint32_t n) { return "((s->cr[" + std::to_string(n >> 2) + "] >> " + std::to_string(3 - (n & 3)) + ") & 1)"; };
        auto crop = [&](const std::string& expr) {
            uint32_t dd = RD(i);
            line("{ uint32_t v = (" + expr + ") & 1; s->cr[" + std::to_string(dd >> 2) + "] = (uint8_t)((s->cr[" +
                 std::to_string(dd >> 2) + "] & ~" + std::to_string(8 >> (dd & 3)) + ") | (v << " + std::to_string(3 - (dd & 3)) + ")); }");
        };
        std::string A = crbit(RA(i)), B = crbit(RB(i));
        switch (XO10(i)) {
        case 0: line("s->cr[" + std::to_string(RD(i) >> 2) + "] = s->cr[" + std::to_string(RA(i) >> 2) + "];"); return;  // mcrf
        case 16: {  // bclr
            std::string c = cond(bo, bi);
            if (i & 1) {
                line("if (" + (c.empty() ? std::string("1") : c) + ") { uint32_t t = s->lr; s->lr = " + next + "; ppc_call(s, t); }");
            } else {
                line((c.empty() ? std::string() : "if (" + c + ") ") + "return;");
            }
            return;
        }
        case 528: {  // bcctr
            std::string c = cond(bo | 4, bi);
            if (i & 1) {
                line("if (" + (c.empty() ? std::string("1") : c) + ") { s->lr = " + next + "; ppc_call(s, s->ctr); }");
                return;
            }
            auto jt = jt_by_func_.find(cur_->addr);
            auto local = switch_.find(pc);
            std::string pre = c.empty() ? std::string() : "if (" + c + ") ";
            const std::vector<uint32_t>* targets = local != switch_.end() ? &local->second
                                                   : jt != jt_by_func_.end() ? &jt->second : nullptr;
            if (targets) {
                std::string sw = "switch (s->ctr) {";
                std::set<uint32_t> seen;
                for (uint32_t t : *targets)
                    if (seen.insert(t).second) sw += " case " + hex(t) + ": goto " + label(t) + ";";
                sw += " default: ppc_call(s, s->ctr); return; }";
                line(pre + "{ " + sw + " }");
            } else {
                line(pre + "{ ppc_call(s, s->ctr); return; }");
            }
            return;
        }
        case 33: crop("~(" + A + " | " + B + ")"); return;   // crnor
        case 129: crop(A + " & ~" + B); return;              // crandc
        case 193: crop(A + " ^ " + B); return;               // crxor
        case 225: crop("~(" + A + " & " + B + ")"); return;  // crnand
        case 257: crop(A + " & " + B); return;               // crand
        case 289: crop("~(" + A + " ^ " + B + ")"); return;  // creqv
        case 417: crop(A + " | ~" + B); return;              // crorc
        case 449: crop(A + " | " + B); return;               // cror
        case 150: return;                                    // isync
        case 50:  // rfi: continue at SRR0 (a return when SRR0 is the link register)
            line("{ uint32_t t = s->spr[26] | 0x80000000u; s->msr = s->spr[27]; if (t == (s->lr | 0x80000000u)) return; "
                 "ppc_call(s, t); return; }");
            return;
        default: bad(i, pc); return;
        }
    }

    void op31(uint32_t i, uint32_t pc) {
        uint32_t d = RD(i), a = RA(i), b = RB(i);
        std::string rd = R(d), ra = R(a), rb = R(b);
        uint32_t xo = XO10(i);
        bool rc = RCB(i);
        auto done = [&](const std::string& reg) { if (rc) cr0(reg); };
        // XO-form arithmetic: OE is bit 21 (xo & 0x200); treat as the base op.
        switch (xo & 0x1FF) {
        case 266: line(rd + " = " + ra + " + " + rb + ";"); done(rd); return;                                  // add
        case 10: line("{ uint32_t t = " + ra + " + " + rb + "; s->xer_ca = t < " + ra + "; " + rd + " = t; }"); done(rd); return;  // addc
        case 138: line("{ uint64_t t = (uint64_t)" + ra + " + " + rb + " + s->xer_ca; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 234: line("{ uint64_t t = (uint64_t)" + ra + " + 0xFFFFFFFFu + s->xer_ca; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 202: line("{ uint64_t t = (uint64_t)" + ra + " + s->xer_ca; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 40: line(rd + " = " + rb + " - " + ra + ";"); done(rd); return;                                   // subf
        case 8: line("{ uint64_t t = (uint64_t)(uint32_t)~" + ra + " + " + rb + " + 1; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 136: line("{ uint64_t t = (uint64_t)(uint32_t)~" + ra + " + " + rb + " + s->xer_ca; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 232: line("{ uint64_t t = (uint64_t)(uint32_t)~" + ra + " + 0xFFFFFFFFu + s->xer_ca; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 200: line("{ uint64_t t = (uint64_t)(uint32_t)~" + ra + " + s->xer_ca; " + rd + " = (uint32_t)t; s->xer_ca = (uint32_t)(t >> 32); }"); done(rd); return;
        case 104: line(rd + " = (uint32_t)-(int32_t)" + ra + ";"); done(rd); return;                           // neg
        case 235: line(rd + " = (uint32_t)((int32_t)" + ra + " * (int32_t)" + rb + ");"); done(rd); return;    // mullw
        case 75: line(rd + " = (uint32_t)(((int64_t)(int32_t)" + ra + " * (int32_t)" + rb + ") >> 32);"); done(rd); return;
        case 11: line(rd + " = (uint32_t)(((uint64_t)" + ra + " * " + rb + ") >> 32);"); done(rd); return;
        case 491: line(rd + " = (" + rb + " == 0 || (" + ra + " == 0x80000000u && " + rb + " == 0xFFFFFFFFu)) ? 0 : (uint32_t)((int32_t)" + ra + " / (int32_t)" + rb + ");"); done(rd); return;
        case 459: line(rd + " = " + rb + " ? " + ra + " / " + rb + " : 0;"); done(rd); return;
        default: break;
        }
        std::string rs = R(d);  // X-form logical ops: rS in the rD slot, result in rA
        switch (xo) {
        case 0:
            line("s->cr[" + std::to_string(d >> 2) + "] = cmp_s((int32_t)" + ra + ", (int32_t)" + rb + ", s->xer_so);");
            return;
        case 32:
            line("s->cr[" + std::to_string(d >> 2) + "] = cmp_u(" + ra + ", " + rb + ", s->xer_so);");
            return;
        case 4: line("ppc_trap(s, " + hex(pc) + "); /* tw */"); return;
        case 28: line(ra + " = " + rs + " & " + rb + ";"); done(ra); return;
        case 60: line(ra + " = " + rs + " & ~" + rb + ";"); done(ra); return;
        case 444: line(ra + " = " + rs + " | " + rb + ";"); done(ra); return;
        case 412: line(ra + " = " + rs + " | ~" + rb + ";"); done(ra); return;
        case 124: line(ra + " = ~(" + rs + " | " + rb + ");"); done(ra); return;
        case 476: line(ra + " = ~(" + rs + " & " + rb + ");"); done(ra); return;
        case 316: line(ra + " = " + rs + " ^ " + rb + ";"); done(ra); return;
        case 284: line(ra + " = ~(" + rs + " ^ " + rb + ");"); done(ra); return;
        case 26: line("{ uint32_t v = " + rs + "; uint32_t n = 0; while (n < 32 && !(v & (0x80000000u >> n))) n++; " + ra + " = n; }"); done(ra); return;
        case 922: line(ra + " = (uint32_t)(int32_t)(int16_t)" + rs + ";"); done(ra); return;
        case 954: line(ra + " = (uint32_t)(int32_t)(int8_t)" + rs + ";"); done(ra); return;
        case 24: line("{ uint32_t n = " + rb + " & 63; " + ra + " = n > 31 ? 0 : " + rs + " << n; }"); done(ra); return;
        case 536: line("{ uint32_t n = " + rb + " & 63; " + ra + " = n > 31 ? 0 : " + rs + " >> n; }"); done(ra); return;
        case 792:
            line("{ uint32_t n = " + rb + " & 63; int32_t v = (int32_t)" + rs + "; if (n > 31) { " + ra +
                 " = (uint32_t)(v >> 31); s->xer_ca = v < 0; } else { " + ra + " = (uint32_t)(v >> n); s->xer_ca = v < 0 && n && (v & ((1u << n) - 1)); } }");
            done(ra);
            return;
        case 824: {
            uint32_t n = b;
            line("{ int32_t v = (int32_t)" + rs + "; " + ra + " = (uint32_t)(v >> " + std::to_string(n) + "); s->xer_ca = v < 0 && (v & " +
                 hex(n ? (1u << n) - 1 : 0) + "u) != 0; }");
            done(ra);
            return;
        }
        // ---- loads/stores, X-form
        case 23: load_x(i, "rd32", "", false); return;
        case 55: load_x(i, "rd32", "", true); return;
        case 87: load_x(i, "rd8", "", false); return;
        case 119: load_x(i, "rd8", "", true); return;
        case 279: load_x(i, "rd16", "", false); return;
        case 311: load_x(i, "rd16", "", true); return;
        case 343: load_x(i, "rd16", "(uint32_t)(int32_t)(int16_t)", false); return;
        case 375: load_x(i, "rd16", "(uint32_t)(int32_t)(int16_t)", true); return;
        case 534: line("{ uint32_t v = rd32(" + ea_x(i) + "); " + rd + " = PPC_BSWAP32(v); }"); return;  // lwbrx
        case 790: line("{ uint16_t v = rd16(" + ea_x(i) + "); " + rd + " = PPC_BSWAP16(v); }"); return;  // lhbrx
        case 20: line("{ uint32_t ea = " + ea_x(i) + "; " + rd + " = rd32(ea); s->reserve = 1; s->reserve_addr = ea; }"); return;
        case 151: store_x(i, "wr32", "", false); return;
        case 183: store_x(i, "wr32", "", true); return;
        case 215: store_x(i, "wr8", "(uint8_t)", false); return;
        case 247: store_x(i, "wr8", "(uint8_t)", true); return;
        case 407: store_x(i, "wr16", "(uint16_t)", false); return;
        case 439: store_x(i, "wr16", "(uint16_t)", true); return;
        case 662: line("wr32(" + ea_x(i) + ", PPC_BSWAP32(" + rs + "));"); return;
        case 918: line("wr16(" + ea_x(i) + ", PPC_BSWAP16((uint16_t)" + rs + "));"); return;
        case 150:
            line("{ uint32_t ea = " + ea_x(i) + "; bool ok = s->reserve && s->reserve_addr == ea; if (ok) wr32(ea, " + rs +
                 "); s->reserve = 0; s->cr[0] = (uint8_t)((ok ? 2 : 0) | (s->xer_so & 1)); }");
            return;
        case 535: case 567:
            line("{ uint32_t ea = " + ea_x(i) + "; " + F(d) + " = rdf32(ea); " + P1(d) + " = " + F(d) + ";" + (xo == 567 ? " " + ra + " = ea;" : "") + " }");
            return;
        case 599: case 631:
            line("{ uint32_t ea = " + ea_x(i) + "; " + FU(d) + " = rd64(ea);" + (xo == 631 ? " " + ra + " = ea;" : "") + " }");
            return;
        case 663: case 695:
            line("{ uint32_t ea = " + ea_x(i) + "; wrf32(ea, " + F(d) + ");" + (xo == 695 ? " " + ra + " = ea;" : "") + " }");
            return;
        case 727: case 759:
            line("{ uint32_t ea = " + ea_x(i) + "; wr64(ea, " + FU(d) + ");" + (xo == 759 ? " " + ra + " = ea;" : "") + " }");
            return;
        case 983: line("wr32(" + ea_x(i) + ", (uint32_t)" + FU(d) + ");"); return;  // stfiwx
        case 597:  // lswi
            line("{ uint32_t ea = " + (a ? ra : std::string("0")) + "; uint32_t n = " + std::to_string(b ? b : 32) + "; int r = " +
                 std::to_string(d) + " - 1; for (uint32_t k = 0; k < n; k++) { if ((k & 3) == 0) { r = (r + 1) & 31; s->r[r] = 0; } "
                 "s->r[r] |= (uint32_t)rd8(ea + k) << (24 - 8 * (k & 3)); } }");
            return;
        case 725:  // stswi
            line("{ uint32_t ea = " + (a ? ra : std::string("0")) + "; uint32_t n = " + std::to_string(b ? b : 32) + "; int r = " +
                 std::to_string(d) + " - 1; for (uint32_t k = 0; k < n; k++) { if ((k & 3) == 0) r = (r + 1) & 31; "
                 "wr8(ea + k, (uint8_t)(s->r[r] >> (24 - 8 * (k & 3)))); } }");
            return;
        // ---- system
        case 19: line(rd + " = cr_get(s);"); return;
        case 144: line("cr_set(s, " + rs + ", " + std::to_string((i >> 12) & 0xFF) + ");"); return;
        case 512: line("s->cr[" + std::to_string(d >> 2) + "] = (uint8_t)(xer_get(s) >> 28); s->xer_so = s->xer_ov = s->xer_ca = 0;"); return;
        case 83: line(rd + " = s->msr;"); return;
        case 146: line("ppc_mtmsr(s, " + rs + ");"); return;
        case 339: {
            uint32_t spr = SPR(i);
            if (spr == 8) line(rd + " = s->lr;");
            else if (spr == 9) line(rd + " = s->ctr;");
            else if (spr == 1) line(rd + " = xer_get(s);");
            else if (spr >= 912 && spr <= 919) line(rd + " = s->gqr[" + std::to_string(spr - 912) + "];");
            else line(rd + " = ppc_mfspr(s, " + std::to_string(spr) + ");");
            return;
        }
        case 467: {
            uint32_t spr = SPR(i);
            if (spr == 8) line("s->lr = " + rs + ";");
            else if (spr == 9) line("s->ctr = " + rs + ";");
            else if (spr == 1) line("xer_set(s, " + rs + ");");
            else if (spr >= 912 && spr <= 919) line("s->gqr[" + std::to_string(spr - 912) + "] = " + rs + ";");
            else line("ppc_mtspr(s, " + std::to_string(spr) + ", " + rs + ");");
            return;
        }
        case 371: {
            uint32_t tbr = SPR(i);
            line(rd + " = (uint32_t)(ppc_timebase() >> " + std::string(tbr == 269 ? "32" : "0") + ");");
            return;
        }
        case 595: case 659: line(rd + " = 0; /* mfsr */"); return;
        case 210: case 242: return;  // mtsr, mtsrin
        case 1014: line("{ uint32_t ea = " + ea_x(i) + " & ~31u; for (int k = 0; k < 32; k += 4) wr32(ea + k, 0); }"); return;  // dcbz
        case 54: case 86: case 246: case 278: case 470: case 982: case 598: case 854: case 306: case 566: return;  // cache/sync no-ops
        default: bad(i, pc); return;
        }
    }

    void fp59(uint32_t i, uint32_t pc) {
        uint32_t d = RD(i), a = RA(i), b = RB(i), c = RC(i);
        std::string fd = F(d), fa = F(a), fb = F(b), fc = F(c);
        auto set = [&](const std::string& e) { line(fd + " = ppc_single(" + e + "); " + P1(d) + " = " + fd + ";"); };
        switch (XO5(i)) {
        case 18: set(fa + " / " + fb); return;
        case 20: set(fa + " - " + fb); return;
        case 21: set(fa + " + " + fb); return;
        case 22: set("std::sqrt(" + fb + ")"); return;
        case 24: set("ppc_fres(" + fb + ")"); return;
        case 25: set(fa + " * " + fc); return;
        case 28: set("std::fma(" + fa + ", " + fc + ", -" + fb + ")"); return;
        case 29: set("std::fma(" + fa + ", " + fc + ", " + fb + ")"); return;
        case 30: set("-std::fma(" + fa + ", " + fc + ", -" + fb + ")"); return;
        case 31: set("-std::fma(" + fa + ", " + fc + ", " + fb + ")"); return;
        default: bad(i, pc); return;
        }
    }

    void fp63(uint32_t i, uint32_t pc) {
        uint32_t d = RD(i), a = RA(i), b = RB(i), c = RC(i);
        std::string fd = F(d), fa = F(a), fb = F(b), fc = F(c);
        switch (XO5(i)) {
        case 18: line(fd + " = " + fa + " / " + fb + ";"); return;
        case 20: line(fd + " = " + fa + " - " + fb + ";"); return;
        case 21: line(fd + " = " + fa + " + " + fb + ";"); return;
        case 22: line(fd + " = std::sqrt(" + fb + ");"); return;
        case 23: line(fd + " = " + fa + " >= 0.0 ? " + fc + " : " + fb + ";"); return;
        case 25: line(fd + " = " + fa + " * " + fc + ";"); return;
        case 26: line(fd + " = ppc_frsqrte(" + fb + ");"); return;
        case 28: line(fd + " = std::fma(" + fa + ", " + fc + ", -" + fb + ");"); return;
        case 29: line(fd + " = std::fma(" + fa + ", " + fc + ", " + fb + ");"); return;
        case 30: line(fd + " = -std::fma(" + fa + ", " + fc + ", -" + fb + ");"); return;
        case 31: line(fd + " = -std::fma(" + fa + ", " + fc + ", " + fb + ");"); return;
        default: break;
        }
        switch (XO10(i)) {
        case 0: case 32: line("s->cr[" + std::to_string(d >> 2) + "] = cmp_f(" + fa + ", " + fb + ");"); return;
        case 12: line(fd + " = ppc_single(" + fb + ");"); return;
        case 14: line(FU(d) + " = (uint64_t)(uint32_t)fctiw_sat(std::nearbyint(" + fb + "));"); return;
        case 15: line(FU(d) + " = (uint64_t)(uint32_t)fctiw_sat(std::trunc(" + fb + "));"); return;
        case 40: line(fd + " = -" + fb + ";"); return;
        case 72: line(FU(d) + " = " + FU(b) + ";"); return;
        case 136: line(fd + " = -std::fabs(" + fb + ");"); return;
        case 264: line(fd + " = std::fabs(" + fb + ");"); return;
        case 583: line(FU(d) + " = s->fpscr;"); return;
        case 711: line("s->fpscr = (uint32_t)" + FU(b) + ";"); return;
        case 38: case 70: case 134: case 64: return;  // mtfsb1, mtfsb0, mtfsfi, mcrfs: rounding mode stays nearest
        default: bad(i, pc); return;
        }
    }

    void ps(uint32_t i, uint32_t pc) {
        uint32_t d = RD(i), a = RA(i), b = RB(i), c = RC(i);
        std::string a0 = F(a), a1 = P1(a), b0 = F(b), b1 = P1(b), c0 = F(c), c1 = P1(c);
        auto set = [&](const std::string& e0, const std::string& e1) {
            line("{ double t0 = ppc_single(" + e0 + "), t1 = ppc_single(" + e1 + "); " + F(d) + " = t0; " + P1(d) + " = t1; }");
        };
        switch (XO5(i)) {
        case 18: set(a0 + " / " + b0, a1 + " / " + b1); return;
        case 20: set(a0 + " - " + b0, a1 + " - " + b1); return;
        case 21: set(a0 + " + " + b0, a1 + " + " + b1); return;
        case 23: set(a0 + " >= 0.0 ? " + c0 + " : " + b0, a1 + " >= 0.0 ? " + c1 + " : " + b1); return;
        case 24: set("ppc_fres(" + b0 + ")", "ppc_fres(" + b1 + ")"); return;
        case 25: set(a0 + " * " + c0, a1 + " * " + c1); return;
        case 26: set("ppc_frsqrte(" + b0 + ")", "ppc_frsqrte(" + b1 + ")"); return;
        case 28: set("std::fma(" + a0 + ", " + c0 + ", -" + b0 + ")", "std::fma(" + a1 + ", " + c1 + ", -" + b1 + ")"); return;
        case 29: set("std::fma(" + a0 + ", " + c0 + ", " + b0 + ")", "std::fma(" + a1 + ", " + c1 + ", " + b1 + ")"); return;
        case 30: set("-std::fma(" + a0 + ", " + c0 + ", -" + b0 + ")", "-std::fma(" + a1 + ", " + c1 + ", -" + b1 + ")"); return;
        case 31: set("-std::fma(" + a0 + ", " + c0 + ", " + b0 + ")", "-std::fma(" + a1 + ", " + c1 + ", " + b1 + ")"); return;
        case 10: set(a0 + " + " + b1, c1); return;  // ps_sum0
        case 11: set(c0, a0 + " + " + b1); return;  // ps_sum1
        case 12: set(a0 + " * " + c0, a1 + " * " + c0); return;  // ps_muls0
        case 13: set(a0 + " * " + c1, a1 + " * " + c1); return;  // ps_muls1
        case 14: set("std::fma(" + a0 + ", " + c0 + ", " + b0 + ")", "std::fma(" + a1 + ", " + c0 + ", " + b1 + ")"); return;
        case 15: set("std::fma(" + a0 + ", " + c1 + ", " + b0 + ")", "std::fma(" + a1 + ", " + c1 + ", " + b1 + ")"); return;
        case 6: case 7: {  // psq_lx / psq_stx (and update forms below via XO10)
            break;
        }
        default: break;
        }
        uint32_t xo = XO10(i);
        uint32_t xo6 = (i >> 1) & 0x3F;
        if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {
            std::string w = std::to_string((i >> 10) & 1), q = std::to_string((i >> 7) & 7);
            bool upd = xo6 >= 38;
            const char* fn = (xo6 & 1) ? "psq_store" : "psq_load";
            line(std::string("{ uint32_t ea = ") + ea_x(i) + "; " + fn + "(s, " + std::to_string(d) + ", ea, " + w + ", " + q + ");" +
                 (upd ? " " + R(a) + " = ea;" : "") + " }");
            return;
        }
        auto mv = [&](const std::string& e0, const std::string& e1) {
            line("{ double t0 = " + e0 + ", t1 = " + e1 + "; " + F(d) + " = t0; " + P1(d) + " = t1; }");
        };
        switch (xo) {
        case 0: case 32: line("s->cr[" + std::to_string(d >> 2) + "] = cmp_f(" + a0 + ", " + b0 + ");"); return;
        case 64: case 96: line("s->cr[" + std::to_string(d >> 2) + "] = cmp_f(" + a1 + ", " + b1 + ");"); return;
        case 40: mv("-" + b0, "-" + b1); return;
        case 72: mv(b0, b1); return;
        case 136: mv("-std::fabs(" + b0 + ")", "-std::fabs(" + b1 + ")"); return;
        case 264: mv("std::fabs(" + b0 + ")", "std::fabs(" + b1 + ")"); return;
        case 528: mv(a0, b0); return;
        case 560: mv(a0, b1); return;
        case 592: mv(a1, b0); return;
        case 624: mv(a1, b1); return;
        case 1014: line("ppc_dcbz_l(" + ea_x(i) + " & ~31u);"); return;
        default: bad(i, pc); return;
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: ppcrecomp <main.dol> <symbols.txt> <out-dir> [--hle names.txt] [--files N]\n");
        return 2;
    }
    std::string outdir = argv[3];
    std::set<std::string> hle, trace;
    size_t files = 32;
    for (int k = 4; k + 1 < argc; k += 2) {
        if (!strcmp(argv[k], "--hle")) {
            std::ifstream f(argv[k + 1]);
            std::string n;
            while (std::getline(f, n)) {
                n.erase(0, n.find_first_not_of(" \t\r"));
                n.erase(n.find_last_not_of(" \t\r") + 1);
                if (n.rfind("trace:", 0) == 0) trace.insert(n.substr(6));
                else if (!n.empty() && n[0] != '#') hle.insert(n);
            }
        } else if (!strcmp(argv[k], "--files")) {
            files = strtoul(argv[k + 1], nullptr, 10);
        }
    }

    Image img;
    if (!load_dol(argv[1], img)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    std::vector<Function> funcs;
    std::vector<std::pair<uint32_t, uint32_t>> jts;
    load_symbols(argv[2], funcs, jts);

    // Calls into the middle of a function (e.g. CodeWarrior's _savegpr_N /
    // _restfpr_N entry labels) get their own entry: the tail of that function.
    {
        std::set<uint32_t> known;
        for (auto& f : funcs) known.insert(f.addr);
        std::map<uint32_t, Function> extra;
        for (auto& f : funcs) {
            for (uint32_t pc = f.addr; pc < f.addr + f.size; pc += 4) {
                uint32_t i = img.word(pc);
                if (OP(i) != 18 || !(i & 1)) continue;
                int32_t li = (int32_t)((i & 0x03FFFFFC) << 6) >> 6;
                uint32_t t = (i & 2) ? (uint32_t)li : pc + li;
                if (known.count(t) || extra.count(t)) continue;
                auto it = std::upper_bound(funcs.begin(), funcs.end(), t, [](uint32_t v, const Function& g) { return v < g.addr; });
                if (it == funcs.begin()) continue;
                --it;
                if (t < it->addr + it->size) {
                    char nm[64];
                    snprintf(nm, sizeof nm, "%s+0x%X", it->name.c_str(), t - it->addr);
                    extra[t] = {t, it->addr + it->size - t, nm};
                }
            }
        }
        for (auto& kv : extra) funcs.push_back(kv.second);
        std::sort(funcs.begin(), funcs.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
        printf("mid-function call entries: %zu\n", extra.size());
    }

    std::set<uint32_t> starts;
    for (auto& f : funcs) starts.insert(f.addr);

    // Assign each jump table to the function its entries point into.
    std::map<uint32_t, std::vector<uint32_t>> jt_by_func;
    for (auto& jt : jts) {
        for (uint32_t off = 0; off + 4 <= jt.second; off += 4) {
            uint32_t t = img.word(jt.first + off);
            auto it = std::upper_bound(funcs.begin(), funcs.end(), t, [](uint32_t v, const Function& f) { return v < f.addr; });
            if (it == funcs.begin()) continue;
            --it;
            if (t < it->addr + it->size) {
                auto& v = jt_by_func[it->addr];
                if (std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
            }
        }
    }

    uint32_t save_context = 0;
    for (auto& f : funcs)
        if (f.name == "OSSaveContext") save_context = f.addr;
    Emitter em(img, funcs, jt_by_func, starts, save_context);
    em.traced_ = trace;
    const char* prelude = "// Generated by ppcrecomp. Do not edit.\n#include \"funcs.h\"\n\n";

    std::ofstream hdr(outdir + "/funcs.h");
    hdr << "// Generated by ppcrecomp. Do not edit.\n#pragma once\n#include \"runtime/ppc.h\"\n\n";
    for (auto& f : funcs) hdr << "void " << fname(f.addr) << "(PPCState* s);\n";
    hdr << "\n";
    for (auto& n : hle) hdr << "void hle_" << n << "(PPCState* s);\n";
    hdr << "\nextern const PPCFuncEntry g_ppc_funcs[];\nextern const uint32_t g_ppc_func_count;\n";
    hdr << "extern const PPCNamedEntry g_ppc_names[];\nextern const uint32_t g_ppc_name_count;\n";
    hdr << "constexpr uint32_t kPpcEntry = " << hex(img.entry) << "u;\n";

    // Always exactly `files` outputs so build systems know them in advance.
    size_t per_file = (funcs.size() + files - 1) / files;
    size_t file_no = 0, in_file = 0, hle_count = 0;
    std::ofstream out;
    for (auto& f : funcs) {
        if (in_file == 0) {
            char nm[64];
            snprintf(nm, sizeof nm, "/recomp_%03zu.cpp", file_no++);
            out.close();
            out.open(outdir + nm);
            out << prelude;
        }
        if (hle.count(f.name)) {
            out << "// " << f.name << " (HLE)\nvoid " << fname(f.addr) << "(PPCState* s) { hle_" << f.name << "(s); }\n\n";
            hle_count++;
        } else {
            out << em.function(f);
        }
        if (++in_file == per_file) in_file = 0;
    }
    out.close();
    for (; file_no < files; file_no++) {
        char nm[64];
        snprintf(nm, sizeof nm, "/recomp_%03zu.cpp", file_no);
        std::ofstream(outdir + nm) << prelude;
    }

    std::ofstream tbl(outdir + "/table.cpp");
    tbl << prelude << "const PPCFuncEntry g_ppc_funcs[] = {\n";
    for (auto& f : funcs) tbl << "    {" << hex(f.addr) << "u, " << fname(f.addr) << "},\n";
    tbl << "};\nconst uint32_t g_ppc_func_count = " << funcs.size() << ";\n";
    tbl << "const PPCNamedEntry g_ppc_names[] = {\n";
    for (auto& f : funcs) tbl << "    {\"" << f.name << "\", " << hex(f.addr) << "u},\n";
    tbl << "};\nconst uint32_t g_ppc_name_count = " << funcs.size() << ";\n";

    printf("functions %zu  files %zu  jump tables %zu  hle %zu  unhandled instructions %d\n",
           funcs.size(), file_no, jts.size(), hle_count, em.unknown);
    return em.unknown ? 3 : 0;
}

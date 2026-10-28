// GX -> OpenGL. Vertex processing (XF: matrices, lighting, texgen) runs on the
// CPU; the TEV pipeline becomes a generated fragment shader. The EFB is an
// offscreen framebuffer; display copies present it, texture copies become
// textures looked up by their destination address.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gx/gx_state.h"
#include "gx/gx_texture.h"
#include "runtime/runtime.h"
#include "video/video.h"

namespace {

uint32_t* const cp = g_gx_cp;
uint32_t* const xf = g_gx_xf;
uint32_t* const bp = g_gx_bp;

bool g_ready = false;
GLuint g_vao = 0, g_vbo = 0;
uint32_t g_frame = 0;

// ---------------------------------------------------------------- vertex
struct Vtx {
    float pos[3];
    float nrm[3];
    float col[2][4];  // 0..1
    float tex[8][2];
    uint8_t pnmtx;
    uint8_t texmtx[8];
};

struct OutVtx {
    float clip[4];
    float col[2][4];
    float tex[8][3];
};

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

const uint8_t* mem(uint32_t phys) {
    const uint8_t* p = ppc_host(0x80000000u | (phys & 0x01FFFFFF));
    static uint8_t zero[64];
    return p ? p : zero;
}

float read_comp(const uint8_t*& p, uint32_t fmt, float scale) {
    switch (fmt) {
    case 0: { float v = *p; p += 1; return v * scale; }
    case 1: { float v = (int8_t)*p; p += 1; return v * scale; }
    case 2: { float v = be16(p); p += 2; return v * scale; }
    case 3: { float v = (int16_t)be16(p); p += 2; return v * scale; }
    default: { union { uint32_t u; float f; } c; c.u = be32(p); p += 4; return c.f; }
    }
}

void read_color(const uint8_t*& p, uint32_t fmt, float* out) {
    uint32_t r, g, b, a = 255;
    switch (fmt) {
    case 0: { uint16_t c = be16(p); p += 2; r = (c >> 11) << 3; g = ((c >> 5) & 63) << 2; b = (c & 31) << 3; break; }
    case 1: r = p[0]; g = p[1]; b = p[2]; p += 3; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; p += 4; break;
    case 3: { uint16_t c = be16(p); p += 2; r = (c >> 12) * 17; g = ((c >> 8) & 15) * 17; b = ((c >> 4) & 15) * 17; a = (c & 15) * 17; break; }
    case 4: {
        uint32_t c = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        p += 3;
        r = ((c >> 18) & 63) * 255 / 63; g = ((c >> 12) & 63) * 255 / 63; b = ((c >> 6) & 63) * 255 / 63; a = (c & 63) * 255 / 63;
        break;
    }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; p += 4; break;
    }
    out[0] = r / 255.f; out[1] = g / 255.f; out[2] = b / 255.f; out[3] = a / 255.f;
}

void decode_vertex(const uint8_t* p, uint32_t vat, Vtx& v) {
    uint32_t lo = cp[0x50], hi = cp[0x60];
    uint32_t a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    v.pnmtx = (uint8_t)(xf[0x1018] & 63);
    static const int tshift[8] = {6, 12, 18, 24, 0, 6, 12, 18};
    for (int k = 0; k < 8; k++) v.texmtx[k] = (uint8_t)((xf[k < 4 ? 0x1018 : 0x1019] >> tshift[k]) & 63);
    if (lo & 1) v.pnmtx = *p++ & 63;
    for (int k = 0; k < 8; k++)
        if (lo & (2u << k)) v.texmtx[k] = *p++ & 63;
    auto index_of = [&](uint32_t type) -> int32_t {
        if (type == 2) return *p++;
        int32_t i = be16(p);
        p += 2;
        return i;
    };
    auto arr = [&](int attr, int32_t idx) { return mem(cp[0xA0 + attr] + (uint32_t)idx * cp[0xB0 + attr]); };
    // Position
    {
        uint32_t t = (lo >> 9) & 3, fmt = (a >> 1) & 7, n = (a & 1) ? 3 : 2;
        float sc = 1.0f / (float)(1u << ((a >> 4) & 31));
        const uint8_t* q = p;
        if (t >= 2) q = arr(0, index_of(t));
        for (uint32_t k = 0; k < 3; k++) v.pos[k] = k < n ? read_comp(q, fmt, sc) : 0.f;
        if (t == 1) p = q;
    }
    // Normal (first vector only; binormals are not used by the lighting model here)
    {
        uint32_t t = (lo >> 11) & 3, fmt = (a >> 10) & 7;
        bool nbt = (a >> 9) & 1, ni3 = (a >> 31) & 1;
        float sc = fmt == 1 ? 1.f / 64 : fmt == 3 ? 1.f / 16384 : 1.f;
        v.nrm[0] = 0; v.nrm[1] = 0; v.nrm[2] = 1;
        if (t == 1) {
            for (int k = 0; k < 3; k++) v.nrm[k] = read_comp(p, fmt, sc);
            if (nbt) for (int k = 0; k < 6; k++) read_comp(p, fmt, sc);
        } else if (t) {
            int32_t i0 = index_of(t);
            if (nbt && ni3) { index_of(t); index_of(t); }
            const uint8_t* q = arr(1, i0);
            for (int k = 0; k < 3; k++) v.nrm[k] = read_comp(q, fmt, sc);
        }
    }
    // Colors
    for (int k = 0; k < 2; k++) {
        uint32_t t = (lo >> (13 + 2 * k)) & 3, fmt = k == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        v.col[k][0] = v.col[k][1] = v.col[k][2] = v.col[k][3] = 1.f;
        if (t == 1) read_color(p, fmt, v.col[k]);
        else if (t) { const uint8_t* q = arr(2 + k, index_of(t)); read_color(q, fmt, v.col[k]); }
    }
    // Texture coordinates
    const uint32_t cnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    const uint32_t fmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    const uint32_t shf[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (int k = 0; k < 8; k++) {
        uint32_t t = (hi >> (2 * k)) & 3;
        v.tex[k][0] = v.tex[k][1] = 0;
        if (!t) continue;
        float sc = 1.0f / (float)(1u << shf[k]);
        const uint8_t* q = p;
        if (t >= 2) q = arr(4 + k, index_of(t));
        v.tex[k][0] = read_comp(q, fmt[k], sc);
        if (cnt[k]) v.tex[k][1] = read_comp(q, fmt[k], sc);
        if (t == 1) p = q;
    }
}

// ---------------------------------------------------------------- XF
void mul34(uint32_t base, const float* in, float* out) {  // 3x4 matrix at XF row address base
    for (int r = 0; r < 3; r++)
        out[r] = gx_xf_float(base + r * 4) * in[0] + gx_xf_float(base + r * 4 + 1) * in[1] + gx_xf_float(base + r * 4 + 2) * in[2] +
                 gx_xf_float(base + r * 4 + 3) * in[3];
}

void unpack_rgba(uint32_t c, float* out) {
    out[0] = (c >> 24) / 255.f; out[1] = ((c >> 16) & 255) / 255.f; out[2] = ((c >> 8) & 255) / 255.f; out[3] = (c & 255) / 255.f;
}

// One lighting channel component set (color = rgb, or alpha).
void light_channel(uint32_t ctrl, uint32_t amb_reg, uint32_t mat_reg, const float* vcol, const float* vpos, const float* vnrm,
                   bool alpha, float* out) {
    float mat[4], amb[4];
    if (ctrl & 1) memcpy(mat, vcol, sizeof mat); else unpack_rgba(mat_reg, mat);
    if (!((ctrl >> 1) & 1)) {
        if (alpha) out[3] = mat[3]; else memcpy(out, mat, 3 * sizeof(float));
        return;
    }
    if ((ctrl >> 6) & 1) memcpy(amb, vcol, sizeof amb); else unpack_rgba(amb_reg, amb);
    float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
    uint32_t mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
    uint32_t diffuse_fn = (ctrl >> 7) & 3;
    bool atten = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
    for (int l = 0; l < 8; l++) {
        if (!(mask & (1u << l))) continue;
        uint32_t base = 0x600 + 0x10 * l;
        float lc[4];
        unpack_rgba(xf[base + 3], lc);
        float lp[3] = {gx_xf_float(base + 10), gx_xf_float(base + 11), gx_xf_float(base + 12)};
        float ld[3] = {gx_xf_float(base + 13), gx_xf_float(base + 14), gx_xf_float(base + 15)};
        float L[3] = {lp[0] - vpos[0], lp[1] - vpos[1], lp[2] - vpos[2]};
        float dist = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (dist > 0) { L[0] /= dist; L[1] /= dist; L[2] /= dist; }
        float ndl = vnrm[0] * L[0] + vnrm[1] * L[1] + vnrm[2] * L[2];
        float diff = diffuse_fn == 0 ? 1.f : diffuse_fn == 1 ? ndl : std::max(ndl, 0.f);
        float att = 1.f;
        if (atten) {
            float a0 = gx_xf_float(base + 4), a1 = gx_xf_float(base + 5), a2 = gx_xf_float(base + 6);
            float k0 = gx_xf_float(base + 7), k1 = gx_xf_float(base + 8), k2 = gx_xf_float(base + 9);
            float cs, d;
            if (spot) {
                cs = std::max(0.f, -(L[0] * ld[0] + L[1] * ld[1] + L[2] * ld[2]));
                d = dist;
            } else {  // specular: half-angle vector in the direction slot
                cs = ndl > 0 ? std::max(0.f, vnrm[0] * ld[0] + vnrm[1] * ld[1] + vnrm[2] * ld[2]) : 0.f;
                d = cs;
            }
            float num = std::max(0.f, a0 + a1 * cs + a2 * cs * cs);
            float den = k0 + k1 * d + k2 * d * d;
            att = den > 0 ? num / den : 0.f;
        }
        for (int k = 0; k < 4; k++) acc[k] += lc[k] * diff * att;
    }
    for (int k = 0; k < 4; k++) acc[k] = std::min(std::max(acc[k], 0.f), 1.f);
    if (alpha) out[3] = mat[3] * acc[3];
    else for (int k = 0; k < 3; k++) out[k] = mat[k] * acc[k];
}

void transform(const Vtx& v, OutVtx& o) {
    float p4[4] = {v.pos[0], v.pos[1], v.pos[2], 1.f}, vp[3];
    uint32_t mbase = (uint32_t)v.pnmtx * 4;
    mul34(mbase, p4, vp);
    // Normal matrix: 3x3 rows of 3 at 0x400 + (pnmtx & 31) * 3
    float n[3];
    uint32_t nb = 0x400 + (v.pnmtx & 31) * 3;
    for (int r = 0; r < 3; r++) n[r] = gx_xf_float(nb + r * 3) * v.nrm[0] + gx_xf_float(nb + r * 3 + 1) * v.nrm[1] + gx_xf_float(nb + r * 3 + 2) * v.nrm[2];
    float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl > 0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }

    // Projection
    float p[6];
    for (int k = 0; k < 6; k++) p[k] = gx_xf_float(0x1020 + k);
    float cx, cy, cz, cw;
    if (xf[0x1026] & 1) {  // orthographic
        cx = p[0] * vp[0] + p[1]; cy = p[2] * vp[1] + p[3]; cz = p[4] * vp[2] + p[5]; cw = 1.f;
    } else {
        cx = p[0] * vp[0] + p[1] * vp[2]; cy = p[2] * vp[1] + p[3] * vp[2]; cz = p[4] * vp[2] + p[5]; cw = -vp[2];
    }
    // Viewport (EFB pixels, +342 offset) and depth range, folded into GL clip space.
    float sx = gx_xf_float(0x101A), sy = gx_xf_float(0x101B), sz = gx_xf_float(0x101C);
    float ox = gx_xf_float(0x101D) - 342.f, oy = gx_xf_float(0x101E) - 342.f, oz = gx_xf_float(0x101F);
    if (uint32_t sofs = bp[0x59]) {  // scissor box offset: raw = (offset + 342) / 2
        ox -= (float)((int)((sofs & 0x3FF) * 2) - 342);
        oy -= (float)((int)(((sofs >> 10) & 0x3FF) * 2) - 342);
    }
    float ex = cx * sx + ox * cw, ey = cy * sy + oy * cw;  // EFB pixel * w
    float ez = (cz * sz + oz * cw) / 16777215.f;           // depth 0..1 * w
    o.clip[0] = ex / (kEfbWidth / 2.f) - cw;
    o.clip[1] = cw - ey / (kEfbHeight / 2.f);
    o.clip[2] = ez * 2.f - cw;
    o.clip[3] = cw;

    // Color channels
    uint32_t nchan = xf[0x1009] & 3;
    for (int ch = 0; ch < 2; ch++) {
        float* out = o.col[ch];
        out[0] = out[1] = out[2] = out[3] = 0.f;
        if ((uint32_t)ch >= nchan) continue;
        light_channel(xf[0x100E + ch], xf[0x100A + ch], xf[0x100C + ch], v.col[ch], vp, n, false, out);
        light_channel(xf[0x1010 + ch], xf[0x100A + ch], xf[0x100C + ch], v.col[ch], vp, n, true, out);
    }

    // Texgen
    uint32_t ntex = xf[0x103F] & 15;
    bool dual = xf[0x1012] & 1;
    for (int t = 0; t < 8; t++) {
        float* out = o.tex[t];
        out[0] = out[1] = 0; out[2] = 1;
        if ((uint32_t)t >= ntex) continue;
        uint32_t g = xf[0x1040 + t];
        uint32_t row = (g >> 7) & 31, type = (g >> 4) & 7;
        float src[4] = {0, 0, 1, 1};
        if (row == 0) { src[0] = v.pos[0]; src[1] = v.pos[1]; src[2] = v.pos[2]; }
        else if (row == 1) { src[0] = v.nrm[0]; src[1] = v.nrm[1]; src[2] = v.nrm[2]; }
        else if (row >= 5 && row <= 12) { src[0] = v.tex[row - 5][0]; src[1] = v.tex[row - 5][1]; }
        else if (row == 2) { src[0] = v.col[0][0]; src[1] = v.col[0][1]; }
        if (!((g >> 2) & 1)) src[2] = 1.f;  // AB11
        float r[3] = {src[0], src[1], 1.f};
        if (type == 0) {
            uint32_t base = (uint32_t)v.texmtx[t] * 4;
            if ((g >> 1) & 1) mul34(base, src, r);
            else {
                for (int k = 0; k < 2; k++)
                    r[k] = gx_xf_float(base + k * 4) * src[0] + gx_xf_float(base + k * 4 + 1) * src[1] + gx_xf_float(base + k * 4 + 2) * src[2] +
                           gx_xf_float(base + k * 4 + 3) * src[3];
                r[2] = 1.f;
            }
        } else if (type == 2 || type == 3) {  // color -> st
            r[0] = o.col[type - 2][0]; r[1] = o.col[type - 2][1]; r[2] = 1.f;
        }
        if (dual) {
            uint32_t pm = xf[0x1050 + t];
            float in4[4] = {r[0], r[1], r[2], 1.f};
            if ((pm >> 8) & 1) {
                float l = std::sqrt(in4[0] * in4[0] + in4[1] * in4[1] + in4[2] * in4[2]);
                if (l > 0) { in4[0] /= l; in4[1] /= l; in4[2] /= l; }
            }
            mul34(0x500 + (pm & 63) * 4, in4, r);
        }
        out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
    }
}

// ---------------------------------------------------------------- textures
struct TexEntry {
    GLuint tex = 0;
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
};
std::unordered_map<uint64_t, TexEntry> g_textures;

struct EfbCopy {
    GLuint tex = 0;
    uint32_t w = 0, h = 0;
    uint64_t frame = 0;
};
std::unordered_map<uint32_t, EfbCopy> g_copies;  // by destination physical address

uint64_t hash_bytes(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t k = 0;
    for (; k + 8 <= n; k += 8) {
        uint64_t v;
        memcpy(&v, p + k, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    for (; k < n; k++) h = (h ^ p[k]) * 1099511628211ull;
    return h;
}

GLuint bind_texture(int map, int unit) {
    uint32_t i = map & 3, hi = map >= 4 ? 0x20 : 0;
    uint32_t mode0 = bp[0x80 + i + hi], img0 = bp[0x88 + i + hi], img3 = bp[0x94 + i + hi], tlut = bp[0x98 + i + hi];
    uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    uint32_t addr = (img3 & 0x00FFFFFF) << 5;
    glActiveTexture(GL_TEXTURE0 + unit);
    GLuint id = 0;
    auto copy = g_copies.find(addr);
    if (copy != g_copies.end()) {
        id = copy->second.tex;
    } else {
        uint32_t size = gx_texture_size(fmt, w, h);
        const uint8_t* src = mem(addr);
        const uint8_t* pal = &g_gx_tmem[((tlut & 0x3FF) << 9) & 0xFFFFF];
        uint32_t pal_fmt = (tlut >> 10) & 3;
        uint64_t key = (uint64_t)addr << 32 ^ (uint64_t)fmt << 28 ^ (uint64_t)w << 14 ^ h;
        uint64_t hsh = hash_bytes(src, size);
        if (fmt >= 8 && fmt <= 10) hsh ^= hash_bytes(pal, fmt == 8 ? 32 : 512) * 31 + pal_fmt;
        TexEntry& e = g_textures[key];
        if (!e.tex) glGenTextures(1, &e.tex);
        glBindTexture(GL_TEXTURE_2D, e.tex);
        if (e.hash != hsh || e.w != w || e.h != h) {
            std::vector<uint32_t> rgba(w * h);
            gx_decode_texture(rgba.data(), src, fmt, w, h, pal, pal_fmt);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            e.hash = hsh; e.w = w; e.h = h;
        }
        id = e.tex;
    }
    glBindTexture(GL_TEXTURE_2D, id);
    static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    uint32_t minf = (mode0 >> 5) & 7;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (minf == 1 || (minf & 4)) ? GL_LINEAR : GL_NEAREST);
    return id;
}

// ---------------------------------------------------------------- TEV shaders
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col0;
layout(location = 2) in vec4 a_col1;
layout(location = 3) in vec3 a_tex[8];
out vec4 v_col0;
out vec4 v_col1;
out vec3 v_tex[8];
void main() {
    gl_Position = a_pos;
    v_col0 = a_col0;
    v_col1 = a_col1;
    for (int i = 0; i < 8; i++) v_tex[i] = a_tex[i];
}
)";

struct Program {
    GLuint id = 0;
    GLint konst = -1, regs = -1, alpha_ref = -1, tex = -1;
};
std::unordered_map<std::string, Program> g_programs;

std::string tev_key() {
    std::string k;
    auto add = [&](uint32_t v) { k.append((const char*)&v, 4); };
    uint32_t gen = bp[0x00];
    uint32_t stages = ((gen >> 10) & 15) + 1;
    add(gen & 0x3C3C7F);  // tex gens, colors, stages, ind stages
    for (uint32_t s = 0; s < stages; s++) { add(bp[0xC0 + 2 * s]); add(bp[0xC1 + 2 * s]); }
    for (uint32_t s = 0; s < (stages + 1) / 2; s++) add(bp[0x28 + s]);
    for (int s = 0; s < 8; s++) add(bp[0xF6 + s]);
    add(bp[0xF3] & 0xFF0000);  // alpha compare functions + logic (refs are uniforms)
    return k;
}

std::string swz(uint32_t table, bool alpha_only) {
    (void)alpha_only;
    uint32_t a = bp[0xF6 + 2 * table], b = bp[0xF7 + 2 * table];
    const char* c = "rgba";
    std::string s;
    s += c[a & 3]; s += c[(a >> 2) & 3]; s += c[b & 3]; s += c[(b >> 2) & 3];
    return s;
}

std::string konst_c(uint32_t sel) {
    static const char* fixed[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    if (sel < 8) return std::string("vec3(") + fixed[sel] + ")";
    if (sel >= 12 && sel < 16) return "u_konst[" + std::to_string(sel - 12) + "].rgb";
    if (sel >= 16) {
        const char* comp = "rgba";
        return std::string("vec3(u_konst[") + std::to_string((sel - 16) & 3) + "]." + comp[(sel - 16) >> 2] + ")";
    }
    return "vec3(1.0)";
}

std::string konst_a(uint32_t sel) {
    static const char* fixed[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
    if (sel < 8) return fixed[sel];
    if (sel >= 16) {
        const char* comp = "rgba";
        return std::string("u_konst[") + std::to_string((sel - 16) & 3) + "]." + comp[(sel - 16) >> 2];
    }
    return "1.0";
}

std::string gen_fragment() {
    uint32_t gen = bp[0x00];
    uint32_t stages = ((gen >> 10) & 15) + 1;
    std::string s = R"(#version 330 core
in vec4 v_col0;
in vec4 v_col1;
in vec3 v_tex[8];
out vec4 o_color;
uniform sampler2D u_tex[8];
uniform vec4 u_konst[4];
uniform vec4 u_regs[4];
uniform ivec4 u_alpha_ref;
void main() {
    vec4 r[4];
    r[0] = u_regs[0]; r[1] = u_regs[1]; r[2] = u_regs[2]; r[3] = u_regs[3];
    vec4 tex = vec4(0.0), ras = vec4(0.0), k = vec4(1.0);
    vec3 ca, cb, cc, cd; float aa, ab, ac, ad, rc, ra;
)";
    static const char* cin[16] = {"r[0].rgb", "vec3(r[0].a)", "r[1].rgb", "vec3(r[1].a)", "r[2].rgb", "vec3(r[2].a)", "r[3].rgb", "vec3(r[3].a)",
                                  "tex.rgb", "vec3(tex.a)", "ras.rgb", "vec3(ras.a)", "vec3(1.0)", "vec3(0.5)", "k.rgb", "vec3(0.0)"};
    static const char* ain[8] = {"r[0].a", "r[1].a", "r[2].a", "r[3].a", "tex.a", "ras.a", "k.a", "0.0"};
    static const char* bias[3] = {"0.0", "0.5", "-0.5"};
    static const char* scale[4] = {"1.0", "2.0", "4.0", "0.5"};
    for (uint32_t st = 0; st < stages; st++) {
        uint32_t order = bp[0x28 + st / 2] >> ((st & 1) * 12);
        uint32_t map = order & 7, coord = (order >> 3) & 7, enable = (order >> 6) & 1, chan = (order >> 7) & 7;
        uint32_t ce = bp[0xC0 + 2 * st], ae = bp[0xC1 + 2 * st];
        uint32_t ksel = bp[0xF6 + st / 2] >> ((st & 1) * 10);
        uint32_t kc = (ksel >> 4) & 31, ka = (ksel >> 9) & 31;
        uint32_t rswap = ae & 3, tswap = (ae >> 2) & 3;
        s += "    // stage " + std::to_string(st) + "\n";
        if (enable) s += "    tex = texture(u_tex[" + std::to_string(map) + "], v_tex[" + std::to_string(coord) + "].xy / (v_tex[" +
                         std::to_string(coord) + "].z == 0.0 ? 1.0 : v_tex[" + std::to_string(coord) + "].z))." + swz(tswap, false) + ";\n";
        else s += "    tex = vec4(1.0);\n";
        if (chan == 0) s += "    ras = v_col0." + swz(rswap, false) + ";\n";
        else if (chan == 1) s += "    ras = v_col1." + swz(rswap, false) + ";\n";
        else s += "    ras = vec4(0.0);\n";
        s += "    k = vec4(" + konst_c(kc) + ", " + konst_a(ka) + ");\n";
        // color
        uint32_t d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
        uint32_t cbias = (ce >> 16) & 3, csub = (ce >> 18) & 1, cclamp = (ce >> 19) & 1, cscale = (ce >> 20) & 3, cdest = (ce >> 22) & 3;
        s += std::string("    ca = ") + cin[a] + "; cb = " + cin[b] + "; cc = clamp(" + cin[c] + ", 0.0, 1.0); cd = " + cin[d] + ";\n";
        if (cbias == 3) {  // compare mode: scale selects the comparison
            static const char* cmp[8] = {
                "(ca.r > cb.r) ? cc : vec3(0.0)", "(abs(ca.r - cb.r) < 0.002) ? cc : vec3(0.0)",
                "(dot(ca.rg, vec2(255.0, 65280.0)) > dot(cb.rg, vec2(255.0, 65280.0))) ? cc : vec3(0.0)",
                "(abs(dot(ca.rg, vec2(255.0, 65280.0)) - dot(cb.rg, vec2(255.0, 65280.0))) < 0.5) ? cc : vec3(0.0)",
                "(dot(ca.rgb, vec3(255.0, 65280.0, 16711680.0)) > dot(cb.rgb, vec3(255.0, 65280.0, 16711680.0))) ? cc : vec3(0.0)",
                "(abs(dot(ca.rgb, vec3(255.0, 65280.0, 16711680.0)) - dot(cb.rgb, vec3(255.0, 65280.0, 16711680.0))) < 0.5) ? cc : vec3(0.0)",
                "mix(vec3(0.0), cc, greaterThan(ca, cb))", "mix(vec3(0.0), cc, lessThan(abs(ca - cb), vec3(0.002)))"};
            s += std::string("    rc = 0.0; { vec3 res = cd + (") + cmp[(cscale << 1) | csub] + ");";
        } else {
            s += std::string("    { vec3 res = (cd ") + (csub ? "-" : "+") + " mix(ca, cb, cc) + vec3(" + bias[cbias] + ")) * " + scale[cscale] + ";";
        }
        s += cclamp ? " res = clamp(res, 0.0, 1.0);" : " res = clamp(res, -4.0, 4.0);";
        s += " r[" + std::to_string(cdest) + "].rgb = res; }\n";
        // alpha
        d = (ae >> 4) & 7; c = (ae >> 7) & 7; b = (ae >> 10) & 7; a = (ae >> 13) & 7;
        uint32_t abias = (ae >> 16) & 3, asub = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3, adest = (ae >> 22) & 3;
        s += std::string("    aa = ") + ain[a] + "; ab = " + ain[b] + "; ac = clamp(" + ain[c] + ", 0.0, 1.0); ad = " + ain[d] + ";\n";
        if (abias == 3) {
            static const char* cmp[8] = {"(aa > ab) ? ac : 0.0", "(abs(aa - ab) < 0.002) ? ac : 0.0", "(aa > ab) ? ac : 0.0",
                                         "(abs(aa - ab) < 0.002) ? ac : 0.0", "(aa > ab) ? ac : 0.0", "(abs(aa - ab) < 0.002) ? ac : 0.0",
                                         "(aa > ab) ? ac : 0.0", "(abs(aa - ab) < 0.002) ? ac : 0.0"};
            s += std::string("    { float res = ad + (") + cmp[(ascale << 1) | asub] + ");";
        } else {
            s += std::string("    { float res = (ad ") + (asub ? "-" : "+") + " mix(aa, ab, ac) + " + bias[abias] + ") * " + scale[ascale] + ";";
        }
        s += aclamp ? " res = clamp(res, 0.0, 1.0);" : " res = clamp(res, -4.0, 4.0);";
        s += " r[" + std::to_string(adest) + "].a = res; }\n";
    }
    // Alpha compare: (a cmp0 ref0) logic (a cmp1 ref1)
    uint32_t ac = bp[0xF3];
    uint32_t c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
    static const char* cmps[8] = {"false", "(a < r0)", "(a == r0)", "(a <= r0)", "(a > r0)", "(a != r0)", "(a >= r0)", "true"};
    std::string t0 = cmps[c0], t1 = cmps[c1];
    size_t pos;
    while ((pos = t1.find("r0")) != std::string::npos) t1.replace(pos, 2, "r1");
    static const char* lg[4] = {" && ", " || ", " != ", " == "};
    s += "    vec4 outc = clamp(r[0], 0.0, 1.0);\n";
    if (!(c0 == 7 && c1 == 7 && logic == 0)) {
        s += "    int a = int(outc.a * 255.0 + 0.5); int r0 = u_alpha_ref.x; int r1 = u_alpha_ref.y;\n";
        s += "    if (!(" + t0 + lg[logic] + t1 + ")) discard;\n";
    }
    s += "    o_color = outc;\n}\n";
    return s;
}

GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        rt_log("gx: shader compile failed: %s", log);
    }
    return sh;
}

Program& program() {
    std::string key = tev_key();
    Program& pr = g_programs[key];
    if (pr.id) return pr;
    static GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, gen_fragment());
    pr.id = glCreateProgram();
    glAttachShader(pr.id, vs);
    glAttachShader(pr.id, fs);
    glLinkProgram(pr.id);
    GLint ok = 0;
    glGetProgramiv(pr.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(pr.id, sizeof log, nullptr, log);
        rt_log("gx: program link failed: %s", log);
    }
    glDeleteShader(fs);
    pr.konst = glGetUniformLocation(pr.id, "u_konst");
    pr.regs = glGetUniformLocation(pr.id, "u_regs");
    pr.alpha_ref = glGetUniformLocation(pr.id, "u_alpha_ref");
    pr.tex = glGetUniformLocation(pr.id, "u_tex");
    glUseProgram(pr.id);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(pr.tex, 8, units);
    return pr;
}

// ---------------------------------------------------------------- PE state
void apply_pipeline_state() {
    int s = video_scale();
    // Scissor (inclusive, +342 offset)
    uint32_t tl = bp[0x20], br = bp[0x21];
    int top = (int)(tl & 0x7FF) - 342, left = (int)((tl >> 12) & 0x7FF) - 342;
    int bottom = (int)(br & 0x7FF) - 342, right = (int)((br >> 12) & 0x7FF) - 342;
    glEnable(GL_SCISSOR_TEST);
    glScissor(left * s, (kEfbHeight - bottom - 1) * s, std::max(0, right - left + 1) * s, std::max(0, bottom - top + 1) * s);
    // Depth
    uint32_t z = bp[0x40];
    static const GLenum zf[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    if (z & 1) { glEnable(GL_DEPTH_TEST); glDepthFunc(zf[(z >> 1) & 7]); } else glDisable(GL_DEPTH_TEST);
    glDepthMask((z >> 4) & 1 ? GL_TRUE : GL_FALSE);
    // Blending
    uint32_t bm = bp[0x41];
    bool has_alpha = (bp[0x43] & 7) == 1;  // RGBA6_Z24
    glColorMask((bm >> 3) & 1, (bm >> 3) & 1, (bm >> 3) & 1, has_alpha && ((bm >> 4) & 1));
    if (bm & 1) {
        static const GLenum src[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
        static const GLenum dst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
        glEnable(GL_BLEND);
        if ((bm >> 11) & 1) { glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); glBlendFunc(GL_ONE, GL_ONE); }
        else { glBlendEquation(GL_FUNC_ADD); glBlendFunc(src[(bm >> 8) & 7], dst[(bm >> 5) & 7]); }
    } else {
        glDisable(GL_BLEND);
    }
    // Culling (BP genmode: 0 none, 1 back, 2 front, 3 all)
    uint32_t cull = (bp[0x00] >> 14) & 3;
    if (cull == 0) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glCullFace(cull == 1 ? GL_BACK : cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK); }
}

std::vector<OutVtx> g_verts, g_tris;

}  // namespace

void gx_render_init() {
    if (!g_ready && glGenVertexArrays) {
        glGenVertexArrays(1, &g_vao);
        glGenBuffers(1, &g_vbo);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        GLsizei stride = sizeof(OutVtx);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(OutVtx, clip));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(OutVtx, col[0]));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(OutVtx, col[1]));
        for (int t = 0; t < 8; t++) {
            glEnableVertexAttribArray(3 + t);
            glVertexAttribPointer(3 + t, 3, GL_FLOAT, GL_FALSE, stride, (void*)(offsetof(OutVtx, tex) + t * 3 * sizeof(float)));
        }
        g_ready = true;
    }
}

void gx_render_draw(uint8_t prim, uint32_t vat, uint32_t count, const uint8_t* data, uint32_t stride) {
    if (!g_ready || !count) return;
    g_verts.resize(count);
    for (uint32_t k = 0; k < count; k++) {
        Vtx v;
        decode_vertex(data + (size_t)k * stride, vat, v);
        transform(v, g_verts[k]);
    }
    GLenum mode = GL_TRIANGLES;
    const std::vector<OutVtx>* verts = &g_verts;
    switch (prim) {
    case 0x80: case 0x88:  // quads -> triangles
        g_tris.clear();
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            const OutVtx* v = &g_verts[q];
            g_tris.push_back(v[0]); g_tris.push_back(v[1]); g_tris.push_back(v[2]);
            g_tris.push_back(v[0]); g_tris.push_back(v[2]); g_tris.push_back(v[3]);
        }
        verts = &g_tris;
        break;
    case 0x90: mode = GL_TRIANGLES; break;
    case 0x98: mode = GL_TRIANGLE_STRIP; break;
    case 0xA0: mode = GL_TRIANGLE_FAN; break;
    case 0xA8: mode = GL_LINES; break;
    case 0xB0: mode = GL_LINE_STRIP; break;
    default: mode = GL_POINTS; break;
    }
    video_bind_efb();
    glViewport(0, 0, kEfbWidth * video_scale(), kEfbHeight * video_scale());
    apply_pipeline_state();
    Program& pr = program();
    glUseProgram(pr.id);
    float konst[16], regs[16];
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++) {
            konst[i * 4 + c] = g_gx_tev.konst[i][c] / 255.f;
            regs[i * 4 + c] = g_gx_tev.color[i][c] / 255.f;
        }
    glUniform4fv(pr.konst, 4, konst);
    glUniform4fv(pr.regs, 4, regs);
    GLint aref[4] = {(GLint)(bp[0xF3] & 0xFF), (GLint)((bp[0xF3] >> 8) & 0xFF), 0, 0};
    glUniform4iv(pr.alpha_ref, 1, aref);
    // Bind the textures the TEV stages use.
    uint32_t stages = ((bp[0x00] >> 10) & 15) + 1;
    uint32_t bound = 0;
    for (uint32_t st = 0; st < stages; st++) {
        uint32_t order = bp[0x28 + st / 2] >> ((st & 1) * 12);
        if (((order >> 6) & 1) && !(bound & (1u << (order & 7)))) {
            bind_texture((int)(order & 7), (int)(order & 7));
            bound |= 1u << (order & 7);
        }
    }
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts->size() * sizeof(OutVtx)), verts->data(), GL_STREAM_DRAW);
    glDrawArrays(mode, 0, (GLsizei)verts->size());
}

void gx_render_bp(uint32_t reg, uint32_t v) {
    if (!g_ready) return;
    switch (reg) {
    case 0x65: {  // TLUT load: from RAM (BP 0x64, >>5) to TMEM
        uint32_t src = (bp[0x64] & 0x00FFFFFF) << 5;
        uint32_t dst = (v & 0x3FF) << 9, bytes = ((v >> 10) & 0x7FF) * 32;
        if (dst + bytes <= sizeof g_gx_tmem) memcpy(&g_gx_tmem[dst], mem(src), bytes);
        break;
    }
    case 0x52: {  // EFB copy execute
        int s = video_scale();
        uint32_t tl = bp[0x49], wh = bp[0x4A];
        int x = (int)(tl & 0x3FF), y = (int)((tl >> 10) & 0x3FF);
        int w = (int)(wh & 0x3FF) + 1, h = (int)((wh >> 10) & 0x3FF) + 1;
        bool to_xfb = (v >> 14) & 1, clear = (v >> 11) & 1;
        if (to_xfb) {
            video_present(x, y, w, h);
            g_frame++;
        } else {
            uint32_t dst = (bp[0x4B] & 0x00FFFFFF) << 5;
            EfbCopy& c = g_copies[dst];
            int tw = (v >> 9) & 1 ? w / 2 : w, th = (v >> 9) & 1 ? h / 2 : h;
            if (!c.tex) glGenTextures(1, &c.tex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, c.tex);
            if (c.w != (uint32_t)tw || c.h != (uint32_t)th) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * s, h * s, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                c.w = (uint32_t)tw; c.h = (uint32_t)th;
            }
            video_bind_efb();
            glDisable(GL_SCISSOR_TEST);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x * s, (kEfbHeight - y - h) * s, w * s, h * s);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            c.frame = g_frame;
        }
        if (clear) {
            video_bind_efb();
            uint32_t ar = bp[0x4F], gb = bp[0x50], zv = bp[0x51] & 0xFFFFFF;
            glEnable(GL_SCISSOR_TEST);
            glScissor(x * s, (kEfbHeight - y - h) * s, w * s, h * s);
            uint32_t bm = bp[0x41];
            glColorMask((bm >> 3) & 1, (bm >> 3) & 1, (bm >> 3) & 1, (bm >> 4) & 1);
            glDepthMask((bp[0x40] >> 4) & 1 ? GL_TRUE : GL_FALSE);
            glClearColor((ar & 0xFF) / 255.f, ((gb >> 8) & 0xFF) / 255.f, (gb & 0xFF) / 255.f, ((ar >> 8) & 0xFF) / 255.f);
            glClearDepth(zv / 16777215.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        break;
    }
    default:
        break;
    }
}

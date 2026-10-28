// SI (serial interface): GameCube controller ports at 0x6400.
//   0x00 + 12*ch  SICxOUTBUF   poll command (0x0040xxyy)
//   0x04 / 0x08   SICxINBUFH/L poll response (8 bytes)
//   0x30 SIPOLL, 0x34 SICOMCSR, 0x38 SISR, 0x80..0xFF COM buffer
// Port 1 holds a standard controller fed from the host (SDL gamepad or
// keyboard); ports 2-4 are empty.
#include "hw/si.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/runtime.h"

namespace {

uint32_t g_out[4], g_inh[4], g_inl[4];
uint32_t g_poll = 0, g_comcsr = 0, g_sr = 0, g_exilk = 0;
uint8_t g_buf[128];
SDL_Gamepad* g_pad = nullptr;
bool g_line = false;

struct PadState {
    uint16_t buttons;  // byte0 << 8 | byte1 of the poll response
    uint8_t sx, sy, cx, cy, l, r;
};

// byte0: 4 Start, 3 Y, 2 X, 1 B, 0 A; byte1: 7 always set, 6 L, 5 R, 4 Z, 3 Up, 2 Down, 1 Right, 0 Left
enum : uint16_t {
    BTN_A = 1 << 8, BTN_B = 2 << 8, BTN_X = 4 << 8, BTN_Y = 8 << 8, BTN_START = 16 << 8,
    BTN_LEFT = 1, BTN_RIGHT = 2, BTN_DOWN = 4, BTN_UP = 8, BTN_Z = 16, BTN_R = 32, BTN_L = 64, BTN_ORIGIN = 128,
};

uint8_t axis8(int v) {  // SDL -32768..32767 -> 0..255 around 0x80
    int x = 128 + v / 256;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

PadState read_host() {
    PadState s{BTN_ORIGIN, 0x80, 0x80, 0x80, 0x80, 0, 0};
    if (!g_pad) {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) {
            g_pad = SDL_OpenGamepad(ids[0]);
            if (g_pad) rt_log("input: using gamepad \"%s\"", SDL_GetGamepadName(g_pad));
        }
        SDL_free(ids);
    }
    if (g_pad && SDL_GamepadConnected(g_pad)) {
        auto b = [&](SDL_GamepadButton btn) { return SDL_GetGamepadButton(g_pad, btn); };
        // GameCube layout: big A = south, B = west, X = east, Y = north.
        if (b(SDL_GAMEPAD_BUTTON_SOUTH)) s.buttons |= BTN_A;
        if (b(SDL_GAMEPAD_BUTTON_WEST)) s.buttons |= BTN_B;
        if (b(SDL_GAMEPAD_BUTTON_EAST)) s.buttons |= BTN_X;
        if (b(SDL_GAMEPAD_BUTTON_NORTH)) s.buttons |= BTN_Y;
        if (b(SDL_GAMEPAD_BUTTON_START)) s.buttons |= BTN_START;
        if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || b(SDL_GAMEPAD_BUTTON_BACK)) s.buttons |= BTN_Z;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) s.buttons |= BTN_UP;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.buttons |= BTN_DOWN;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.buttons |= BTN_LEFT;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.buttons |= BTN_RIGHT;
        s.sx = axis8(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX));
        s.sy = axis8(-SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY) - 1);
        s.cx = axis8(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX));
        s.cy = axis8(-SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY) - 1);
        int lt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128;
        int rt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128;
        s.l = (uint8_t)(lt < 0 ? 0 : lt > 255 ? 255 : lt);
        s.r = (uint8_t)(rt < 0 ? 0 : rt > 255 ? 255 : rt);
        if (s.l > 0xE0) s.buttons |= BTN_L;
        if (s.r > 0xE0) s.buttons |= BTN_R;
    }
    // Keyboard: WASD move, IJKL look, Space A, Backspace B, E X, Q Y, Enter Start,
    // Tab Z, Shift L, Ctrl R, arrows D-pad.
    const bool* k = SDL_GetKeyboardState(nullptr);
    if (k) {
        auto key = [&](SDL_Scancode c) { return k[c]; };
        if (key(SDL_SCANCODE_SPACE)) s.buttons |= BTN_A;
        if (key(SDL_SCANCODE_BACKSPACE) || key(SDL_SCANCODE_ESCAPE)) s.buttons |= BTN_B;
        if (key(SDL_SCANCODE_E)) s.buttons |= BTN_X;
        if (key(SDL_SCANCODE_Q)) s.buttons |= BTN_Y;
        if (key(SDL_SCANCODE_RETURN)) s.buttons |= BTN_START;
        if (key(SDL_SCANCODE_TAB)) s.buttons |= BTN_Z;
        if (key(SDL_SCANCODE_UP)) s.buttons |= BTN_UP;
        if (key(SDL_SCANCODE_DOWN)) s.buttons |= BTN_DOWN;
        if (key(SDL_SCANCODE_LEFT)) s.buttons |= BTN_LEFT;
        if (key(SDL_SCANCODE_RIGHT)) s.buttons |= BTN_RIGHT;
        if (key(SDL_SCANCODE_LSHIFT)) { s.buttons |= BTN_L; s.l = 255; }
        if (key(SDL_SCANCODE_LCTRL)) { s.buttons |= BTN_R; s.r = 255; }
        if (key(SDL_SCANCODE_A)) s.sx = 0x20;
        if (key(SDL_SCANCODE_D)) s.sx = 0xE0;
        if (key(SDL_SCANCODE_W)) s.sy = 0xE0;
        if (key(SDL_SCANCODE_S)) s.sy = 0x20;
        if (key(SDL_SCANCODE_J)) s.cx = 0x20;
        if (key(SDL_SCANCODE_L)) s.cx = 0xE0;
        if (key(SDL_SCANCODE_I)) s.cy = 0xE0;
        if (key(SDL_SCANCODE_K)) s.cy = 0x20;
    }
    si_scripted_input(&s.buttons, &s.sx, &s.sy);
    return s;
}

uint32_t rdst_bit(int ch) { return 1u << (24 - 8 * ch + 5); }
uint32_t norep_bit(int ch) { return 1u << (24 - 8 * ch + 3); }

void update_line() {
    bool tc = (g_comcsr >> 31) & 1 && (g_comcsr >> 30) & 1;
    bool rd = (g_comcsr >> 28) & 1 && (g_comcsr >> 27) & 1;
    g_line = tc || rd;
}

// Answer a COM transfer: command in the buffer, reply written back in place.
void com_transfer() {
    int ch = (int)((g_comcsr >> 1) & 3);
    uint32_t in_len = (g_comcsr >> 8) & 0x7F;
    if (!in_len) in_len = 128;
    uint8_t cmd = g_buf[0];
    memset(g_buf, 0, in_len);
    if (ch != 0) {  // empty port
        g_sr |= norep_bit(ch);
        g_comcsr |= 1u << 29;  // COMERR
    } else {
        PadState p = read_host();
        switch (cmd) {
        case 0x00: case 0xFF:  // ID / reset: standard controller
            g_buf[0] = 0x09; g_buf[1] = 0x00; g_buf[2] = 0x03;
            break;
        case 0x41: case 0x42:  // origin / recalibrate
            g_buf[0] = 0; g_buf[1] = 0x80; g_buf[2] = 0x80; g_buf[3] = 0x80; g_buf[4] = 0x80; g_buf[5] = 0x80;
            g_buf[6] = 0x1F; g_buf[7] = 0x1F;
            break;
        case 0x40:  // direct status
            g_buf[0] = (uint8_t)(p.buttons >> 8); g_buf[1] = (uint8_t)p.buttons;
            g_buf[2] = p.sx; g_buf[3] = p.sy; g_buf[4] = p.cx; g_buf[5] = p.cy; g_buf[6] = p.l; g_buf[7] = p.r;
            break;
        default:
            break;
        }
        g_sr &= ~norep_bit(0);
    }
    g_comcsr &= ~1u;          // TSTART done
    g_comcsr |= 1u << 31;     // TCINT
    update_line();
}

}  // namespace

void si_poll_frame() {
    // Polling runs once per field for enabled channels (SIPOLL EN bits 4..7, ch0 = bit 7).
    for (int ch = 0; ch < 4; ch++) {
        if (!((g_poll >> (7 - ch)) & 1)) continue;
        if (ch != 0) {
            g_inh[ch] = 0x80000000u;  // ERRSTAT: nothing connected
            g_sr |= norep_bit(ch);
            continue;
        }
        PadState p = read_host();
        g_inh[0] = (uint32_t)p.buttons << 16 | (uint32_t)p.sx << 8 | p.sy;
        g_inl[0] = (uint32_t)p.cx << 24 | (uint32_t)p.cy << 16 | (uint32_t)p.l << 8 | p.r;
        g_sr |= rdst_bit(0);
        g_comcsr |= 1u << 28;  // RDSTINT
    }
    update_line();
}

bool si_interrupt_line() { return g_line; }

uint32_t si_read(uint32_t o) {
    uint32_t r = o - 0x6400;
    if (r < 0x30) {
        int ch = (int)(r / 12);
        switch (r % 12) {
        case 0: return g_out[ch];
        case 4: g_sr &= ~rdst_bit(ch); if (!(g_sr & 0x20202020)) g_comcsr &= ~(1u << 28); update_line(); return g_inh[ch];
        default: return g_inl[ch];
        }
    }
    switch (r) {
    case 0x30: return g_poll;
    case 0x34: return g_comcsr;
    case 0x38: return g_sr;
    case 0x3C: return g_exilk;
    default:
        if (r >= 0x80 && r < 0x100) {
            uint32_t i = r - 0x80;
            return (uint32_t)g_buf[i] << 24 | g_buf[i + 1] << 16 | g_buf[i + 2] << 8 | g_buf[i + 3];
        }
        return 0;
    }
}

void si_write(uint32_t o, uint32_t v) {
    uint32_t r = o - 0x6400;
    if (r < 0x30) {
        if (r % 12 == 0) g_out[r / 12] = v;
        return;
    }
    switch (r) {
    case 0x30: g_poll = v; break;
    case 0x34:
        // TCINT (31) and RDSTINT (28) are write-1-to-clear; the rest is control.
        g_comcsr = (v & ~((1u << 31) | (1u << 28) | (1u << 29))) | (g_comcsr & ((1u << 31) | (1u << 28)) & ~(v & ((1u << 31) | (1u << 28))));
        if (v & 1) com_transfer();
        update_line();
        break;
    case 0x38:
        g_sr &= ~(v & 0x0F0F0F0F);  // error bits write-1-to-clear
        break;
    case 0x3C: g_exilk = v; break;
    default:
        if (r >= 0x80 && r < 0x100) {
            uint32_t i = r - 0x80;
            g_buf[i] = (uint8_t)(v >> 24); g_buf[i + 1] = (uint8_t)(v >> 16); g_buf[i + 2] = (uint8_t)(v >> 8); g_buf[i + 3] = (uint8_t)v;
        }
        break;
    }
}

// ---------------------------------------------------------------- scripted input
// TSFP_INPUT="frame:buttons,frame:buttons,..." presses buttons (A,B,X,Y,S,Z,U,D,L,R)
// at given VI frames for ~6 frames each: lets automated runs get past menus.

namespace {
struct Press {
    uint32_t frame;
    uint16_t buttons;
};
std::vector<Press> g_script;
bool g_script_loaded = false;
}  // namespace

uint32_t si_frame_counter = 0;

void si_scripted_input(uint16_t* buttons, uint8_t* sx, uint8_t* sy) {
    (void)sx; (void)sy;
    if (!g_script_loaded) {
        g_script_loaded = true;
        if (const char* env = getenv("TSFP_INPUT")) {
            std::string s = env;
            size_t pos = 0;
            while (pos < s.size()) {
                size_t comma = s.find(',', pos);
                std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    Press p{(uint32_t)atoi(item.c_str()), 0};
                    for (char c : item.substr(colon + 1)) {
                        switch (c) {
                        case 'A': p.buttons |= BTN_A; break;
                        case 'B': p.buttons |= BTN_B; break;
                        case 'X': p.buttons |= BTN_X; break;
                        case 'Y': p.buttons |= BTN_Y; break;
                        case 'S': p.buttons |= BTN_START; break;
                        case 'Z': p.buttons |= BTN_Z; break;
                        case 'U': p.buttons |= BTN_UP; break;
                        case 'D': p.buttons |= BTN_DOWN; break;
                        case 'L': p.buttons |= BTN_LEFT; break;
                        case 'R': p.buttons |= BTN_RIGHT; break;
                        }
                    }
                    g_script.push_back(p);
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
    }
    for (const Press& p : g_script)
        if (si_frame_counter >= p.frame && si_frame_counter < p.frame + 6) *buttons |= p.buttons;
}

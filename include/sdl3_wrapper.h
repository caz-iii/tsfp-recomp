#ifndef TSFP_SDL3_WRAPPER_H
#define TSFP_SDL3_WRAPPER_H

// SDL3 utility declarations for tsfp-recomp
// Wraps SDL3 calls with error handling and logging

// Initialization
int sdl3_init_all(void);
void sdl3_shutdown(void);

// Window management
struct SDL_Window;
SDL_Window* sdl3_create_window(const char* title, int w, int h);
void sdl3_destroy_window(SDL_Window* window);

// Audio
int sdl3_audio_init(int sample_rate, int channels);
void sdl3_audio_shutdown(void);

// Input
void sdl3_poll_input(void);

#endif // TSFP_SDL3_WRAPPER_H

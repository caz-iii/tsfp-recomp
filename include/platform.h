#ifndef TSFP_PLATFORM_H
#define TSFP_PLATFORM_H

// Platform abstraction layer for tsfp-recomp
// Maps GameCube hardware operations to PC equivalents

#ifdef _WIN32
#define TSFP_PLATFORM_WINDOWS
#elif defined(__linux__)
#define TSFP_PLATFORM_LINUX
#endif

// Platform initialization
int platform_init(void);
void platform_shutdown(void);

// Display
int platform_create_window(int width, int height, const char* title);
void platform_swap_buffers(void);
void platform_poll_events(void);

// Timing
unsigned long long platform_get_ticks_ms(void);
void platform_sleep_ms(unsigned int ms);

#endif // TSFP_PLATFORM_H

/*
 * Tyrian 32X - minimal SDL2 replacement.
 *
 * Only the subset of SDL that OpenTyrian's game code actually uses.
 * The display, timing and pad parts are implemented on top of plat.h
 * (plat_mars.c on the 32X, plat_host.c on a PC for testing).
 * Window, renderer and audio-device calls are inert stubs.
 */
#ifndef T32X_SDL_H
#define T32X_SDL_H

#include "SDL_types.h"
#include "SDL_endian.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>  /* real SDL.h provides these; animlib.c, editship.c, logging.c rely on it */
#include <stddef.h>
#include <string.h>

#define SDL_VERSION_ATLEAST(x, y, z) 0
#define SDL_PRINTF_FORMAT_STRING
#define SDL_PRINTF_VARARG_FUNC(n) __attribute__((format(printf, n, n + 1)))

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

/* ---------------------------------------------------------------- init */
#define SDL_INIT_VIDEO    0x0020u
#define SDL_INIT_AUDIO    0x0010u
#define SDL_INIT_JOYSTICK 0x0200u

int  SDL_Init(Uint32 flags);
int  SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
char *SDL_GetBasePath(void);
void SDL_free(void *p);
size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen);
SDL_bool SDL_SetHint(const char *name, const char *value);
#define SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE "SDL_MOUSE_RELATIVE_SYSTEM_SCALE"

/* --------------------------------------------------------------- timing */
Uint32 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);

/* ------------------------------------------------------------- graphics */
typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;

#define SDL_PIXELFORMAT_RGB888 0x16161804u
#define SDL_PIXELFORMAT_RGB565 0x15151002u
#define SDL_PIXELFORMAT_MARS555 0x32580555u  /* 32X CRAM: 0BBBBBGGGGGRRRRR */

typedef struct SDL_PixelFormat
{
	Uint32 format;
	void *palette;
	Uint8 BitsPerPixel;
	Uint8 BytesPerPixel;
} SDL_PixelFormat;

typedef struct SDL_Surface
{
	Uint32 flags;
	SDL_PixelFormat *format;
	int w, h;
	int pitch;
	void *pixels;
} SDL_Surface;

#define SDL_MUSTLOCK(s) 0

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am);
void SDL_FreeSurface(SDL_Surface *s);
int  SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int  SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b);
SDL_PixelFormat *SDL_AllocFormat(Uint32 pixel_format);
void SDL_FreeFormat(SDL_PixelFormat *fmt);
const char *SDL_GetPixelFormatName(Uint32 format);

/* There is no window; these exist so leftover references compile. */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

#define SDL_DISABLE 0
#define SDL_ENABLE  1
#define SDL_IGNORE  0
int SDL_GetNumVideoDisplays(void);
int SDL_ShowCursor(int toggle);
int SDL_SetRelativeMouseMode(SDL_bool enabled);

#define SDL_MESSAGEBOX_ERROR 0x10
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);

/* --------------------------------------------------------------- input */
typedef enum
{
	SDL_SCANCODE_UNKNOWN = 0,
	SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
	SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
	SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
	SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
	SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
	SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
	SDL_SCANCODE_Y, SDL_SCANCODE_Z,
	SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
	SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
	SDL_SCANCODE_9, SDL_SCANCODE_0,
	SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE,
	SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE, SDL_SCANCODE_MINUS,
	SDL_SCANCODE_EQUALS, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
	SDL_SCANCODE_BACKSLASH,
	SDL_SCANCODE_SEMICOLON = 51, SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE,
	SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH,
	SDL_SCANCODE_CAPSLOCK,
	SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
	SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
	SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
	SDL_SCANCODE_PRINTSCREEN = 70, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_PAUSE,
	SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP,
	SDL_SCANCODE_DELETE, SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN,
	SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
	SDL_SCANCODE_NUMLOCKCLEAR,
	SDL_SCANCODE_KP_ENTER = 88, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2,
	SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6,
	SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_0,
	SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT,
	SDL_SCANCODE_LGUI, SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT,
	SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI,
	SDL_NUM_SCANCODES = 256  /* real SDL uses 512; 256 covers every code above */
} SDL_Scancode;

typedef Sint32 SDL_Keycode;
#define SDLK_d 'd'
#define SDLK_g 'g'
#define SDLK_l 'l'
#define SDLK_o 'o'
#define SDLK_r 'r'
#define SDLK_s 's'
#define SDLK_RIGHTBRACKET ']'

typedef enum
{
	KMOD_NONE = 0x0000,
	KMOD_LSHIFT = 0x0001, KMOD_RSHIFT = 0x0002,
	KMOD_LCTRL = 0x0040,  KMOD_RCTRL = 0x0080,
	KMOD_LALT = 0x0100,   KMOD_RALT = 0x0200,
	KMOD_LGUI = 0x0400,   KMOD_RGUI = 0x0800,
	KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL,
	KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
	KMOD_ALT = KMOD_LALT | KMOD_RALT,
	KMOD_GUI = KMOD_LGUI | KMOD_RGUI,
} SDL_Keymod;

const char *SDL_GetScancodeName(SDL_Scancode scancode);
SDL_Scancode SDL_GetScancodeFromName(const char *name);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);

#define SDL_BUTTON(x) (1u << ((x) - 1))
#define SDL_BUTTON_LEFT  1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_LMASK SDL_BUTTON(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK SDL_BUTTON(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK SDL_BUTTON(SDL_BUTTON_RIGHT)

#define SDL_RELEASED 0
#define SDL_PRESSED  1

typedef enum
{
	SDL_QUIT = 0x100,
	SDL_WINDOWEVENT = 0x200,
	SDL_KEYDOWN = 0x300, SDL_KEYUP, SDL_TEXTEDITING, SDL_TEXTINPUT,
	SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP,
} SDL_EventType;

enum
{
	SDL_WINDOWEVENT_RESIZED = 5,
	SDL_WINDOWEVENT_FOCUS_GAINED = 12,
	SDL_WINDOWEVENT_FOCUS_LOST = 13,
};

typedef struct SDL_Keysym
{
	SDL_Scancode scancode;
	SDL_Keycode sym;
	Uint16 mod;
} SDL_Keysym;

typedef union SDL_Event
{
	Uint32 type;
	struct { Uint32 type; Uint8 event; } window;
	struct { Uint32 type; Uint8 state; Uint8 repeat; SDL_Keysym keysym; } key;
	struct { Uint32 type; char text[32]; } text;
	struct { Uint32 type; Sint32 x, y, xrel, yrel; } motion;
	struct { Uint32 type; Uint8 button; Sint32 x, y; } button;
} SDL_Event;

int SDL_PollEvent(SDL_Event *event);
int SDL_PushEvent(SDL_Event *event);

/* ------------------------------------------------------------ joystick */
/* The Genesis pad shows up as joystick 0: one hat (D-pad), six buttons. */
typedef struct SDL_Joystick SDL_Joystick;

#define SDL_HAT_CENTERED 0x00
#define SDL_HAT_UP       0x01
#define SDL_HAT_RIGHT    0x02
#define SDL_HAT_DOWN     0x04
#define SDL_HAT_LEFT     0x08

int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int index);
void SDL_JoystickClose(SDL_Joystick *j);
const char *SDL_JoystickName(SDL_Joystick *j);
int SDL_JoystickNumAxes(SDL_Joystick *j);
int SDL_JoystickNumButtons(SDL_Joystick *j);
int SDL_JoystickNumHats(SDL_Joystick *j);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis);
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int button);
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int hat);
void SDL_JoystickUpdate(void);
int SDL_JoystickEventState(int state);

/* --------------------------------------------------------------- audio */
/* Audio goes through CD tracks + PWM later (T4); the SDL audio device is never opened. */
typedef Uint32 SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;
#define AUDIO_S16SYS 0x8010
#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x01
#define SDL_AUDIO_ALLOW_SAMPLES_CHANGE   0x08

typedef struct SDL_AudioSpec
{
	int freq;
	SDL_AudioFormat format;
	Uint8 channels;
	Uint16 samples;
	void (*callback)(void *userdata, Uint8 *stream, int len);
	void *userdata;
} SDL_AudioSpec;

typedef struct SDL_AudioCVT
{
	int needed;
	Uint8 *buf;
	int len;
	int len_cvt;
	int len_mult;
	double len_ratio;
} SDL_AudioCVT;

int  SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat sf, Uint8 sc, int sr,
                       SDL_AudioFormat df, Uint8 dc, int dr);
int  SDL_ConvertAudio(SDL_AudioCVT *cvt);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);

/* ------------------------------------------------------------- logging */
#define SDL_LOG_CATEGORY_APPLICATION 0
typedef enum
{
	SDL_LOG_PRIORITY_VERBOSE = 1, SDL_LOG_PRIORITY_DEBUG, SDL_LOG_PRIORITY_INFO,
	SDL_LOG_PRIORITY_WARN, SDL_LOG_PRIORITY_ERROR, SDL_LOG_PRIORITY_CRITICAL,
} SDL_LogPriority;
typedef void (*SDL_LogOutputFunction)(void *userdata, int category, SDL_LogPriority priority, const char *message);

void SDL_Log(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
void SDL_LogDebug(int category, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
void SDL_LogWarn(int category, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
void SDL_LogError(int category, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
void SDL_LogSetPriority(int category, SDL_LogPriority priority);
void SDL_LogGetOutputFunction(SDL_LogOutputFunction *callback, void **userdata);

#endif /* T32X_SDL_H */

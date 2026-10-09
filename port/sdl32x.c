/*
 * Tyrian 32X - minimal SDL2 replacement (see port/include/SDL.h).
 */
#include "dirty32x.h"  // DIRTY
#include "SDL.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ init */

int SDL_PushEvent(SDL_Event *event);

int SDL_Init(Uint32 flags)
{
	(void)flags;
	plat_init();

	/* The console "window" always has focus. Without this event OpenTyrian's
	 * windowHasFocus stays false and every level starts paused. */
	SDL_Event focus = { .window = { SDL_WINDOWEVENT, SDL_WINDOWEVENT_FOCUS_GAINED } };
	SDL_PushEvent(&focus);
	return 0;
}
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
void SDL_Quit(void) { }
const char *SDL_GetError(void) { return "unsupported on 32X"; }
char *SDL_GetBasePath(void) { return NULL; }
void SDL_free(void *p) { free(p); }
SDL_bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return SDL_FALSE; }

size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
	size_t len = strlen(src);
	if (maxlen > 0)
	{
		size_t n = len < maxlen - 1 ? len : maxlen - 1;
		memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}

/* ============================================================== timing */

Uint32 SDL_GetTicks(void) { return plat_ticks_ms(); }

void SDL_Delay(Uint32 ms)
{
	Uint32 start = plat_ticks_ms();
	while (plat_ticks_ms() - start < ms)
		;
}

/* ============================================================ graphics */

static SDL_PixelFormat format8 = { 0, NULL, 8, 1 };

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am)
{
	(void)flags; (void)rm; (void)gm; (void)bm; (void)am;
	if (depth != 8)
		return NULL;

	SDL_Surface *s = malloc(sizeof *s);
	if (s == NULL)
		return NULL;
	s->flags = 0;
	s->format = &format8;
	s->w = w;
	s->h = h;
	s->pitch = w;
	s->pixels = malloc((size_t)w * h);
	if (s->pixels == NULL)
	{
		free(s);
		return NULL;
	}
	memset(s->pixels, 0, (size_t)w * h);
	return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
	if (s == NULL)
		return;
	free(s->pixels);
	free(s);
}

/* Clip r against the surface; returns false if nothing is left. */
static bool clip_rect(const SDL_Surface *s, SDL_Rect *r)
{
	int x0 = r->x < 0 ? 0 : r->x;
	int y0 = r->y < 0 ? 0 : r->y;
	int x1 = r->x + r->w > s->w ? s->w : r->x + r->w;
	int y1 = r->y + r->h > s->h ? s->h : r->y + r->h;
	if (x1 <= x0 || y1 <= y0)
		return false;
	r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
	return true;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
	SDL_Rect r = rect ? *rect : (SDL_Rect){ 0, 0, dst->w, dst->h };
	if (!clip_rect(dst, &r))
		return 0;
	T32X_DIRTY_RECT(dst, r.x, r.y, r.w, r.h);  // DIRTY

	Uint8 *p = (Uint8 *)dst->pixels + r.y * dst->pitch + r.x;
	for (int y = 0; y < r.h; ++y, p += dst->pitch)
		memset(p, (int)(color & 0xFF), (size_t)r.w);
	return 0;
}

int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
	SDL_Rect s = srcrect ? *srcrect : (SDL_Rect){ 0, 0, src->w, src->h };
	if (!clip_rect(src, &s))
		return 0;

	int dx = dstrect ? dstrect->x : 0;
	int dy = dstrect ? dstrect->y : 0;
	/* Clip the destination, shifting the source by the same amount. */
	SDL_Rect d = { dx, dy, s.w, s.h };
	if (!clip_rect(dst, &d))
		return 0;
	s.x += d.x - dx;
	s.y += d.y - dy;
	T32X_DIRTY_RECT(dst, d.x, d.y, d.w, d.h);  // DIRTY

	const Uint8 *sp = (const Uint8 *)src->pixels + s.y * src->pitch + s.x;
	Uint8 *dp = (Uint8 *)dst->pixels + d.y * dst->pitch + d.x;
	for (int y = 0; y < d.h; ++y, sp += src->pitch, dp += dst->pitch)
		memmove(dp, sp, (size_t)d.w);

	if (dstrect)
		*dstrect = d;
	return 0;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b)
{
	if (fmt != NULL && fmt->format == SDL_PIXELFORMAT_MARS555)
		return ((Uint32)(b >> 3) << 10) | ((Uint32)(g >> 3) << 5) | (Uint32)(r >> 3);
	return ((Uint32)r << 16) | ((Uint32)g << 8) | b;
}

SDL_PixelFormat *SDL_AllocFormat(Uint32 pixel_format)
{
	SDL_PixelFormat *f = calloc(1, sizeof *f);
	if (f != NULL)
	{
		f->format = pixel_format;
		f->BitsPerPixel = 16;
		f->BytesPerPixel = 2;
	}
	return f;
}

void SDL_FreeFormat(SDL_PixelFormat *fmt) { free(fmt); }
const char *SDL_GetPixelFormatName(Uint32 format) { (void)format; return "MARS555"; }

int SDL_GetNumVideoDisplays(void) { return 0; }  /* the setup menu then offers only "window" */
int SDL_ShowCursor(int toggle) { (void)toggle; return SDL_DISABLE; }
int SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; return -1; }

/* logFatal() ends here; every caller exits right after, so stop now and show the text. */
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
	(void)flags; (void)title; (void)window;
	plat_fatal(message);
}

/* =============================================================== events */

static SDL_Event event_queue[32];
static unsigned int event_head, event_count;

int SDL_PushEvent(SDL_Event *event)
{
	if (event_count == sizeof event_queue / sizeof *event_queue)
		return 0;
	unsigned int tail = (event_head + event_count) % (sizeof event_queue / sizeof *event_queue);
	event_queue[tail] = *event;
	++event_count;
	return 1;
}

/* Sega Mouse -> SDL mouse events (plat_read_mouse, which itself asks the
 * 68000 at most every 4 ms, like the pad). The cursor position is kept here, in screen coordinates
 * (mapWindowPointToScreen does nothing on the 32X); the mouse counts Y
 * upwards, the screen downwards. Left/right/middle are SDL's buttons; the
 * mouse's Start button acts as the pad's Start (SDL_JoystickUpdate). */
static int mouse_x = 160, mouse_y = 100;
static uint8_t mouse_buttons;

static void push_button(Uint8 button, bool down)
{
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	e.button.button = button;
	e.button.x = mouse_x;
	e.button.y = mouse_y;
	SDL_PushEvent(&e);
}

static void mouse_poll(void)
{
	int dx, dy;
	uint8_t b;
	if (!plat_read_mouse(&dx, &dy, &b))
	{
		mouse_buttons = 0;
		return;
	}
	dy = -dy;
	if (dx != 0 || dy != 0)
	{
		mouse_x += dx;
		mouse_y += dy;
		if (mouse_x < 0) mouse_x = 0;
		if (mouse_x > 319) mouse_x = 319;
		if (mouse_y < 0) mouse_y = 0;
		if (mouse_y > 199) mouse_y = 199;
		SDL_Event e;
		memset(&e, 0, sizeof e);
		e.type = SDL_MOUSEMOTION;
		e.motion.x = mouse_x;
		e.motion.y = mouse_y;
		e.motion.xrel = dx;
		e.motion.yrel = dy;
		SDL_PushEvent(&e);
	}
	const uint8_t changed = (uint8_t)(b ^ mouse_buttons);
	if (changed & MOUSE_LEFT)   push_button(SDL_BUTTON_LEFT, (b & MOUSE_LEFT) != 0);
	if (changed & MOUSE_RIGHT)  push_button(SDL_BUTTON_RIGHT, (b & MOUSE_RIGHT) != 0);
	if (changed & MOUSE_MIDDLE) push_button(SDL_BUTTON_MIDDLE, (b & MOUSE_MIDDLE) != 0);
	mouse_buttons = b;
}

int SDL_PollEvent(SDL_Event *event)
{
	if (event_count == 0)
		mouse_poll();
	if (event_count == 0)
		return 0;
	if (event != NULL)
		*event = event_queue[event_head];
	event_head = (event_head + 1) % (sizeof event_queue / sizeof *event_queue);
	--event_count;
	return 1;
}

void SDL_StartTextInput(void) { }
void SDL_StopTextInput(void) { }

/* Names are only shown in the keyboard setup menu, which needs a keyboard anyway. */
const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
	switch (scancode)
	{
	case SDL_SCANCODE_UP:     return "Up";
	case SDL_SCANCODE_DOWN:   return "Down";
	case SDL_SCANCODE_LEFT:   return "Left";
	case SDL_SCANCODE_RIGHT:  return "Right";
	case SDL_SCANCODE_SPACE:  return "Space";
	case SDL_SCANCODE_RETURN: return "Return";
	case SDL_SCANCODE_ESCAPE: return "Escape";
	case SDL_SCANCODE_LCTRL:  return "Left Ctrl";
	case SDL_SCANCODE_LALT:   return "Left Alt";
	case SDL_SCANCODE_RCTRL:  return "Right Ctrl";
	case SDL_SCANCODE_RALT:   return "Right Alt";
	default:                  return "";
	}
}

SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
	for (int sc = 1; sc < SDL_NUM_SCANCODES; ++sc)
	{
		const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
		if (n[0] != '\0' && strcmp(n, name) == 0)
			return (SDL_Scancode)sc;
	}
	return SDL_SCANCODE_UNKNOWN;
}

/* ============================================================= joystick */
/*
 * Joystick 0 = the Genesis pad. OpenTyrian's default assignment
 * (joystick.c reset_joystick_assignments) maps hat 0 to the directions and
 * buttons 0..5 to: fire, change fire, left sidekick, right sidekick, menu, pause.
 * OpenTyrian treats fire/menu as "confirm" and change fire/pause as "cancel" in menus.
 */
static const Uint16 pad_button_masks[6] =
{
	PAD_A,             /* 0 fire                         (menus: confirm) */
	PAD_C,             /* 1 change rear weapon mode      (menus: cancel)  */
	PAD_B | PAD_X,     /* 2 left sidekick                                 */
	PAD_B | PAD_Z,     /* 3 right sidekick                                */
	PAD_START,         /* 4 in-game menu                 (menus: confirm) */
	PAD_MODE | PAD_Y,  /* 5 pause                        (menus: cancel)  */
};

static int pad_joystick_dummy;
static Uint16 pad_state;

int SDL_NumJoysticks(void) { return 1; }
SDL_Joystick *SDL_JoystickOpen(int index) { return index == 0 ? (SDL_Joystick *)&pad_joystick_dummy : NULL; }
void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
const char *SDL_JoystickName(SDL_Joystick *j) { (void)j; return "Genesis pad"; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 6; }
int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 1; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis) { (void)j; (void)axis; return 0; }
int SDL_JoystickEventState(int state) { (void)state; return SDL_IGNORE; }

#ifdef T32X_FPS
/* FPS=1: the Z button shows or hides the frames-per-second counter
 * (port/plat_mars.c fps_draw); in these builds Z is not the right
 * sidekick's second button (B still is). */
int t32x_fps_on = 1;
#endif

#ifndef T32X_RELEASE
/* the level select (src/tyrian2.c): a new press of Y, seen once */
static bool y_pressed;
bool t32x_take_y_press(void)
{
	const bool y = y_pressed;
	y_pressed = false;
	return y;
}
#endif

#ifdef T32X_PROFILE
/* PROFILE=1: the X button shows or hides the profiler panel; in these builds
 * X is not the left sidekick's second button (B still is) */
int t32x_prof_on = 1;
#endif

void SDL_JoystickUpdate(void)
{
	pad_state = plat_read_pad();
#ifndef T32X_RELEASE
	{
		static Uint16 prev_y;
		if ((pad_state & ~prev_y) & PAD_Y)
			y_pressed = true;
		prev_y = pad_state;
	}
#endif
#ifdef T32X_PROFILE
	{
		static Uint16 prev_x;
		if ((pad_state & ~prev_x) & PAD_X)
			t32x_prof_on ^= 1;
		prev_x = pad_state;
		pad_state &= (Uint16)~PAD_X;
	}
#endif
#ifdef T32X_FPS
	{
		static Uint16 prev;
		if ((pad_state & ~prev) & PAD_Z)
			t32x_fps_on ^= 1;
		prev = pad_state;
		pad_state &= (Uint16)~PAD_Z;
	}
#endif
	if (mouse_buttons & MOUSE_START)
		pad_state |= PAD_START;  /* the mouse's Start button = the pad's */
}

Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int button)
{
	(void)j;
	if (button < 0 || button >= 6)
		return 0;
	return (pad_state & pad_button_masks[button]) != 0;
}

Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int hat)
{
	(void)j;
	if (hat != 0)
		return SDL_HAT_CENTERED;
	Uint8 h = SDL_HAT_CENTERED;
	if (pad_state & PAD_UP)    h |= SDL_HAT_UP;
	if (pad_state & PAD_DOWN)  h |= SDL_HAT_DOWN;
	if (pad_state & PAD_LEFT)  h |= SDL_HAT_LEFT;
	if (pad_state & PAD_RIGHT) h |= SDL_HAT_RIGHT;
	return h;
}

/* ================================================================ audio */

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat sf, Uint8 sc, int sr,
                      SDL_AudioFormat df, Uint8 dc, int dr)
{
	(void)sf; (void)sc; (void)sr; (void)df; (void)dc; (void)dr;
	memset(cvt, 0, sizeof *cvt);
	cvt->len_mult = 1;
	cvt->len_ratio = 1.0;
	return 0;
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt) { cvt->len_cvt = cvt->len; return 0; }
void SDL_LockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) { (void)dev; (void)pause_on; }

/* ============================================================== logging */

static void log_output(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
	(void)userdata; (void)category; (void)priority;
	plat_log(message);
}

static void log_v(const char *fmt, va_list ap)
{
	char buf[160];
	vsnprintf(buf, sizeof buf, fmt, ap);
	plat_log(buf);
}

void SDL_Log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_v(fmt, ap); va_end(ap); }
void SDL_LogDebug(int c, const char *fmt, ...) { (void)c; va_list ap; va_start(ap, fmt); log_v(fmt, ap); va_end(ap); }
void SDL_LogWarn(int c, const char *fmt, ...) { (void)c; va_list ap; va_start(ap, fmt); log_v(fmt, ap); va_end(ap); }
void SDL_LogError(int c, const char *fmt, ...) { (void)c; va_list ap; va_start(ap, fmt); log_v(fmt, ap); va_end(ap); }
void SDL_LogSetPriority(int category, SDL_LogPriority priority) { (void)category; (void)priority; }

void SDL_LogGetOutputFunction(SDL_LogOutputFunction *callback, void **userdata)
{
	*callback = log_output;
	*userdata = NULL;
}

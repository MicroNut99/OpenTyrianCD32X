/*
 * Tyrian 32X - SPRITEQ (branch two-sh2, SPRITEQ=1 with TWO_SH2=1): the slave
 * SH2 draws the sprites while the master goes on with the game's logic.
 *
 * Tyrian moves and draws each enemy, shot and explosion in one loop: logic,
 * draw, logic, draw. With many small sprites the master spends that whole
 * stretch alone. Here the sprite2 drawing calls (blit_sprite2 and its clip,
 * blend, darken and filter variants - enemies, shots, explosions, the
 * player's ship) only write a small command into a queue in cache-through
 * SDRAM; the first one starts a job on the slave, which draws the commands in
 * order while the master computes the next enemies.
 *
 * Order is kept: anything else that draws into the playfield (layers, text,
 * rectangles, stars, filters - every place that reports to DIRTY) or reads it
 * (the frame's end, menus) first waits until the queue is drawn
 * (t32x_drawq_barrier). DIRTY's marks are made by the master when it queues.
 * While the queue is empty the slave keeps the sound mixed
 * (plat_slave_service), so a long logic stretch cannot starve the sound.
 *
 * PC build: no second CPU - the queue is drawn at the barrier, by the same
 * code, so the frame comparison checks the order and the marks.
 */
#include "hot.h"  /* T32X_HOT (HOT=1) */
#include "drawq32x.h"

#if defined(T32X_SPRITEQ) && defined(T32X_TWO_SH2)

#include "plat.h"
#include "prof.h"   /* PROFILE: the SLAVEW line */
#include "sprite.h"
#include "video.h"

#include <stdint.h>
#include <string.h>

#define DQ_N 64                        /* commands (a power of two); full = the master waits a moment */

typedef struct
{
	const Uint8 *data;                 /* the Sprite2_array's data */
	Sint16 x, y;
	Uint16 index;
	Uint8 kind, filter;
} DqItem;

typedef struct
{
	volatile uint32_t write, read;     /* master writes `write`, slave `read` */
	volatile uint32_t closed;          /* master: no more commands - finish and stop */
	DqItem item[DQ_N];
} DrawQ;

static DrawQ drawq_mem __attribute__((aligned(16)));
#ifdef __sh__
#define DQ ((volatile DrawQ *)((uintptr_t)&drawq_mem | 0x20000000u))  /* cache-through: both CPUs */
#else
#define DQ ((volatile DrawQ *)&drawq_mem)
#endif

void *t32x_drawq_target;
#ifdef __sh__
static bool worker_running;            /* master: the slave's job is on */
#endif
static SDL_Surface worker_surface;     /* game_screen, flagged: the worker draws through it */

T32X_HOT  // in RAM with HOT=1 (port/hot.h)
static void draw_item(const volatile DqItem *it, SDL_Surface *s)
{
	const Sprite2_array a = { 0, (Uint8 *)it->data };
	const int x = it->x, y = it->y;
	const unsigned int index = it->index;
	switch (it->kind)
	{
	case DQ_SPRITE2:             blit_sprite2(s, x, y, a, index); break;
	case DQ_SPRITE2_CLIP:        blit_sprite2_clip(s, x, y, a, index); break;
	case DQ_SPRITE2_BLEND:       blit_sprite2_blend(s, x, y, a, index); break;
	case DQ_SPRITE2_DARKEN:      blit_sprite2_darken(s, x, y, a, index); break;
	case DQ_SPRITE2_FILTER:      blit_sprite2_filter(s, x, y, a, index, it->filter); break;
	case DQ_SPRITE2_FILTER_CLIP: blit_sprite2_filter_clip(s, x, y, a, index, it->filter); break;
	}
}

#ifdef __sh__
/* the slave's job: draw commands as they come, until the master closes */
T32X_HOT  // in RAM with HOT=1 (port/hot.h)
static void worker(void *arg)
{
	SDL_Surface *s = arg;
	volatile DrawQ *q = DQ;
	for (;;)
	{
		const uint32_t r = q->read;
		if (r != q->write)
		{
			draw_item(&q->item[r & (DQ_N - 1)], s);
			q->read = r + 1;
		}
		else if (q->closed)
		{
			if (q->read == q->write)
				break;
		}
		else
			plat_slave_service();      /* nothing to draw: keep the sound mixed */
	}
}
#endif

bool t32x_drawq_push(int kind, const void *surface_pixels, Uint32 flags, int x, int y,
                     const Uint8 *data, unsigned int index, Uint8 filter)
{
	if (surface_pixels != t32x_drawq_target || t32x_drawq_target == NULL || flags == T32X_DRAWQ_WORKER_FLAGS)
		return false;
	volatile DrawQ *q = DQ;
#ifdef __sh__
	if (!worker_running)
	{
		q->write = q->read = 0;
		q->closed = 0;
		if (!plat_slave_job(worker, &worker_surface))
			return false;              /* no helper: draw it now */
		worker_running = true;
	}
	if (q->write - q->read >= DQ_N)
	{
		const uint32_t since = T32X_PROF_NOW();   /* PROFILE: the SLAVEW line */
		while (q->write - q->read >= DQ_N) { }    /* full: the slave catches up */
		T32X_PROF_SLAVE(since);
	}
#else
	if (q->write - q->read >= DQ_N)
		t32x_drawq_barrier();          /* PC: draw what is queued */
#endif
	volatile DqItem *it = &q->item[q->write & (DQ_N - 1)];
	it->data = data;
	it->x = (Sint16)x;
	it->y = (Sint16)y;
	it->index = (Uint16)index;
	it->kind = (Uint8)kind;
	it->filter = filter;
	q->write = q->write + 1;           /* last: the command is complete in SDRAM */
	return true;
}

void t32x_drawq_barrier(void)
{
	volatile DrawQ *q = DQ;
#ifdef __sh__
	if (!worker_running)
		return;
	q->closed = 1;
	plat_slave_wait();
	worker_running = false;
	q->write = q->read = 0;
#else
	while (q->read != q->write)
	{
		draw_item(&q->item[q->read & (DQ_N - 1)], &worker_surface);
		q->read = q->read + 1;
	}
	q->write = q->read = 0;
#endif
}

void t32x_drawq_frame_begin(void)
{
	t32x_drawq_barrier();
	worker_surface = *game_screen;
	worker_surface.flags = T32X_DRAWQ_WORKER_FLAGS;
	t32x_drawq_target = game_screen->pixels;
}

void t32x_drawq_frame_end(void)
{
	t32x_drawq_barrier();
	t32x_drawq_target = NULL;
}

#endif

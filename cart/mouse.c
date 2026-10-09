/*
 * Tyrian 32X - the Sega Mouse (Mega Mouse) in controller port 2, 68000 side.
 *
 * Read once per V-blank (cart_md.s vblank_handler calls tyr_mouse_poll); the
 * movement adds up until the SH2 asks (command 63, tyr_mouse_get - called with
 * interrupts off, so no reading is lost or counted twice).
 *
 * Protocol (Plutiedev "Mouse"; SpritesMind "Mega Mouse Usage"):
 *   TH and TR are outputs (control register 0x60), TL is the mouse's
 *   "ready" line. Write 0x60 (idle): bits 3-0 read 0000 when a mouse is
 *   there. Write 0x20 (TH low) to start a packet, then for every nibble
 *   toggle TR (0x00, 0x20, 0x00, ...), wait until TL has the same value,
 *   read bits 3-0. 9 nibbles: 0xB (mouse ID), 0xF, 0xF, flags (Y overflow,
 *   X overflow, Y sign, X sign), buttons (Start, Middle, Right, Left -
 *   1 = pressed), X high, X low, Y high, Y low. Then 0x60 again.
 *   Motion is 9-bit: the sign flag extends the 8 bits (but 0 stays 0, else
 *   it would read as -256); Y counts upwards. Every wait has a limit, so a
 *   missing or unplugged mouse cannot hang the V-blank.
 */
#include <stdint.h>

#define IO_DATA2   (*(volatile uint8_t *)0xA10005)
#define IO_CTRL2   (*(volatile uint8_t *)0xA1000B)
#define COMMW(n)   (*(volatile uint16_t *)(0xA15120 + 2 * (n)))  /* n = 0..7: COMM0..COMM14 */

extern volatile int8_t net_type; /* cart/net_link.s: port 2 carries the network then */

static int16_t acc_x, acc_y;     /* movement since the SH2 last asked */
static uint8_t buttons;          /* bit 0 left, 1 right, 2 middle, 3 start */
static uint8_t present;

static int wait_tl(uint8_t level)
{
	for (int n = 0; n < 400; n++)
		if (((IO_DATA2 >> 4) & 1) == level)
			return 1;
	return 0;
}

static void lost(void)
{
	IO_DATA2 = 0x60;
	present = 0;
	buttons = 0;
}

void tyr_mouse_poll(void)
{
	uint8_t nib[9];

	if (net_type != 0)               /* network game: port 2 is the link, no mouse */
	{
		present = 0;
		buttons = 0;
		return;
	}

	IO_CTRL2 = 0x60;                 /* TH, TR outputs */
	IO_DATA2 = 0x60;                 /* idle */
	for (volatile int d = 0; d < 8; d++) { }
	if ((IO_DATA2 & 0x0F) != 0)      /* no mouse here */
	{
		lost();
		return;
	}

	IO_DATA2 = 0x20;                 /* TH low: start a packet */
	uint8_t tr = 1;
	for (int i = 0; i < 9; i++)
	{
		tr ^= 1;
		IO_DATA2 = tr ? 0x20 : 0x00;
		if (!wait_tl(tr))
		{
			lost();
			return;
		}
		nib[i] = IO_DATA2 & 0x0F;
	}
	IO_DATA2 = 0x60;

	if (nib[0] != 0x0B)              /* not a mouse */
	{
		lost();
		return;
	}

	const uint8_t flags = nib[3];
	int16_t x = (int16_t)((nib[5] << 4) | nib[6]);
	int16_t y = (int16_t)((nib[7] << 4) | nib[8]);
	if ((flags & 1) && x != 0)
		x -= 256;                    /* X sign */
	if ((flags & 2) && y != 0)
		y -= 256;                    /* Y sign */
	if (flags & 4)
		x = 0;                       /* X overflow: drop this reading */
	if (flags & 8)
		y = 0;                       /* Y overflow */

	acc_x += x;
	acc_y += y;
	buttons = nib[4];
	present = 1;
}

/* command 63: COMM4 = X movement, COMM6 = Y movement (up = positive),
 * COMM8 = buttons (bits 0-3) | 0x8000 if a mouse is there */
void tyr_mouse_get(void)
{
	COMMW(2) = (uint16_t)acc_x;
	COMMW(3) = (uint16_t)acc_y;
	COMMW(4) = (uint16_t)(buttons | (present ? 0x8000 : 0));
	acc_x = 0;
	acc_y = 0;
}

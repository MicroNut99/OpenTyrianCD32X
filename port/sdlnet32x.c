/*
 * Tyrian 32X - SDL_net for OpenTyrian's network.c, over the console link.
 *
 * The link (plat_net_*, on the 32X D32XR's transport in cart/net_link.s) moves
 * bytes between the two consoles; bytes can be lost (a handshake timeout, or
 * both consoles sending at once). network.c already works with unreliable
 * datagrams (UDP: it acknowledges, resends and times out by itself), so here
 * each packet just becomes a frame:
 *   0xC0, [length high, length low, data..., CRC-16 high, CRC-16 low], 0xC0
 * with SLIP escaping inside (0xC0 -> 0xDB 0xDC, 0xDB -> 0xDB 0xDD). The
 * receiver drops any frame whose length or CRC is wrong - a lost packet, as
 * UDP would lose it. There is only one other console: addresses and ports are
 * accepted and ignored.
 */
#include "SDL_net.h"
#include "plat.h"

#include <stdlib.h>
#include <string.h>

static void frame_reset(void);

#define FRAME_END     0xC0
#define FRAME_ESC     0xDB
#define FRAME_ESC_END 0xDC
#define FRAME_ESC_ESC 0xDD
#define MAX_PAYLOAD   512

struct T32X_UDPsocket { int unused; };
static struct T32X_UDPsocket the_socket;

/* link cable (default) or serial: chosen in the network menu */
int t32x_net_type = NET_LINK;
static const char *last_error = "no error";

/* CRC-16/CCITT-FALSE */
static Uint16 crc16(const Uint8 *p, size_t n)
{
	Uint16 crc = 0xFFFF;
	while (n--)
	{
		crc ^= (Uint16)(*p++ << 8);
		for (int i = 0; i < 8; ++i)
			crc = (Uint16)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
	}
	return crc;
}

int SDLNet_Init(void) { return 0; }
void SDLNet_Quit(void) { }
const char *SDLNet_GetError(void) { return last_error; }

int SDLNet_ResolveHost(IPaddress *address, const char *host, Uint16 port)
{
	(void)host;
	address->host = 1;
	address->port = port;
	return 0;
}

/* opening the "socket" sets port 2 up for the link (or serial) */
UDPsocket SDLNet_UDP_Open(Uint16 port)
{
	(void)port;
	if (!plat_net_setup(t32x_net_type))
	{
		last_error = "the link could not be set up";
		return NULL;
	}
	frame_reset();
	return &the_socket;
}

void SDLNet_UDP_Close(UDPsocket sock)
{
	(void)sock;
	plat_net_cleanup();
}

int SDLNet_UDP_Bind(UDPsocket sock, int channel, const IPaddress *address)
{
	(void)sock; (void)address;
	return channel;
}

UDPpacket *SDLNet_AllocPacket(int size)
{
	UDPpacket *p = calloc(1, sizeof *p);
	if (p == NULL)
		return NULL;
	p->data = calloc(1, (size_t)size);
	if (p->data == NULL)
	{
		free(p);
		return NULL;
	}
	p->maxlen = size;
	return p;
}

void SDLNet_FreePacket(UDPpacket *packet)
{
	if (packet != NULL)
		free(packet->data);
	free(packet);
}

/* ---- sending: one frame ---- */
static Uint8 out[2 * (MAX_PAYLOAD + 4) + 2];
static size_t out_len;

static void put_escaped(Uint8 b)
{
	if (b == FRAME_END)
	{
		out[out_len++] = FRAME_ESC;
		out[out_len++] = FRAME_ESC_END;
	}
	else if (b == FRAME_ESC)
	{
		out[out_len++] = FRAME_ESC;
		out[out_len++] = FRAME_ESC_ESC;
	}
	else
		out[out_len++] = b;
}

int SDLNet_UDP_Send(UDPsocket sock, int channel, UDPpacket *packet)
{
	(void)sock; (void)channel;
	if (packet->len < 0 || packet->len > MAX_PAYLOAD)
	{
		last_error = "packet too large";
		return 0;
	}
	Uint8 head[2] = { (Uint8)(packet->len >> 8), (Uint8)packet->len };
	Uint16 crc = crc16(head, 2);
	crc ^= crc16(packet->data, (size_t)packet->len);  /* combined below, the same way on receipt */
	out_len = 0;
	out[out_len++] = FRAME_END;
	put_escaped(head[0]);
	put_escaped(head[1]);
	for (int i = 0; i < packet->len; ++i)
		put_escaped(packet->data[i]);
	put_escaped((Uint8)(crc >> 8));
	put_escaped((Uint8)crc);
	out[out_len++] = FRAME_END;
	if (!plat_net_send(out, (unsigned int)out_len))
	{
		/* a lost datagram; network.c resends what was not acknowledged */
		last_error = "link timeout";
	}
	packet->status = (int)out_len;
	return 1;  /* as UDP: sent = handed over, not delivered */
}

/* ---- receiving: bytes into frames ---- */
static Uint8 frame[MAX_PAYLOAD + 4];
static size_t frame_len;
static bool escaped, overflow;
static Uint8 in_buf[64];
static unsigned int in_len, in_pos;

static void frame_reset(void)
{
	frame_len = 0;
	escaped = false;
	overflow = false;
	in_len = in_pos = 0;
}

/* a complete frame in `frame`: valid -> into the packet */
static bool frame_done(UDPpacket *packet)
{
	const bool bad = overflow || frame_len < 4;
	const size_t len = bad ? 0 : (size_t)((frame[0] << 8) | frame[1]);
	frame_len = bad ? 0 : frame_len;
	overflow = false;
	escaped = false;
	if (bad || len + 4 != frame_len || (int)len > packet->maxlen)
	{
		frame_len = 0;
		return false;
	}
	Uint16 crc = crc16(frame, 2) ^ crc16(frame + 2, len);
	const Uint16 got = (Uint16)((frame[2 + len] << 8) | frame[3 + len]);
	frame_len = 0;
	if (crc != got)
		return false;
	memcpy(packet->data, frame + 2, len);
	packet->len = (int)len;
	packet->channel = 0;
	packet->status = (int)len;
	packet->address.host = 1;
	packet->address.port = 0;
	return true;
}

int SDLNet_UDP_Recv(UDPsocket sock, UDPpacket *packet)
{
	(void)sock;
	for (;;)
	{
		if (in_pos == in_len)
		{
			in_len = plat_net_recv(in_buf, sizeof in_buf);
			in_pos = 0;
			if (in_len == 0)
				return 0;  /* nothing (more) waiting */
		}
		const Uint8 b = in_buf[in_pos++];
		if (b == FRAME_END)
		{
			if (frame_len > 0 && frame_done(packet))
				return 1;
			frame_len = 0;
			escaped = false;
			overflow = false;
			continue;
		}
		Uint8 v = b;
		if (escaped)
		{
			v = b == FRAME_ESC_END ? FRAME_END : b == FRAME_ESC_ESC ? FRAME_ESC : b;
			escaped = false;
		}
		else if (b == FRAME_ESC)
		{
			escaped = true;
			continue;
		}
		if (frame_len < sizeof frame)
			frame[frame_len++] = v;
		else
			overflow = true;
	}
}

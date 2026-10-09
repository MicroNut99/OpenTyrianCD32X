/*
 * Tyrian 32X - the part of SDL_net OpenTyrian's network.c uses, on top of the
 * console link (port/sdlnet32x.c): datagrams over the cable between the two
 * consoles' controller port 2 (D32XR's transport, cart/net_link.s).
 */
#ifndef T32X_SDL_NET_H
#define T32X_SDL_NET_H

#include "SDL_types.h"

typedef struct { Uint32 host; Uint16 port; } IPaddress;
typedef struct T32X_UDPsocket *UDPsocket;
typedef struct
{
	int channel;
	Uint8 *data;
	int len;
	int maxlen;
	int status;
	IPaddress address;
} UDPpacket;

int SDLNet_Init(void);
void SDLNet_Quit(void);
const char *SDLNet_GetError(void);
int SDLNet_ResolveHost(IPaddress *address, const char *host, Uint16 port);
UDPsocket SDLNet_UDP_Open(Uint16 port);
void SDLNet_UDP_Close(UDPsocket sock);
int SDLNet_UDP_Bind(UDPsocket sock, int channel, const IPaddress *address);
UDPpacket *SDLNet_AllocPacket(int size);
void SDLNet_FreePacket(UDPpacket *packet);
int SDLNet_UDP_Send(UDPsocket sock, int channel, UDPpacket *packet);
int SDLNet_UDP_Recv(UDPsocket sock, UDPpacket *packet);

/* network byte order (big-endian), as SDL_net */
#define SDLNet_Write16(value, area) do { Uint8 *p_ = (Uint8 *)(area); Uint16 v_ = (Uint16)(value); \
	p_[0] = (Uint8)(v_ >> 8); p_[1] = (Uint8)v_; } while (0)
#define SDLNet_Write32(value, area) do { Uint8 *p_ = (Uint8 *)(area); Uint32 v_ = (Uint32)(value); \
	p_[0] = (Uint8)(v_ >> 24); p_[1] = (Uint8)(v_ >> 16); p_[2] = (Uint8)(v_ >> 8); p_[3] = (Uint8)v_; } while (0)
#define SDLNet_Read16(area) ((Uint16)((((const Uint8 *)(area))[0] << 8) | ((const Uint8 *)(area))[1]))
#define SDLNet_Read32(area) ((Uint32)(((Uint32)((const Uint8 *)(area))[0] << 24) | ((Uint32)((const Uint8 *)(area))[1] << 16) | \
	((Uint32)((const Uint8 *)(area))[2] << 8) | ((const Uint8 *)(area))[3]))

#endif

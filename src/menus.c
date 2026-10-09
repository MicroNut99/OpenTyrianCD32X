/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "menus.h"
#include "network.h"
#include "../port/plat.h"

#include "config.h"
#include "episodes.h"
#include "font.h"
#include "fonthand.h"
#include "keyboard.h"
#include "mouse.h"
#include "nortsong.h"
#include "opentyr.h"
#include "palette.h"
#include "picload.h"
#include "sprite.h"
#include "video.h"

char episode_name[6][31];
char difficulty_name[7][21];
char gameplay_name[5][26];

#if defined(TYRIAN32X) && defined(WITH_NETWORK)
/*
 * PORT32X: the console's network menu. On a PC, OpenTyrian starts a network
 * game from the command line (--net, --net-player-number); on the 32X this
 * menu sets the same things: the cable (link cable or serial, controller
 * port 2 to port 2 - D32XR's transport, cart/net_link.s) and which player
 * this console is (the two consoles must choose differently). Then the
 * network is set up (network_init) and newGame hands over to
 * networkStartScreen, as the main loop does on a PC.
 */
extern int t32x_net_type;  /* port/sdlnet32x.c */

static bool t32x_network_menu(void)
{
	static char name1[] = "PLAYER 1", name2[] = "PLAYER 2";
	const int xCenter = 320 / 2, yHeader = 20, yItems = 64, dyItems = 24;
	size_t selected = 2;
	bool serial = t32x_net_type < 0;
	int playerNum = thisPlayerNum == 2 ? 2 : 1;

	JE_loadPic(VGAScreen2, 2, false);
	drawFontHvShadowAligned(VGAScreen2, xCenter, yHeader, "Network Game", FONT_LARGE, ALIGN_CENTER, 15, -3, false, 2);
	drawFontHvShadowAligned(VGAScreen2, xCenter, 170, "Both consoles: cable in controller port 2.", FONT_SMALL, ALIGN_CENTER, 15, -4, false, 1);
	drawFontHvShadowAligned(VGAScreen2, xCenter, 180, "Choose a different player on each one.", FONT_SMALL, ALIGN_CENTER, 15, -4, false, 1);

	for (bool first = true; ; first = false)
	{
		setFrameCount(1);
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);
		const char *items[3] = {
			serial ? "Serial cable (4800 baud)" : "Link cable",
			playerNum == 1 ? "I am player 1" : "I am player 2",
			"Connect" };
		for (size_t i = 0; i < 3; ++i)
			drawFontHvShadowAligned(VGAScreen, xCenter, yItems + dyItems * (int)i, items[i], FONT_NORMAL,
			                        ALIGN_CENTER, 15, -4 + (i == selected ? 2 : 0), false, 2);
		JE_showVGA();
		if (first)
			fade_palette(colors, 10, 0, 255);

		waitUntilElapsed();
		waitUntilHasInput(INPUT_ANY);

		KeyboardInput k;
		if (!keyboardGetInput(&k))
			continue;
		switch (k.scancode)
		{
		case SDL_SCANCODE_UP:
			JE_playSampleNum(S_CURSOR);
			selected = selected == 0 ? 2 : selected - 1;
			break;
		case SDL_SCANCODE_DOWN:
			JE_playSampleNum(S_CURSOR);
			selected = selected == 2 ? 0 : selected + 1;
			break;
		case SDL_SCANCODE_LEFT:
		case SDL_SCANCODE_RIGHT:
		case SDL_SCANCODE_SPACE:
		case SDL_SCANCODE_RETURN:
			if (selected == 0)
				serial = !serial;
			else if (selected == 1)
				playerNum = 3 - playerNum;
			else if (k.scancode == SDL_SCANCODE_SPACE || k.scancode == SDL_SCANCODE_RETURN)
			{
				JE_playSampleNum(S_SELECT);
				t32x_net_type = serial ? NET_SERIAL : NET_LINK;
				thisPlayerNum = playerNum;
				network_player_name = playerNum == 1 ? name1 : name2;
				isNetworkGame = true;
				if (network_init() != 0)
				{
					isNetworkGame = false;  /* no link: stay in the menu */
					JE_playSampleNum(S_SPRING);
					break;
				}
				fade_black(10);
				return true;
			}
			JE_playSampleNum(S_CURSOR);
			break;
		case SDL_SCANCODE_ESCAPE:
			JE_playSampleNum(S_SPRING);
			fade_black(10);
			return false;
		default:
			break;
		}
	}
}
#endif

bool gameplaySelect(void)
{
	enum MenuItemIndex
	{
		MENU_ITEM_1_PLAYER_FULL_GAME = 0,
		MENU_ITEM_1_PLAYER_ARCADE,
		MENU_ITEM_2_PLAYER_ARCADE,
		MENU_ITEM_NETWORK,
	};

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = COUNTOF(gameplay_name) - 1;
	size_t selectedIndex = MENU_ITEM_1_PLAYER_FULL_GAME;

	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int yMenuItems = 54;
	const int dyMenuItems = 24;
	const int hMenuItem = 13;
	int wMenuItem[COUNTOF(gameplay_name) - 1] = { 0 };

	for (; ; )
	{
		setFrameCount(1);

		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			drawFontHvShadowAligned(VGAScreen2, xCenter, yMenuHeader, gameplay_name[0], FONT_LARGE, ALIGN_CENTER, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const char *const text = gameplay_name[i + 1];

			wMenuItem[i] = JE_textWidth(text, FONT_NORMAL);
			const int x = xCenter - wMenuItem[i] / 2;
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;
#if defined(TYRIAN32X) && defined(WITH_NETWORK)
			const bool disabled = false;  // PORT32X: the console network menu
#else
			const bool disabled = i == MENU_ITEM_NETWORK;
#endif

			drawFontHvShadow(VGAScreen, x, y, text, FONT_NORMAL, 15, -4 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
		}

		if (restart)
		{
			mouseCursor = MOUSE_POINTER_NORMAL;

			fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		waitUntilElapsed();
		waitUntilHasInput(INPUT_ANY);

		// Handle interaction.

		bool action = false;
		bool cancel = false;

		MouseInput mouseInput;
		KeyboardInput keyboardInput;

		if (mouseGetInput(INPUT_ANY, &mouseInput))
		{
			// Find menu item that was hovered or clicked.
			for (size_t i = 0; i < menuItemsCount; ++i)
			{
				const int xMenuItem = xCenter - wMenuItem[i] / 2;
				if (mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem[i])
				{
					const int yMenuItem = yMenuItems + dyMenuItems * i;
					if (mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
					{
						if (selectedIndex != i)
						{
							JE_playSampleNum(S_CURSOR);

							selectedIndex = i;
						}

						if (mouseInput.button == SDL_BUTTON_LEFT &&
						    mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem[i] &&
						    mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
						{
							action = true;
						}

						break;
					}
				}
			}

			if (mouseInput.button == SDL_BUTTON_RIGHT)
			{
				JE_playSampleNum(S_SPRING);

				cancel = true;
			}
		}
		else if (keyboardGetInput(&keyboardInput))
		{
			switch (keyboardInput.scancode)
			{
			case SDL_SCANCODE_UP:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == 0
					? menuItemsCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsCount - 1
					? 0
					: selectedIndex + 1;
				break;
			}
			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
			{
				action = true;
				break;
			}
			case SDL_SCANCODE_ESCAPE:
			{
				JE_playSampleNum(S_SPRING);

				cancel = true;
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			switch (selectedIndex)
			{
			case MENU_ITEM_1_PLAYER_FULL_GAME:
			case MENU_ITEM_1_PLAYER_ARCADE:
			case MENU_ITEM_2_PLAYER_ARCADE:
			{
				JE_playSampleNum(S_SELECT);

				fade_black(10);

				onePlayerAction = selectedIndex == MENU_ITEM_1_PLAYER_ARCADE;
				twoPlayerMode = selectedIndex == MENU_ITEM_2_PLAYER_ARCADE;
				return true;
			}
			case MENU_ITEM_NETWORK:
			{
#if defined(TYRIAN32X) && defined(WITH_NETWORK)
				fade_black(10);
				if (t32x_network_menu())
					return true;  // newGame continues with networkStartScreen
				restart = true;
#else
				JE_playSampleNum(S_SPRING);
#endif
				break;
			}
			default:
				break;
			}
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}

bool episodeSelect(void)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = EPISODE_AVAILABLE;
	size_t selectedIndex = 0;

	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int xMenuItem = 20;
	const int yMenuItems = 50;
	const int dyMenuItems = 30;
	const int hMenuItem = 13;
	int wMenuItem[EPISODE_AVAILABLE] = { 0 };

	for (; ; )
	{
		setFrameCount(1);

		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			drawFontHvShadowAligned(VGAScreen2, xCenter, yMenuHeader, episode_name[0], FONT_LARGE, ALIGN_CENTER, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const char *const text = episode_name[i + 1];

			wMenuItem[i] = JE_textWidth(text, FONT_NORMAL);
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;
			const bool disabled = !episodeAvail[i];

			drawFontHvShadow(VGAScreen, xMenuItem, y, text, FONT_NORMAL, 15, -4 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
		}

		if (restart)
		{
			mouseCursor = MOUSE_POINTER_NORMAL;

			fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		waitUntilElapsed();
		waitUntilHasInput(INPUT_ANY);

		// Handle interaction.

		bool action = false;
		bool cancel = false;

		MouseInput mouseInput;
		KeyboardInput keyboardInput;

		if (mouseGetInput(INPUT_ANY, &mouseInput))
		{
			// Find menu item that was hovered or clicked.
			for (size_t i = 0; i < menuItemsCount; ++i)
			{
				if (mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem[i])
				{
					const int yMenuItem = yMenuItems + dyMenuItems * i;
					if (mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
					{
						if (selectedIndex != i)
						{
							JE_playSampleNum(S_CURSOR);

							selectedIndex = i;
						}

						if (mouseInput.button == SDL_BUTTON_LEFT &&
						    mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem[i] &&
						    mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
						{
							action = true;
						}

						break;
					}
				}
			}

			if (mouseInput.button == SDL_BUTTON_RIGHT)
			{
				JE_playSampleNum(S_SPRING);

				cancel = true;
			}
		}
		else if (keyboardGetInput(&keyboardInput))
		{
			switch (keyboardInput.scancode)
			{
			case SDL_SCANCODE_UP:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == 0
					? menuItemsCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsCount - 1
					? 0
					: selectedIndex + 1;
				break;
			}
			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
			{
				action = true;
				break;
			}
			case SDL_SCANCODE_ESCAPE:
			{
				JE_playSampleNum(S_SPRING);

				cancel = true;
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			if (episodeAvail[selectedIndex])
			{
				JE_playSampleNum(S_SELECT);

				fade_black(10);

				JE_initEpisode(selectedIndex + 1);
				initial_episode_num = episodeNum;
				return true;
			}
			else
			{
				JE_playSampleNum(S_SPRING);
			}
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}

bool difficultySelect(void)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = COUNTOF(difficulty_name) - 1;
	size_t menuItemsVisibleCount = menuItemsCount - 3;
	size_t selectedIndex = 1;
	size_t lordProgress = 0;

	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int yMenuItems = 54;
	const int dyMenuItems = 24;
	const int hMenuItem = 13;
	int wMenuItem[COUNTOF(difficulty_name) - 1] = { 0 };

	for (; ; )
	{
		setFrameCount(1);

		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			drawFontHvShadowAligned(VGAScreen2, xCenter, yMenuHeader, difficulty_name[0], FONT_LARGE, ALIGN_CENTER, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsVisibleCount; ++i)
		{
			const char *const text = difficulty_name[i + 1];

			wMenuItem[i] = JE_textWidth(text, FONT_NORMAL);
			const int x = xCenter - wMenuItem[i] / 2;
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;

			drawFontHvShadow(VGAScreen, x, y, text, FONT_NORMAL, 15, -4 + (selected ? 2 : 0), false, 2);
		}

		if (restart)
		{
			mouseCursor = MOUSE_POINTER_NORMAL;

			fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		waitUntilElapsed();
		waitUntilHasInput(INPUT_ANY);

		// Handle interaction.

		bool action = false;
		bool cancel = false;

		MouseInput mouseInput;
		KeyboardInput keyboardInput;

		if (mouseGetInput(INPUT_ANY, &mouseInput))
		{
			// Find menu item that was hovered or clicked.
			for (size_t i = 0; i < menuItemsVisibleCount; ++i)
			{
				const int xMenuItem = xCenter - wMenuItem[i] / 2;
				if (mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem[i])
				{
					const int yMenuItem = yMenuItems + dyMenuItems * i;
					if (mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
					{
						if (selectedIndex != i)
						{
							JE_playSampleNum(S_CURSOR);

							selectedIndex = i;
						}

						if (mouseInput.button == SDL_BUTTON_LEFT &&
						    mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem[i] &&
						    mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
						{
							action = true;
						}

						break;
					}
				}
			}

			if (mouseInput.button == SDL_BUTTON_RIGHT)
			{
				JE_playSampleNum(S_SPRING);

				cancel = true;
			}
		}
		else if (keyboardGetInput(&keyboardInput))
		{
			switch (keyboardInput.scancode)
			{
			case SDL_SCANCODE_UP:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == 0
					? menuItemsVisibleCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsVisibleCount - 1
					? 0
					: selectedIndex + 1;
				break;
			}
			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
			{
				action = true;
				break;
			}
			case SDL_SCANCODE_ESCAPE:
			{
				JE_playSampleNum(S_SPRING);

				cancel = true;
				break;
			}
			default:
				break;
			}

			switch (menuItemsVisibleCount)
			{
			case 3:
			{
				if (keyboardInput.mod & KMOD_SHIFT &&
				    keyboardInput.sym == SDLK_g)
				{
					menuItemsVisibleCount = 4;
				}
				break;
			}
			case 4:
			{
				if (keyboardInput.mod & KMOD_SHIFT &&
				    keyboardInput.sym == SDLK_RIGHTBRACKET)
				{
					menuItemsVisibleCount = 5;
				}
				break;
			}
			case 5:
			{
				for (size_t i = 0; ; ++i)
				{
					if (i == COUNTOF(lordKeySymsDown))
					{
						menuItemsVisibleCount = 6;
						break;
					}

					if (!lordKeySymsDown[i])
						break;
				}

				// Due to key rollover, holding down 4 keys simultaneous may not always be possible,
				// so allow for typing them sequentially as well.
				if (lordProgress < COUNTOF(lordKeySyms) &&
				    keyboardInput.sym == lordKeySyms[lordProgress])
				{
					lordProgress += 1;

					if (lordProgress == COUNTOF(lordKeySyms))
						menuItemsVisibleCount = 6;
				}
				else
				{
					lordProgress = 0;
				}
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			JE_playSampleNum(S_SELECT);

			switch (selectedIndex)
			{
			case 0:
				difficultyLevel = DIFFICULTY_EASY;
				break;
			case 1:
				difficultyLevel = DIFFICULTY_NORMAL;
				break;
			case 2:
				difficultyLevel = DIFFICULTY_HARD;
				break;
			case 3:
				difficultyLevel = DIFFICULTY_IMPOSSIBLE;
				break;
			case 4:
				difficultyLevel = DIFFICULTY_SUICIDE;
				break;
			case 5:
				difficultyLevel = DIFFICULTY_ZINGLON;
				break;
			}

			fade_black(10);

			return true;
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}

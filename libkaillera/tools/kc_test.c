/* libkaillera - console test client. LGPL-2.1-or-later, see LICENSE.

   kc_test HOST PORT NAME create ROM FRAMES CONN   creates a game, starts it when a second player
                                                    is in, plays FRAMES frames
   kc_test HOST PORT NAME join ROM FRAMES CONN     joins the first game of ROM, plays FRAMES frames
   kc_test HOST PORT NAME lobby SECONDS            logs in, prints what happens, leaves
   kc_test HOST PORT ping                          PING/PONG round trip

   Every frame the input is 2 bytes made from the player number and the frame number; every frame
   received is written as one hex line to NAME.frames, so that the files of the players can be
   compared (they must be identical). */
#include "kaillera/kc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
static unsigned now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}
#else
#include <windows.h>
static unsigned now_ms(void) { return GetTickCount(); }
static void sleep_ms(int ms) { Sleep(ms); }
#endif

static void print_event(const kc_event *e)
{
	switch (e->type)
	{
	case KC_EV_CONNECTED: printf("* connected as %s (id %d)\n", e->name, e->user_id); break;
	case KC_EV_REJECTED: printf("* rejected: %s\n", e->text); break;
	case KC_EV_DISCONNECTED: printf("* disconnected: %s\n", e->text); break;
	case KC_EV_SERVER_INFO: printf("[%s] %s\n", e->name, e->text); break;
	case KC_EV_USER_JOINED: printf("* %s joined (id %d, ping %d, conn %d)\n", e->name, e->user_id, e->ping, e->conn); break;
	case KC_EV_USER_LEFT: printf("* %s left: %s\n", e->name, e->text); break;
	case KC_EV_CHAT: printf("<%s> %s\n", e->name, e->text); break;
	case KC_EV_GAME_CHAT: printf("{%s} %s\n", e->name, e->text); break;
	case KC_EV_GAME_CREATED: printf("* game %u created by %s: %s (%s)\n", e->game_id, e->name, e->game, e->emulator); break;
	case KC_EV_GAME_CLOSED: printf("* game %u closed\n", e->game_id); break;
	case KC_EV_ROOM_JOINED: printf("* in room of game %u\n", e->game_id); break;
	case KC_EV_ROOM_LEFT: printf("* left the room\n"); break;
	case KC_EV_GAME_START: printf("* start: %s, player %d of %d, delay %d\n", e->game, e->player, e->num_players, e->delay); break;
	case KC_EV_GAME_PLAYING: printf("* all ready, playing\n"); break;
	case KC_EV_PLAYER_DROPPED: printf("* %s (player %d) dropped\n", e->name, e->player); break;
	case KC_EV_GAME_ENDED: printf("* game ended\n"); break;
	default: break;
	}
	fflush(stdout);
}

static int wait_event(kc_client *c, int type, int timeout_ms, kc_event *out)
{
	unsigned t0 = now_ms();
	kc_event e;
	while ((int) (now_ms() - t0) < timeout_ms)
	{
		if (kc_poll(c, 100, &e))
		{
			print_event(&e);
			if (e.type == type)
			{
				if (out) *out = e;
				return 1;
			}
			if (e.type == KC_EV_DISCONNECTED || e.type == KC_EV_REJECTED)
				return 0;
		}
	}
	return 0;
}

static int play(kc_client *c, const char *name, int frames, const kc_event *start)
{
	char fn[256];
	FILE *f;
	int i, stalls = 0;
	unsigned t0, waited = 0;
	snprintf(fn, sizeof(fn), "%s.frames", name);
	f = fopen(fn, "w");
	kc_game_ready(c);
	t0 = now_ms();
	for (i = 0; i < frames; i++)
	{
		unsigned char in[2], out[KC_MAX_PLAYERS * 2];
		int n, k;
		unsigned tf = now_ms(), tw;
		in[0] = (unsigned char) start->player;
		in[1] = (unsigned char) (i * 7 + start->player);
		do
		{
			tw = now_ms();
			n = kc_game_frame(c, in, 2, out, 5000);
			if (n == 0)
			{
				printf("! frame %d: nothing for 5 s\n", i);
				stalls++;
				if (stalls > 3)
					return -1;
			}
		} while (n == 0);
		waited += now_ms() - tw;
		if (n < 0)
		{
			printf("! frame %d: game over\n", i);
			break;
		}
		for (k = 0; k < n; k++)
			fprintf(f, "%02X", out[k]);
		fputc('\n', f);
		/* 60 frames per second, like an emulator */
		{
			int left = 16 - (int) (now_ms() - tf);
			if (left > 0)
				sleep_ms(left);
		}
	}
	fclose(f);
	printf("* %d frames in %u ms, %u ms of it waiting for the network\n", i, now_ms() - t0, waited);
	return 0;
}

int main(int argc, char **argv)
{
	kc_client *c;
	kc_event e;
	const char *host, *name, *mode;
	int port;

	if (argc >= 4 && !strcmp(argv[3], "ping"))
	{
		printf("ping: %d ms\n", kc_ping_server(argv[1], atoi(argv[2]), 3000));
		return 0;
	}
	if (argc < 5)
	{
		fprintf(stderr, "usage: see the top of kc_test.c\n");
		return 2;
	}
	host = argv[1];
	port = atoi(argv[2]);
	name = argv[3];
	mode = argv[4];
	c = kc_create();
	if (kc_connect(c, host, port, name, "Gens 2.10", argc >= 8 ? atoi(argv[7]) : KC_CONN_GOOD) != 0)
	{
		fprintf(stderr, "can't connect\n");
		return 1;
	}
	if (!wait_event(c, KC_EV_CONNECTED, 10000, NULL))
		return 1;

	if (!strcmp(mode, "lobby"))
	{
		unsigned t0 = now_ms();
		int secs = argc >= 6 ? atoi(argv[5]) : 5, i;
		kc_chat(c, "hello from libkaillera");
		while ((int) (now_ms() - t0) < secs * 1000)
			if (kc_poll(c, 100, &e))
				print_event(&e);
		printf("users:\n");
		for (i = 0; i < kc_user_count(c); i++)
			printf("  %s id %d ping %d conn %d status %d\n", kc_user_at(c, i)->name, kc_user_at(c, i)->id,
				kc_user_at(c, i)->ping, kc_user_at(c, i)->conn, kc_user_at(c, i)->status);
		printf("games:\n");
		for (i = 0; i < kc_game_count(c); i++)
			printf("  %u %s (%s) by %s %s status %d\n", kc_game_at(c, i)->id, kc_game_at(c, i)->rom,
				kc_game_at(c, i)->emulator, kc_game_at(c, i)->owner, kc_game_at(c, i)->players, kc_game_at(c, i)->status);
	}
	else if (!strcmp(mode, "create") && argc >= 7)
	{
		unsigned t0;
		kc_create_game(c, argv[5]);
		if (!wait_event(c, KC_EV_ROOM_JOINED, 5000, NULL))
			return 1;
		t0 = now_ms();
		while (kc_room_count(c) < 2 && (int) (now_ms() - t0) < 30000)
			if (kc_poll(c, 100, &e))
				print_event(&e);
		if (kc_room_count(c) < 2)
		{
			printf("! nobody joined\n");
			return 1;
		}
		kc_game_chat(c, "starting");
		kc_start_game(c);
		if (!wait_event(c, KC_EV_GAME_START, 5000, &e))
			return 1;
		play(c, name, atoi(argv[6]), &e);
		kc_quit_game(c);
	}
	else if (!strcmp(mode, "join") && argc >= 7)
	{
		unsigned t0 = now_ms();
		uint32_t id = 0;
		int i;
		while (!id && (int) (now_ms() - t0) < 30000)
		{
			if (kc_poll(c, 100, &e))
				print_event(&e);
			for (i = 0; i < kc_game_count(c); i++)
				if (!strcmp(kc_game_at(c, i)->rom, argv[5]) && kc_game_at(c, i)->status == KC_GAME_WAITING)
					id = kc_game_at(c, i)->id;
		}
		if (!id)
		{
			printf("! no game %s\n", argv[5]);
			return 1;
		}
		kc_join_game(c, id);
		if (!wait_event(c, KC_EV_GAME_START, 30000, &e))
			return 1;
		play(c, name, atoi(argv[6]), &e);
		kc_quit_game(c);
	}
	{
		unsigned t0 = now_ms();
		while ((int) (now_ms() - t0) < 500)
			if (kc_poll(c, 100, &e))
				print_event(&e);
	}
	kc_disconnect(c, "test over");
	kc_destroy(c);
	return 0;
}

/* libkaillera - a client for the Kaillera netplay protocol (0.83).
   Copyright (C) 2026 the Gens64 authors.

   This library is free software; you can redistribute it and/or modify it under the terms of the
   GNU Lesser General Public License as published by the Free Software Foundation; either version
   2.1 of the License, or (at your option) any later version. See LICENSE in this folder.

   The core: protocol, lobby state and the game input exchange, with no threads and no user
   interface. One kc_client is one connection to one server. Not thread-safe: the caller serialises
   all calls (the kailleraclient-compatible layer does it with a mutex).

   Usage:
     kc_client *c = kc_create();
     kc_connect(c, "gs.emu-land.net", 27888, "nick", "Gens 2.10", KC_CONN_GOOD);
     loop: kc_poll(c, timeout_ms, &ev) -> handle ev (lists: kc_user/kc_game)
     kc_chat / kc_create_game / kc_join_game / kc_start_game ...
     in a game: kc_game_ready(c) once the emulator is ready, then every frame
       kc_game_frame(c, my_input, size, all_inputs, timeout_ms)
     kc_disconnect(c, "bye"); kc_destroy(c);

   Strings are bytes as they go over the wire (the server's charset, usually the Windows code
   page of the players): converting them is up to the user interface. */
#ifndef KAILLERA_KC_H
#define KAILLERA_KC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KC_PROTOCOL_VERSION "0.83"
#define KC_DEFAULT_PORT 27888
#define KC_MAX_PLAYERS 8
#define KC_NAME_MAX 128		/* user, game and emulator names, including the 0 */
#define KC_TEXT_MAX 512		/* chat and server messages, including the 0 */

/* Connection types: how many frames go in one packet (LAN = every frame ... Bad = every 6). */
enum kc_conn_type
{
	KC_CONN_LAN = 1,
	KC_CONN_EXCELLENT = 2,
	KC_CONN_GOOD = 3,
	KC_CONN_AVERAGE = 4,
	KC_CONN_LOW = 5,
	KC_CONN_BAD = 6
};

enum kc_game_status
{
	KC_GAME_WAITING = 0,
	KC_GAME_PLAYING = 1,
	KC_GAME_NETSYNC = 2
};

enum kc_state
{
	KC_STATE_IDLE = 0,		/* not connected */
	KC_STATE_HELLO,			/* waiting for HELLOD00D */
	KC_STATE_LOGIN,			/* login sent, waiting for our own "user joined" */
	KC_STATE_LOBBY,			/* logged in, not in a game */
	KC_STATE_IN_GAME,		/* in a game room (created or joined), not started */
	KC_STATE_STARTING,		/* the game was started: waiting for the emulator to be ready */
	KC_STATE_SYNC,			/* ready sent, waiting for all players */
	KC_STATE_PLAYING		/* exchanging input */
};

enum kc_event_type
{
	KC_EV_NONE = 0,
	KC_EV_CONNECTED,		/* logged in: user_id = our id */
	KC_EV_REJECTED,			/* server full, refused login (text) */
	KC_EV_DISCONNECTED,		/* timeout or kc_disconnect (text) */
	KC_EV_SERVER_INFO,		/* text from the server (name = "Server" or a source) */
	KC_EV_USERS_CHANGED,	/* the user list changed (name/user_id: who; 0 for a full refresh) */
	KC_EV_USER_JOINED,		/* name, user_id, ping, conn */
	KC_EV_USER_LEFT,		/* name, user_id, text = quit message */
	KC_EV_CHAT,				/* name, text */
	KC_EV_GAME_CHAT,		/* name, text */
	KC_EV_GAMES_CHANGED,	/* the game list changed (game_id: which; 0 for a full refresh) */
	KC_EV_GAME_CREATED,		/* game_id, game (rom name), emulator, name = owner */
	KC_EV_GAME_CLOSED,		/* game_id */
	KC_EV_ROOM_CHANGED,		/* the players of our room changed (kc_room_player) */
	KC_EV_ROOM_JOINED,		/* we are in a room: game_id */
	KC_EV_ROOM_LEFT,		/* we left the room or were kicked, or it was closed */
	KC_EV_GAME_START,		/* the game starts: game, player (1-based), num_players, delay;
							   load the game, then call kc_game_ready */
	KC_EV_GAME_PLAYING,		/* all players ready: the input exchange begins */
	KC_EV_PLAYER_DROPPED,	/* name, player: left the running game */
	KC_EV_GAME_ENDED		/* our running game ended (we dropped or the server ended it) */
};

typedef struct kc_event
{
	int type;
	char name[KC_NAME_MAX];
	char game[KC_NAME_MAX];
	char emulator[KC_NAME_MAX];
	char text[KC_TEXT_MAX];
	uint32_t game_id;
	int user_id;
	int ping;
	int conn;
	int player;
	int num_players;
	int delay;
} kc_event;

typedef struct kc_user
{
	char name[KC_NAME_MAX];
	int id;
	int ping;
	int conn;
	int status;				/* 0 playing, 1 idle */
} kc_user;

typedef struct kc_game
{
	char rom[KC_NAME_MAX];
	char emulator[KC_NAME_MAX];
	char owner[KC_NAME_MAX];
	char players[16];		/* "1/2" as the server sends it */
	uint32_t id;
	int status;				/* enum kc_game_status */
} kc_game;

typedef struct kc_client kc_client;

kc_client *kc_create(void);
void kc_destroy(kc_client *c);

/* Starts the login; the result comes as KC_EV_CONNECTED or KC_EV_REJECTED/DISCONNECTED.
   Returns 0, or -1 if the host can't be resolved or the socket can't be opened. */
int kc_connect(kc_client *c, const char *host, int port, const char *user, const char *emulator, int conn_type);
void kc_disconnect(kc_client *c, const char *message);

/* Handles the network for up to timeout_ms (0: only what has arrived) and returns 1 with the next
   event in *ev, or 0 if there is none. Call it often (the keep-alives are sent from here). */
int kc_poll(kc_client *c, int timeout_ms, kc_event *ev);

/* The socket, to wait on it in a select/poll of one's own (-1 if none). */
intptr_t kc_socket(kc_client *c);

int kc_state(kc_client *c);
int kc_user_count(kc_client *c);
const kc_user *kc_user_at(kc_client *c, int i);
int kc_game_count(kc_client *c);
const kc_game *kc_game_at(kc_client *c, int i);
const kc_game *kc_game_find(kc_client *c, uint32_t id);
int kc_room_count(kc_client *c);			/* players in our room, us included */
const kc_user *kc_room_player(kc_client *c, int i);
uint32_t kc_room_game(kc_client *c);		/* id of our room, 0 if none */
int kc_room_owner(kc_client *c);			/* 1 if we created our room */
int kc_my_id(kc_client *c);
const char *kc_my_name(kc_client *c);

/* Lobby requests. Return 0, or -1 when not possible in the current state. */
int kc_chat(kc_client *c, const char *text);
int kc_game_chat(kc_client *c, const char *text);
int kc_create_game(kc_client *c, const char *rom);
int kc_join_game(kc_client *c, uint32_t game_id);
int kc_quit_game(kc_client *c);			/* leave (or close, if ours) the room */
int kc_start_game(kc_client *c);		/* owner only */
int kc_kick(kc_client *c, int user_id);	/* owner only */

/* The game. kc_game_ready: the emulator has loaded the game. kc_game_frame: give our input for this
   frame (size bytes, the same every frame) and get all players' inputs for it, in player order,
   in out (KC_MAX_PLAYERS * size bytes). Returns the length written to out, 0 if it isn't there yet
   within timeout_ms (call again with the same input), or -1 if the game is over. */
int kc_game_ready(kc_client *c);
int kc_game_frame(kc_client *c, const void *in, int size, void *out, int timeout_ms);
int kc_game_drop(kc_client *c);			/* leave the running game */

/* Asks a server for a PONG (no login) and returns the round trip in ms, or -1 after timeout_ms. */
int kc_ping_server(const char *host, int port, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

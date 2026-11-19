/* libkaillera - the Kaillera 0.83 protocol client core. LGPL-2.1-or-later, see LICENSE.

   Written from the published protocol description (Anthem, Okai project, SupraFast, Moosehead:
   "Kaillera Network Protocol") and from the behaviour of the EmuLinker-K server; no code from
   other clients or servers. See docs/kaillera-protocol.md for what each message holds. */
#include "kaillera/kc.h"
#include "kc_net.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- message types ---- */
enum
{
	M_USER_QUIT = 0x01, M_USER_JOINED = 0x02, M_LOGIN = 0x03, M_SERVER_STATUS = 0x04,
	M_SERVER_ACK = 0x05, M_CLIENT_ACK = 0x06, M_CHAT = 0x07, M_GAME_CHAT = 0x08,
	M_KEEPALIVE = 0x09, M_CREATE_GAME = 0x0A, M_QUIT_GAME = 0x0B, M_JOIN_GAME = 0x0C,
	M_PLAYER_INFO = 0x0D, M_GAME_STATUS = 0x0E, M_KICK = 0x0F, M_CLOSE_GAME = 0x10,
	M_START_GAME = 0x11, M_GAME_DATA = 0x12, M_GAME_CACHE = 0x13, M_DROP_GAME = 0x14,
	M_READY = 0x15, M_REJECTED = 0x16, M_SERVER_INFO = 0x17
};

#define HISTORY 9				/* messages repeated in every packet: the most a server takes in
								   one bundle (EmuLinker: 9); a lost or late packet must never
								   leave a gap, the server takes a gap for a desync */
#define MAX_PACKET 4096
#define CACHE_SLOTS 256
#define HELLO_RETRY_MS 1000
#define HELLO_TRIES 5
#define LOGIN_RETRY_MS 200		/* the server times the ACKs for the ping: answer a lost one fast */
#define LOGIN_TIMEOUT_MS 10000
#define KEEPALIVE_MS 30000
#define POKE_MS 1000			/* lobby: send our last packet again, so that the server sends its
								   recent messages again (a lost one is found by the next one) */
#define GAME_RESEND_MS 100
#define GAP_RESEND_MS 30		/* a message from the server is missing: ask again */		/* in a game, resend the last packet if nothing came back */

typedef struct
{
	uint8_t data[MAX_PACKET];
	int len;
} msgbuf;

typedef struct
{
	uint8_t *slot[CACHE_SLOTS];
	int len[CACHE_SLOTS];
	int count;					/* slots used; logical 0 = oldest */
	int head;					/* ring index of logical 0 */
} cache;

struct kc_client
{
	kc_sock sock;
	struct sockaddr_in server;
	int state;
	uint32_t t_state, t_send, t_recv, t_resend, t_new;
	int tries;

	char user[KC_NAME_MAX], emulator[KC_NAME_MAX];
	int conn, my_id;

	/* sending: message numbers and the last messages, repeated in each packet */
	uint16_t tx_seq;
	msgbuf tx_hist[HISTORY];	/* tx_hist[0] = newest encoded message (seq, len, type, data) */
	int tx_hist_n;
	/* receiving: next message number expected (-1 = take the first one) */
	int rx_next;

	/* lists */
	kc_user *users;
	int n_users, cap_users;
	kc_game *games;
	int n_games, cap_games;
	kc_user room[KC_MAX_PLAYERS * 2];
	int n_room;
	uint32_t room_game;
	int room_owner;
	char room_rom[KC_NAME_MAX];

	/* events */
	kc_event *ev;
	int ev_head, ev_count, ev_cap;

	/* the game */
	int player, num_players, delay;
	int in_size;				/* bytes of one player's input per frame (0 = not known yet) */
	uint8_t *acc;				/* our inputs of the packet being filled */
	int acc_frames;
	int frame_pending;			/* the last kc_game_frame returned 0: its input is in acc already */
	int frame;					/* frames given to the emulator */
	uint8_t *rxq;				/* combined frames received and not used yet */
	int rxq_len, rxq_cap;
	cache out_cache, in_cache;
	int game_over;
};

/* ---- little helpers ---- */

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static unsigned get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t) get16(p + 2) << 16); }

static void copystr(char *dst, const char *src, size_t size)
{
	size_t n = src ? strlen(src) : 0;
	if (n >= size)
		n = size - 1;
	if (n)
		memcpy(dst, src, n);
	dst[n] = 0;
}

/* Reader over one message body: strings end with 0; a short body reads as zeros/empty. */
typedef struct
{
	const uint8_t *p, *end;
	int bad;
} reader;

static void rd_str(reader *r, char *dst, size_t size)
{
	size_t n = 0;
	while (r->p < r->end && *r->p)
	{
		if (n + 1 < size)
			dst[n++] = (char) *r->p;
		r->p++;
	}
	if (r->p < r->end)
		r->p++;					/* the 0 */
	else
		r->bad = 1;
	if (size)
		dst[n] = 0;
}

static unsigned rd8(reader *r)
{
	if (r->p + 1 > r->end) { r->bad = 1; return 0; }
	return *r->p++;
}

static unsigned rd16(reader *r)
{
	unsigned v;
	if (r->p + 2 > r->end) { r->bad = 1; r->p = r->end; return 0; }
	v = get16(r->p);
	r->p += 2;
	return v;
}

static uint32_t rd32(reader *r)
{
	uint32_t v;
	if (r->p + 4 > r->end) { r->bad = 1; r->p = r->end; return 0; }
	v = get32(r->p);
	r->p += 4;
	return v;
}

/* Writer for one message body */
typedef struct
{
	uint8_t buf[MAX_PACKET];
	int len;
} writer;

static void wr8(writer *w, unsigned v) { if (w->len < MAX_PACKET) w->buf[w->len++] = (uint8_t) v; }
static void wr16(writer *w, unsigned v) { wr8(w, v & 0xFF); wr8(w, (v >> 8) & 0xFF); }
static void wr32(writer *w, uint32_t v) { wr16(w, v & 0xFFFF); wr16(w, v >> 16); }
static void wrstr(writer *w, const char *s)
{
	while (s && *s)
		wr8(w, (uint8_t) *s++);
	wr8(w, 0);
}
static void wrbytes(writer *w, const void *d, int n)
{
	if (n > MAX_PACKET - w->len)
		n = MAX_PACKET - w->len;
	memcpy(w->buf + w->len, d, n);
	w->len += n;
}

/* ---- events ---- */

static kc_event *push_event(kc_client *c, int type)
{
	kc_event *e;
	if (c->ev_count == c->ev_cap)
	{
		int ncap = c->ev_cap ? c->ev_cap * 2 : 32, i;
		kc_event *n = (kc_event *) malloc(sizeof(kc_event) * ncap);
		if (!n)
			return NULL;
		for (i = 0; i < c->ev_count; i++)
			n[i] = c->ev[(c->ev_head + i) % c->ev_cap];
		free(c->ev);
		c->ev = n;
		c->ev_cap = ncap;
		c->ev_head = 0;
	}
	e = &c->ev[(c->ev_head + c->ev_count) % c->ev_cap];
	c->ev_count++;
	memset(e, 0, sizeof(*e));
	e->type = type;
	return e;
}

static int pop_event(kc_client *c, kc_event *out)
{
	if (!c->ev_count)
		return 0;
	*out = c->ev[c->ev_head];
	c->ev_head = (c->ev_head + 1) % c->ev_cap;
	c->ev_count--;
	return 1;
}

/* ---- the caches of game data (256 slots, logical 0 = oldest) ---- */

static void cache_clear(cache *k)
{
	int i;
	for (i = 0; i < CACHE_SLOTS; i++)
	{
		free(k->slot[i]);
		k->slot[i] = NULL;
	}
	k->count = k->head = 0;
}

static int cache_find(const cache *k, const uint8_t *d, int len)
{
	int i;
	for (i = k->count - 1; i >= 0; i--)	/* the newest copy, like the servers */
	{
		int s = (k->head + i) % CACHE_SLOTS;
		if (k->len[s] == len && !memcmp(k->slot[s], d, len))
			return i;
	}
	return -1;
}

static void cache_add(cache *k, const uint8_t *d, int len)
{
	int s;
	if (k->count == CACHE_SLOTS)
	{
		free(k->slot[k->head]);
		k->slot[k->head] = NULL;
		k->head = (k->head + 1) % CACHE_SLOTS;
		k->count--;
	}
	s = (k->head + k->count) % CACHE_SLOTS;
	k->slot[s] = (uint8_t *) malloc(len);
	if (!k->slot[s])
		return;
	memcpy(k->slot[s], d, len);
	k->len[s] = len;
	k->count++;
}

static const uint8_t *cache_get(const cache *k, int pos, int *len)
{
	int s;
	if (pos < 0 || pos >= k->count)
		return NULL;
	s = (k->head + pos) % CACHE_SLOTS;
	*len = k->len[s];
	return k->slot[s];
}

/* ---- sending ---- */

static void send_raw(kc_client *c, const void *d, int len)
{
	if (kc_net_send(c->sock, &c->server, d, len) == 0)
		c->t_send = kc_net_ms();
}

/* Sends the history: [count][newest msg][older]... */
static void send_history(kc_client *c)
{
	uint8_t pkt[1 + HISTORY * MAX_PACKET];
	int i, len = 1;
	pkt[0] = (uint8_t) c->tx_hist_n;
	for (i = 0; i < c->tx_hist_n; i++)
	{
		memcpy(pkt + len, c->tx_hist[i].data, c->tx_hist[i].len);
		len += c->tx_hist[i].len;
	}
	send_raw(c, pkt, len);
	c->t_resend = kc_net_ms();
}

static void queue_msg(kc_client *c, int type, const writer *body)
{
	msgbuf *m;
	int i;
	for (i = HISTORY - 1; i > 0; i--)
		c->tx_hist[i] = c->tx_hist[i - 1];
	m = &c->tx_hist[0];
	c->t_new = kc_net_ms();
	put16(m->data, c->tx_seq++);
	put16(m->data + 2, body->len + 1);
	m->data[4] = (uint8_t) type;
	memcpy(m->data + 5, body->buf, body->len);
	m->len = body->len + 5;
	if (c->tx_hist_n < HISTORY)
		c->tx_hist_n++;
}

static void send_msg(kc_client *c, int type, const writer *body)
{
	queue_msg(c, type, body);
	send_history(c);
}

static void send_login(kc_client *c)
{
	writer w = { {0}, 0 };
	wrstr(&w, c->user);
	wrstr(&w, c->emulator);
	wr8(&w, c->conn);
	send_msg(c, M_LOGIN, &w);
}

/* ---- the lists ---- */

static kc_user *user_find(kc_client *c, int id, int *idx)
{
	int i;
	for (i = 0; i < c->n_users; i++)
		if (c->users[i].id == id)
		{
			if (idx) *idx = i;
			return &c->users[i];
		}
	return NULL;
}

static kc_user *user_add(kc_client *c, int id)
{
	kc_user *u = user_find(c, id, NULL);
	if (u)
		return u;
	if (c->n_users == c->cap_users)
	{
		int ncap = c->cap_users ? c->cap_users * 2 : 32;
		kc_user *n = (kc_user *) realloc(c->users, sizeof(kc_user) * ncap);
		if (!n)
			return NULL;
		c->users = n;
		c->cap_users = ncap;
	}
	u = &c->users[c->n_users++];
	memset(u, 0, sizeof(*u));
	u->id = id;
	u->status = 1;
	return u;
}

static void user_remove(kc_client *c, int id)
{
	int i;
	if (user_find(c, id, &i))
		c->users[i] = c->users[--c->n_users];
}

static kc_game *game_find(kc_client *c, uint32_t id, int *idx)
{
	int i;
	for (i = 0; i < c->n_games; i++)
		if (c->games[i].id == id)
		{
			if (idx) *idx = i;
			return &c->games[i];
		}
	return NULL;
}

static kc_game *game_add(kc_client *c, uint32_t id)
{
	kc_game *g = game_find(c, id, NULL);
	if (g)
		return g;
	if (c->n_games == c->cap_games)
	{
		int ncap = c->cap_games ? c->cap_games * 2 : 16;
		kc_game *n = (kc_game *) realloc(c->games, sizeof(kc_game) * ncap);
		if (!n)
			return NULL;
		c->games = n;
		c->cap_games = ncap;
	}
	g = &c->games[c->n_games++];
	memset(g, 0, sizeof(*g));
	g->id = id;
	return g;
}

static void game_remove(kc_client *c, uint32_t id)
{
	int i;
	if (game_find(c, id, &i))
		c->games[i] = c->games[--c->n_games];
}

static void room_add(kc_client *c, const char *name, int id, int ping, int conn)
{
	int i;
	for (i = 0; i < c->n_room; i++)
		if (c->room[i].id == id)
			return;
	if (c->n_room >= (int) (sizeof(c->room) / sizeof(c->room[0])))
		return;
	memset(&c->room[c->n_room], 0, sizeof(kc_user));
	copystr(c->room[c->n_room].name, name, KC_NAME_MAX);
	c->room[c->n_room].id = id;
	c->room[c->n_room].ping = ping;
	c->room[c->n_room].conn = conn;
	c->n_room++;
}

static void room_remove(kc_client *c, int id)
{
	int i;
	for (i = 0; i < c->n_room; i++)
		if (c->room[i].id == id)
		{
			memmove(&c->room[i], &c->room[i + 1], (c->n_room - i - 1) * sizeof(kc_user));
			c->n_room--;
			return;
		}
}

static void game_reset(kc_client *c)
{
	free(c->acc);
	c->acc = NULL;
	free(c->rxq);
	c->rxq = NULL;
	c->rxq_len = c->rxq_cap = 0;
	c->acc_frames = 0;
	c->in_size = 0;
	c->frame_pending = 0;
	c->frame = 0;
	c->game_over = 0;
	cache_clear(&c->out_cache);
	cache_clear(&c->in_cache);
}

static void leave_room(kc_client *c, int ended_game)
{
	int was_playing = c->state >= KC_STATE_STARTING;
	c->room_game = 0;
	c->room_owner = 0;
	c->n_room = 0;
	c->room_rom[0] = 0;
	if (c->state >= KC_STATE_IN_GAME)
		c->state = KC_STATE_LOBBY;
	if (was_playing || ended_game)
	{
		c->game_over = 1;
		push_event(c, KC_EV_GAME_ENDED);
	}
	push_event(c, KC_EV_ROOM_LEFT);
}

/* ---- game data received ---- */

static void rxq_append(kc_client *c, const uint8_t *d, int len)
{
	if (c->rxq_len + len > c->rxq_cap)
	{
		int ncap = (c->rxq_len + len) * 2 + 256;
		uint8_t *n = (uint8_t *) realloc(c->rxq, ncap);
		if (!n)
			return;
		c->rxq = n;
		c->rxq_cap = ncap;
	}
	memcpy(c->rxq + c->rxq_len, d, len);
	c->rxq_len += len;
}

/* ---- receiving ---- */

static void handle_message(kc_client *c, int type, const uint8_t *body, int len)
{
	reader r = { body, body + len, 0 };
	kc_event *e;
	char name[KC_NAME_MAX], text[KC_TEXT_MAX], s1[KC_NAME_MAX], s2[KC_NAME_MAX];
	writer w = { {0}, 0 };

	switch (type)
	{
	case M_SERVER_ACK:
		wr8(&w, 0);
		wr32(&w, 0); wr32(&w, 1); wr32(&w, 2); wr32(&w, 3);
		send_msg(c, M_CLIENT_ACK, &w);
		break;

	case M_SERVER_STATUS:
	{
		uint32_t nu, ng, i;
		rd_str(&r, name, sizeof(name));
		nu = rd32(&r);
		ng = rd32(&r);
		for (i = 0; i < nu && !r.bad; i++)
		{
			kc_user tmp;
			kc_user *u;
			memset(&tmp, 0, sizeof(tmp));
			rd_str(&r, tmp.name, sizeof(tmp.name));
			tmp.ping = (int) rd32(&r);
			tmp.status = (int) rd8(&r);
			tmp.id = (int) rd16(&r);
			tmp.conn = (int) rd8(&r);
			if (r.bad)
				break;
			u = user_add(c, tmp.id);
			if (u)
				*u = tmp;
		}
		for (i = 0; i < ng && !r.bad; i++)
		{
			kc_game tmp;
			kc_game *g;
			memset(&tmp, 0, sizeof(tmp));
			rd_str(&r, tmp.rom, sizeof(tmp.rom));
			tmp.id = rd32(&r);
			rd_str(&r, tmp.emulator, sizeof(tmp.emulator));
			rd_str(&r, tmp.owner, sizeof(tmp.owner));
			rd_str(&r, tmp.players, sizeof(tmp.players));
			tmp.status = (int) rd8(&r);
			if (r.bad)
				break;
			g = game_add(c, tmp.id);
			if (g)
				*g = tmp;
		}
		push_event(c, KC_EV_USERS_CHANGED);
		push_event(c, KC_EV_GAMES_CHANGED);
		break;
	}

	case M_USER_JOINED:
	{
		int id, ping, conn;
		kc_user *u;
		rd_str(&r, name, sizeof(name));
		id = (int) rd16(&r);
		ping = (int) rd32(&r);
		conn = (int) rd8(&r);
		u = user_add(c, id);
		if (u)
		{
			copystr(u->name, name, KC_NAME_MAX);
			u->ping = ping;
			u->conn = conn;
			u->status = 1;
		}
		if (c->state == KC_STATE_LOGIN && !strcmp(name, c->user))
		{
			c->my_id = id;
			c->state = KC_STATE_LOBBY;
			if ((e = push_event(c, KC_EV_CONNECTED)) != NULL)
			{
				copystr(e->name, name, KC_NAME_MAX);
				e->user_id = id;
				e->ping = ping;
			}
		}
		if ((e = push_event(c, KC_EV_USER_JOINED)) != NULL)
		{
			copystr(e->name, name, KC_NAME_MAX);
			e->user_id = id;
			e->ping = ping;
			e->conn = conn;
		}
		break;
	}

	case M_USER_QUIT:
	{
		int id;
		rd_str(&r, name, sizeof(name));
		id = (int) rd16(&r);
		rd_str(&r, text, sizeof(text));
		user_remove(c, id);
		if ((e = push_event(c, KC_EV_USER_LEFT)) != NULL)
		{
			copystr(e->name, name, KC_NAME_MAX);
			copystr(e->text, text, KC_TEXT_MAX);
			e->user_id = id;
		}
		break;
	}

	case M_CHAT:
	case M_GAME_CHAT:
	case M_SERVER_INFO:
		rd_str(&r, name, sizeof(name));
		rd_str(&r, text, sizeof(text));
		if ((e = push_event(c, type == M_CHAT ? KC_EV_CHAT : type == M_GAME_CHAT ? KC_EV_GAME_CHAT : KC_EV_SERVER_INFO)) != NULL)
		{
			copystr(e->name, name, KC_NAME_MAX);
			copystr(e->text, text, KC_TEXT_MAX);
		}
		break;

	case M_CREATE_GAME:
	{
		uint32_t id;
		kc_game *g;
		rd_str(&r, name, sizeof(name));		/* owner */
		rd_str(&r, s1, sizeof(s1));			/* rom */
		rd_str(&r, s2, sizeof(s2));			/* emulator */
		id = rd32(&r);
		g = game_add(c, id);
		if (g)
		{
			copystr(g->rom, s1, KC_NAME_MAX);
			copystr(g->emulator, s2, KC_NAME_MAX);
			copystr(g->owner, name, KC_NAME_MAX);
			copystr(g->players, "1/2", sizeof(g->players));
			g->status = KC_GAME_WAITING;
		}
		if ((e = push_event(c, KC_EV_GAME_CREATED)) != NULL)
		{
			copystr(e->name, name, KC_NAME_MAX);
			copystr(e->game, s1, KC_NAME_MAX);
			copystr(e->emulator, s2, KC_NAME_MAX);
			e->game_id = id;
		}
		/* our own game: we are in its room */
		if (c->state == KC_STATE_LOBBY && c->room_owner == 2 && !strcmp(name, c->user))
		{
			c->room_game = id;
			c->room_owner = 1;
			copystr(c->room_rom, s1, KC_NAME_MAX);
			c->state = KC_STATE_IN_GAME;
			c->n_room = 0;
			room_add(c, c->user, c->my_id, 0, c->conn);
			if ((e = push_event(c, KC_EV_ROOM_JOINED)) != NULL)
			{
				e->game_id = id;
				copystr(e->game, s1, KC_NAME_MAX);
			}
			push_event(c, KC_EV_ROOM_CHANGED);
		}
		push_event(c, KC_EV_GAMES_CHANGED);
		break;
	}

	case M_GAME_STATUS:
	{
		uint32_t id;
		int st, np, mp;
		kc_game *g;
		rd8(&r);
		id = rd32(&r);
		st = (int) rd8(&r);
		np = (int) rd8(&r);
		mp = (int) rd8(&r);
		g = game_find(c, id, NULL);
		if (g)
		{
			g->status = st;
			snprintf(g->players, sizeof(g->players), "%d/%d", np, mp);
		}
		if ((e = push_event(c, KC_EV_GAMES_CHANGED)) != NULL)
			e->game_id = id;
		break;
	}

	case M_CLOSE_GAME:
	{
		uint32_t id;
		rd8(&r);
		id = rd32(&r);
		game_remove(c, id);
		if ((e = push_event(c, KC_EV_GAME_CLOSED)) != NULL)
			e->game_id = id;
		push_event(c, KC_EV_GAMES_CHANGED);
		if (id == c->room_game)
			leave_room(c, 0);
		break;
	}

	case M_JOIN_GAME:					/* someone (maybe us) joined the room we are in */
	{
		uint32_t gid;
		int ping, id, conn;
		rd8(&r);
		gid = rd32(&r);
		rd_str(&r, name, sizeof(name));
		ping = (int) rd32(&r);
		id = (int) rd16(&r);
		conn = (int) rd8(&r);
		if (id == c->my_id && c->state == KC_STATE_LOBBY)
		{
			c->state = KC_STATE_IN_GAME;
			if (!c->room_game)
				c->room_game = gid;
			if ((e = push_event(c, KC_EV_ROOM_JOINED)) != NULL)
				e->game_id = c->room_game;
		}
		room_add(c, name, id, ping, conn);
		push_event(c, KC_EV_ROOM_CHANGED);
		break;
	}

	case M_PLAYER_INFO:					/* the players already in the room we joined */
	{
		uint32_t n, i;
		rd8(&r);
		n = rd32(&r);
		c->n_room = 0;
		for (i = 0; i < n && !r.bad; i++)
		{
			int ping, id, conn;
			rd_str(&r, name, sizeof(name));
			ping = (int) rd32(&r);
			id = (int) rd16(&r);
			conn = (int) rd8(&r);
			if (!r.bad)
				room_add(c, name, id, ping, conn);
		}
		push_event(c, KC_EV_ROOM_CHANGED);
		break;
	}

	case M_QUIT_GAME:					/* someone (maybe us) left the room */
	{
		int id;
		rd_str(&r, name, sizeof(name));
		id = (int) rd16(&r);
		if (id == c->my_id)
		{
			if (c->state >= KC_STATE_IN_GAME)
				leave_room(c, 0);
		}
		else
		{
			room_remove(c, id);
			push_event(c, KC_EV_ROOM_CHANGED);
		}
		break;
	}

	case M_START_GAME:
		rd8(&r);
		c->delay = (int) rd16(&r);
		c->player = (int) rd8(&r);
		c->num_players = (int) rd8(&r);
		if (c->num_players < 1 || c->num_players > KC_MAX_PLAYERS)
			c->num_players = c->num_players < 1 ? 1 : KC_MAX_PLAYERS;
		game_reset(c);
		c->state = KC_STATE_STARTING;
		if ((e = push_event(c, KC_EV_GAME_START)) != NULL)
		{
			const kc_game *g = game_find(c, c->room_game, NULL);
			copystr(e->game, c->room_rom[0] ? c->room_rom : g ? g->rom : "", KC_NAME_MAX);
			e->game_id = c->room_game;
			e->player = c->player;
			e->num_players = c->num_players;
			e->delay = c->delay;
		}
		break;

	case M_READY:
		if (c->state == KC_STATE_SYNC || c->state == KC_STATE_STARTING)
		{
			c->state = KC_STATE_PLAYING;
			push_event(c, KC_EV_GAME_PLAYING);
		}
		break;

	case M_GAME_DATA:
	{
		unsigned n;
		rd8(&r);
		n = rd16(&r);
		if (r.p + n > r.end || !n)
			break;
		if (cache_find(&c->in_cache, r.p, (int) n) < 0)
			cache_add(&c->in_cache, r.p, (int) n);
		if (c->state == KC_STATE_PLAYING || c->state == KC_STATE_SYNC)
			rxq_append(c, r.p, (int) n);
		break;
	}

	case M_GAME_CACHE:
	{
		int pos, n;
		const uint8_t *d;
		rd8(&r);
		pos = (int) rd8(&r);
		d = cache_get(&c->in_cache, pos, &n);
		if (d && (c->state == KC_STATE_PLAYING || c->state == KC_STATE_SYNC))
			rxq_append(c, d, n);
		break;
	}

	case M_DROP_GAME:
	{
		int pn;
		rd_str(&r, name, sizeof(name));
		pn = (int) rd8(&r);
		if ((e = push_event(c, KC_EV_PLAYER_DROPPED)) != NULL)
		{
			copystr(e->name, name, KC_NAME_MAX);
			e->player = pn;
		}
		if (!strcmp(name, c->user) && c->state >= KC_STATE_STARTING)
		{
			c->game_over = 1;
			c->state = KC_STATE_IN_GAME;
			push_event(c, KC_EV_GAME_ENDED);
		}
		break;
	}

	case M_REJECTED:
	{
		rd_str(&r, name, sizeof(name));
		rd16(&r);
		rd_str(&r, text, sizeof(text));
		if ((e = push_event(c, KC_EV_REJECTED)) != NULL)
			copystr(e->text, text, KC_TEXT_MAX);
		if (c->state <= KC_STATE_LOGIN)
		{
			kc_net_close(c->sock);
			c->sock = KC_BAD_SOCK;
			c->state = KC_STATE_IDLE;
		}
		break;
	}

	default:
		break;
	}
}

static int seq_after(unsigned a, unsigned b)	/* a is after b, mod 65536 */
{
	return (uint16_t) (a - b) != 0 && (uint16_t) (a - b) < 0x8000;
}

static void handle_packet(kc_client *c, const uint8_t *pkt, int len)
{
	const uint8_t *msg[64];
	int mlen[64], mtype[64];
	unsigned mseq[64];
	int count, i, n = 0, p = 1;

	if (len < 1)
		return;
	count = pkt[0];
	for (i = 0; i < count && n < 64; i++)
	{
		unsigned l;
		if (p + 5 > len)
			break;
		l = get16(pkt + p + 2);
		if (l < 1 || p + 4 + (int) l > len)
			break;
		mseq[n] = get16(pkt + p);
		mtype[n] = pkt[p + 4];
		msg[n] = pkt + p + 5;
		mlen[n] = (int) l - 1;
		n++;
		p += 4 + l;
	}
	/* Messages come newest first. Handle them strictly in order: on a gap (a lost packet), take
	   nothing newer and send our last packet again; a packet with no new messages makes the
	   server send its recent messages again. */
	if (c->rx_next < 0 && n)
	{
		c->rx_next = (int) mseq[n - 1];
		for (i = 0; i < n; i++)
			if (seq_after((unsigned) c->rx_next, mseq[i]))
				c->rx_next = (int) mseq[i];
	}
	for (;;)
	{
		int found = -1, newer = 0;
		for (i = 0; i < n; i++)
		{
			if (mseq[i] == (unsigned) c->rx_next)
				found = i;
			else if (seq_after(mseq[i], (unsigned) c->rx_next))
				newer = 1;
		}
		if (found < 0)
		{
			if (newer && kc_net_ms() - c->t_resend >= GAP_RESEND_MS)
				send_history(c);
			break;
		}
		c->rx_next = (mseq[found] + 1) & 0xFFFF;
		handle_message(c, mtype[found], msg[found], mlen[found]);
		if (c->sock == KC_BAD_SOCK)
			break;
	}
}

static void receive_all(kc_client *c)
{
	uint8_t buf[MAX_PACKET * 2];
	struct sockaddr_in from;
	int n;
	if (c->sock == KC_BAD_SOCK)
		return;
	while ((n = kc_net_recv(c->sock, buf, sizeof(buf) - 1, &from)) > 0)
	{
		if (from.sin_addr.s_addr != c->server.sin_addr.s_addr)
			continue;
		c->t_recv = kc_net_ms();
		if (c->state == KC_STATE_HELLO)
		{
			buf[n] = 0;
			if (!memcmp(buf, "HELLOD00D", 9))
			{
				c->server.sin_port = htons((uint16_t) atoi((char *) buf + 9));
				c->state = KC_STATE_LOGIN;
				c->t_state = kc_net_ms();
				send_login(c);
			}
			else if (!memcmp(buf, "TOO", 3))
			{
				kc_event *e = push_event(c, KC_EV_REJECTED);
				if (e)
					copystr(e->text, "Server is full", KC_TEXT_MAX);
				kc_net_close(c->sock);
				c->sock = KC_BAD_SOCK;
				c->state = KC_STATE_IDLE;
				return;
			}
			continue;
		}
		if (from.sin_port != c->server.sin_port)
			continue;
		handle_packet(c, buf, n);
		if (c->sock == KC_BAD_SOCK)
			return;
	}
}

static void timers(kc_client *c)
{
	uint32_t now = kc_net_ms();
	kc_event *e;
	switch (c->state)
	{
	case KC_STATE_HELLO:
		if (now - c->t_send >= HELLO_RETRY_MS)
		{
			if (++c->tries > HELLO_TRIES)
			{
				if ((e = push_event(c, KC_EV_DISCONNECTED)) != NULL)
					copystr(e->text, "No answer from the server", KC_TEXT_MAX);
				kc_net_close(c->sock);
				c->sock = KC_BAD_SOCK;
				c->state = KC_STATE_IDLE;
				break;
			}
			send_raw(c, "HELLO" KC_PROTOCOL_VERSION, sizeof("HELLO" KC_PROTOCOL_VERSION));
		}
		break;
	case KC_STATE_LOGIN:
		if (now - c->t_state >= LOGIN_TIMEOUT_MS)
		{
			if ((e = push_event(c, KC_EV_DISCONNECTED)) != NULL)
				copystr(e->text, "Login timed out", KC_TEXT_MAX);
			kc_net_close(c->sock);
			c->sock = KC_BAD_SOCK;
			c->state = KC_STATE_IDLE;
		}
		else if (now - c->t_send >= LOGIN_RETRY_MS)
			send_history(c);
		break;
	case KC_STATE_PLAYING:
		break;					/* kc_game_frame resends */
	case KC_STATE_IDLE:
		break;
	default:
		if (now - c->t_new >= KEEPALIVE_MS)
		{
			writer w = { {0}, 0 };
			wr8(&w, 0);
			send_msg(c, M_KEEPALIVE, &w);
		}
		else if (now - c->t_resend >= POKE_MS && c->tx_hist_n)
			send_history(c);
		break;
	}
}

/* ---- the public API ---- */

kc_client *kc_create(void)
{
	kc_client *c = (kc_client *) calloc(1, sizeof(kc_client));
	if (!c)
		return NULL;
	c->sock = KC_BAD_SOCK;
	c->rx_next = -1;
	return c;
}

static void forget_server(kc_client *c)
{
	free(c->users);
	free(c->games);
	c->users = NULL;
	c->games = NULL;
	c->n_users = c->cap_users = c->n_games = c->cap_games = 0;
	c->n_room = 0;
	c->room_game = 0;
	c->room_owner = 0;
	game_reset(c);
}

void kc_destroy(kc_client *c)
{
	if (!c)
		return;
	kc_net_close(c->sock);
	forget_server(c);
	free(c->ev);
	free(c);
}

int kc_connect(kc_client *c, const char *host, int port, const char *user, const char *emulator, int conn_type)
{
	if (c->sock != KC_BAD_SOCK)
		kc_net_close(c->sock);
	forget_server(c);
	c->sock = KC_BAD_SOCK;
	c->state = KC_STATE_IDLE;
	if (kc_net_resolve(host, port > 0 ? port : KC_DEFAULT_PORT, &c->server) != 0)
		return -1;
	c->sock = kc_net_open();
	if (c->sock == KC_BAD_SOCK)
		return -1;
	copystr(c->user, user, KC_NAME_MAX);
	copystr(c->emulator, emulator, KC_NAME_MAX);
	c->conn = (conn_type >= KC_CONN_LAN && conn_type <= KC_CONN_BAD) ? conn_type : KC_CONN_GOOD;
	c->tx_seq = 0;
	c->tx_hist_n = 0;
	c->rx_next = -1;
	c->my_id = -1;
	c->tries = 1;
	c->state = KC_STATE_HELLO;
	c->t_state = kc_net_ms();
	send_raw(c, "HELLO" KC_PROTOCOL_VERSION, sizeof("HELLO" KC_PROTOCOL_VERSION));
	return 0;
}

void kc_disconnect(kc_client *c, const char *message)
{
	kc_event *e;
	if (c->state >= KC_STATE_LOGIN && c->sock != KC_BAD_SOCK)
	{
		writer w = { {0}, 0 };
		wr8(&w, 0);
		wr16(&w, 0xFFFF);
		wrstr(&w, message ? message : "");
		send_msg(c, M_USER_QUIT, &w);
	}
	kc_net_close(c->sock);
	c->sock = KC_BAD_SOCK;
	if (c->state != KC_STATE_IDLE && (e = push_event(c, KC_EV_DISCONNECTED)) != NULL)
		copystr(e->text, message ? message : "", KC_TEXT_MAX);
	c->state = KC_STATE_IDLE;
	forget_server(c);
}

int kc_poll(kc_client *c, int timeout_ms, kc_event *ev)
{
	uint32_t start = kc_net_ms();
	for (;;)
	{
		receive_all(c);
		timers(c);
		if (pop_event(c, ev))
			return 1;
		if (c->sock == KC_BAD_SOCK)
			return 0;
		{
			int left = timeout_ms - (int) (kc_net_ms() - start);
			if (left <= 0)
				return 0;
			kc_net_wait(c->sock, left > 250 ? 250 : left);
		}
	}
}

intptr_t kc_socket(kc_client *c) { return c->sock == KC_BAD_SOCK ? -1 : (intptr_t) c->sock; }
int kc_state(kc_client *c) { return c->state; }
int kc_user_count(kc_client *c) { return c->n_users; }
const kc_user *kc_user_at(kc_client *c, int i) { return (i >= 0 && i < c->n_users) ? &c->users[i] : NULL; }
int kc_game_count(kc_client *c) { return c->n_games; }
const kc_game *kc_game_at(kc_client *c, int i) { return (i >= 0 && i < c->n_games) ? &c->games[i] : NULL; }
const kc_game *kc_game_find(kc_client *c, uint32_t id) { return game_find(c, id, NULL); }
int kc_room_count(kc_client *c) { return c->n_room; }
const kc_user *kc_room_player(kc_client *c, int i) { return (i >= 0 && i < c->n_room) ? &c->room[i] : NULL; }
uint32_t kc_room_game(kc_client *c) { return c->room_game; }
int kc_room_owner(kc_client *c) { return c->room_owner == 1; }
int kc_my_id(kc_client *c) { return c->my_id; }
const char *kc_my_name(kc_client *c) { return c->user; }

int kc_chat(kc_client *c, const char *text)
{
	writer w = { {0}, 0 };
	if (c->state < KC_STATE_LOBBY)
		return -1;
	wr8(&w, 0);
	wrstr(&w, text);
	send_msg(c, M_CHAT, &w);
	return 0;
}

int kc_game_chat(kc_client *c, const char *text)
{
	writer w = { {0}, 0 };
	if (c->state < KC_STATE_IN_GAME)
		return -1;
	wr8(&w, 0);
	wrstr(&w, text);
	send_msg(c, M_GAME_CHAT, &w);
	return 0;
}

int kc_create_game(kc_client *c, const char *rom)
{
	writer w = { {0}, 0 };
	if (c->state != KC_STATE_LOBBY)
		return -1;
	wr8(&w, 0);
	wrstr(&w, rom);
	wr8(&w, 0);
	wr32(&w, 0xFFFFFFFFu);
	c->room_owner = 2;			/* waiting for our "game created" */
	copystr(c->room_rom, rom, KC_NAME_MAX);
	send_msg(c, M_CREATE_GAME, &w);
	return 0;
}

int kc_join_game(kc_client *c, uint32_t game_id)
{
	writer w = { {0}, 0 };
	const kc_game *g = game_find(c, game_id, NULL);
	if (c->state != KC_STATE_LOBBY)
		return -1;
	wr8(&w, 0);
	wr32(&w, game_id);
	wr8(&w, 0);
	wr32(&w, 0);
	wr16(&w, 0xFFFF);
	wr8(&w, c->conn);
	c->room_game = game_id;
	c->room_owner = 0;
	copystr(c->room_rom, g ? g->rom : "", KC_NAME_MAX);
	send_msg(c, M_JOIN_GAME, &w);
	return 0;
}

int kc_quit_game(kc_client *c)
{
	writer w = { {0}, 0 };
	if (c->state < KC_STATE_IN_GAME)
		return -1;
	if (c->state >= KC_STATE_STARTING)
		kc_game_drop(c);
	wr8(&w, 0);
	wr16(&w, 0xFFFF);
	send_msg(c, M_QUIT_GAME, &w);
	return 0;
}

int kc_start_game(kc_client *c)
{
	writer w = { {0}, 0 };
	if (c->state != KC_STATE_IN_GAME)
		return -1;
	wr8(&w, 0);
	wr16(&w, 0xFFFF);
	wr8(&w, 0xFF);
	wr8(&w, 0xFF);
	send_msg(c, M_START_GAME, &w);
	return 0;
}

int kc_kick(kc_client *c, int user_id)
{
	writer w = { {0}, 0 };
	if (c->state != KC_STATE_IN_GAME)
		return -1;
	wr8(&w, 0);
	wr16(&w, (unsigned) user_id);
	send_msg(c, M_KICK, &w);
	return 0;
}

int kc_game_ready(kc_client *c)
{
	writer w = { {0}, 0 };
	if (c->state != KC_STATE_STARTING)
		return -1;
	wr8(&w, 0);
	send_msg(c, M_READY, &w);
	c->state = KC_STATE_SYNC;
	return 0;
}

int kc_game_drop(kc_client *c)
{
	writer w = { {0}, 0 };
	if (c->state < KC_STATE_STARTING)
		return -1;
	wr8(&w, 0);
	wr8(&w, 0);
	send_msg(c, M_DROP_GAME, &w);
	c->state = KC_STATE_IN_GAME;
	c->game_over = 1;
	return 0;
}

/* Sends our inputs of one packet (conn frames) as game data, or as a cache position. */
static void send_inputs(kc_client *c, const uint8_t *d, int len, int send_now)
{
	writer w = { {0}, 0 };
	int pos = cache_find(&c->out_cache, d, len);
	wr8(&w, 0);
	if (pos >= 0)
	{
		wr8(&w, (unsigned) pos);
		queue_msg(c, M_GAME_CACHE, &w);
	}
	else
	{
		wr16(&w, (unsigned) len);
		wrbytes(&w, d, len);
		cache_add(&c->out_cache, d, len);
		queue_msg(c, M_GAME_DATA, &w);
	}
	if (send_now)
		send_history(c);
}

int kc_game_frame(kc_client *c, const void *in, int size, void *out, int timeout_ms)
{
	uint32_t start = kc_net_ms();
	int frame_bytes;

	if (size <= 0 || size > 256)
		return -1;
	/* still synchronising: wait for "all ready" */
	while (c->state == KC_STATE_SYNC || c->state == KC_STATE_STARTING)
	{
		kc_event dummy;
		int left = timeout_ms - (int) (kc_net_ms() - start);
		if (c->state == KC_STATE_STARTING)
			kc_game_ready(c);
		receive_all(c);
		timers(c);
		if (c->state != KC_STATE_SYNC)
			break;
		if (left <= 0)
			return 0;
		kc_net_wait(c->sock, left > 50 ? 50 : left);
		(void) dummy;
	}
	if (c->state != KC_STATE_PLAYING || c->game_over || c->sock == KC_BAD_SOCK)
		return -1;

	if (!c->in_size)
	{
		c->in_size = size;
		c->acc = (uint8_t *) calloc((size_t) c->conn, (size_t) size);
		if (!c->acc)
			return -1;
	}
	if (size != c->in_size)
		return -1;
	frame_bytes = size * c->num_players;

	if (!c->frame_pending)
	{
		memcpy(c->acc + c->acc_frames * size, in, size);
		if (++c->acc_frames == c->conn)
		{
			send_inputs(c, c->acc, c->conn * size, 1);
			c->acc_frames = 0;
		}
		c->frame_pending = 1;
	}

	/* The delay: the first conn * (delay + 1) frames have no input from anyone; the answers to
	   our first packets are used after them. The server sends each player as many frames of no
	   input before the real ones as makes every player's stream the same (EmuLinker:
	   highest delay + (highest - own) + 5 packets): the delay it gave us is part of that sum. */
	if (c->frame < c->conn * (c->delay + 1))
	{
		receive_all(c);
		timers(c);
		memset(out, 0, frame_bytes);
		c->frame++;
		c->frame_pending = 0;
		return frame_bytes;
	}

	for (;;)
	{
		int left;
		receive_all(c);
		timers(c);
		if (c->state != KC_STATE_PLAYING || c->game_over || c->sock == KC_BAD_SOCK)
			return -1;
		if (c->rxq_len >= frame_bytes)
		{
			memcpy(out, c->rxq, frame_bytes);
			memmove(c->rxq, c->rxq + frame_bytes, c->rxq_len - frame_bytes);
			c->rxq_len -= frame_bytes;
			c->frame_pending = 0;
			c->frame++;
			return frame_bytes;
		}
		/* waiting: if nothing came back for a while, our last packet may be lost */
		if (kc_net_ms() - c->t_recv >= GAME_RESEND_MS && kc_net_ms() - c->t_resend >= GAME_RESEND_MS)
			send_history(c);
		left = timeout_ms - (int) (kc_net_ms() - start);
		if (left <= 0)
			return 0;
		kc_net_wait(c->sock, left > 20 ? 20 : left);
	}
}

int kc_ping_server(const char *host, int port, int timeout_ms)
{
	struct sockaddr_in a, from;
	kc_sock s;
	uint32_t t0;
	uint8_t buf[64];
	int result = -1;
	if (kc_net_resolve(host, port > 0 ? port : KC_DEFAULT_PORT, &a) != 0)
		return -1;
	s = kc_net_open();
	if (s == KC_BAD_SOCK)
		return -1;
	t0 = kc_net_ms();
	kc_net_send(s, &a, "PING", 5);
	while ((int) (kc_net_ms() - t0) < timeout_ms)
	{
		int n;
		kc_net_wait(s, 50);
		n = kc_net_recv(s, buf, sizeof(buf), &from);
		if (n >= 4 && !memcmp(buf, "PONG", 4))
		{
			result = (int) (kc_net_ms() - t0);
			break;
		}
	}
	kc_net_close(s);
	return result;
}

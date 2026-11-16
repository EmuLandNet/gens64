/* gens64: Paprium (WaterMelon, T-574120-00): high-level emulation of the cartridge's processor
   (docs/paprium.md).

   Ported from "Project Little Man" (core/cart_hw/paprium.h of a Genesis Plus GX fork, 2025): the
   commands the game sends to the chip (word written to 1FEA, arguments at 1E10...) are carried out
   here on the chip's 64 KB RAM, which replaces 000000-00FFFF. Kept as in that code: the ROM and
   RAM patches, the sprite engine, the decoders, the scaler, the sound effects, the SRAM banks.
   Changed for Gens: no randomness (reads of 1880-1AFF and the echo side come from a generator kept
   in the savestate), a fixed number of 48 kHz sound samples per frame, the music tracks (MP3 files)
   do not change anything the game can see, the whole state is in the savestate.

   All of it is reached only through paprium.h with Rom_Mapper == 3 (Mem_M68k_asm.c). */

#include <string.h>
#include <stdint.h>

#include "gens64_addr.h"
#include "Star_68k.h"
#include "paprium.h"

/* -DPAPRIUM_TRACE: commands to paprium_trace.txt (debugging) */
#ifdef PAPRIUM_TRACE
#include <stdio.h>
unsigned int main68k_readPC(void);
static FILE *trace_f;
static unsigned trace_frame;
static unsigned dma_n, dma_lo = ~0u, dma_hi, run_start = ~0u, run_last, run_len;
static char runs[4096]; static int runs_len;
#define TRACE(...) do { if (!trace_f) trace_f = fopen("paprium_trace.txt", "w"); if (trace_f) { fprintf(trace_f, __VA_ARGS__); fflush(trace_f); } } while (0)
#else
#define TRACE(...) ((void)0)
#endif

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t int8;
typedef unsigned int uint;

extern unsigned char Rom_Data[];
extern unsigned int Rom_Size, Rom_Mask;
extern unsigned char SRAM[64 * 1024];

#define PAPRIUM_BOSS1 0x17
#define PAPRIUM_BOSS2 0x21
#define PAPRIUM_BOSS3 0x22
#define PAPRIUM_BOSS4 0x23

static uint8 paprium_obj_ram[0x80000 + 0x20000];	/* sprite data, decoded from the ROM (not saved) */

/* the rest of the chip's state (saved) */
static struct
{
	int music_ptr, wave_ptr, sfx_ptr, tile_ptr, sprite_ptr;
	int tmss;		/* A14101 */
	uint32 rng;		/* gens64: replaces rand() */
	uint32 sample_acc;	/* gens64: 48 kHz samples per frame, remainder */
	int track_last;
} pap;

static uint32 paprium_rand(void)
{
	/* xorshift32 */
	uint32 x = pap.rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return pap.rng = x;
}

#define paprium_music_ptr pap.music_ptr
#define paprium_wave_ptr pap.wave_ptr
#define paprium_sfx_ptr pap.sfx_ptr
#define paprium_tile_ptr pap.tile_ptr
#define paprium_sprite_ptr pap.sprite_ptr

/* sound of the last frame at 48 kHz (step 2 mixes it) */
#define PAPRIUM_MAX_SAMPLES 1024
static int paprium_out[2][PAPRIUM_MAX_SAMPLES];
static int paprium_out_count;

typedef struct paprium_voice_t
{
	int volume;
	int panning;
	int flags;
	int type;

	int size;
	int ptr;
	int tick;

	int loop;
	int echo;
	int program;

	int count;

	int time;

	int start;
	int num;

	int decay;
	int decay2;
	int release;
	int sustain;

	int duration;
	int velocity;
	int keyon;
	int key_type;
	int pitch;
	int cents;
	int modulation;
} paprium_voice_t;

struct paprium_t
{
	uint8 ram[0x10000];
	uint8 decoder_ram[0x10000];
	uint8 scaler_ram[0x1000];
	uint8 music_ram[0x8000];
	uint8 exps_ram[14*8];

	paprium_voice_t sfx[8];
	paprium_voice_t music[26];

	int music_section, audio_tick, music_segment;

	int out_l, out_r;
	int audio_flags, sfx_volume, music_volume;

	int decoder_mode, decoder_ptr, decoder_size;
	int draw_src, draw_dst;
	int obj[0x31];

	int echo_l[48000/4], echo_r[48000/4];
	int echo_ptr, echo_pan;

	int music_track, mp3_ptr, music_tick;
};

/* gens64: room after the state for the overruns the HLE allows (tile copies, decoder output) */
static struct { struct paprium_t s; uint8 overrun[0x20000]; } paprium_box;
#define paprium_s (paprium_box.s)

static const uint8 paprium_volume_table[256] =
{
    0x00, 0x03, 0x07, 0x0B, 0x0E, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21, 0x24, 0x27, 0x29, 0x2C, 0x2F, 
    0x31, 0x34, 0x36, 0x38, 0x3B, 0x3D, 0x3F, 0x41, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E, 0x50, 0x51, 
    0x53, 0x55, 0x57, 0x59, 0x5A, 0x5C, 0x5E, 0x5F, 0x61, 0x63, 0x64, 0x66, 0x67, 0x69, 0x6A, 0x6C, 
    0x6D, 0x6F, 0x70, 0x72, 0x73, 0x74, 0x76, 0x77, 0x78, 0x7A, 0x7B, 0x7C, 0x7E, 0x7F, 0x80, 0x81, 
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 
    0xA4, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xAF, 0xB0, 
    0xB1, 0xB2, 0xB3, 0xB3, 0xB4, 0xB5, 0xB6, 0xB6, 0xB7, 0xB8, 0xB9, 0xB9, 0xBA, 0xBB, 0xBC, 0xBC, 
    0xBD, 0xBE, 0xBE, 0xBF, 0xC0, 0xC1, 0xC1, 0xC2, 0xC3, 0xC3, 0xC4, 0xC5, 0xC5, 0xC6, 0xC7, 0xC7, 
    0xC8, 0xC9, 0xC9, 0xCA, 0xCA, 0xCB, 0xCC, 0xCC, 0xCD, 0xCE, 0xCE, 0xCF, 0xCF, 0xD0, 0xD1, 0xD1, 
    0xD2, 0xD2, 0xD3, 0xD3, 0xD4, 0xD5, 0xD5, 0xD6, 0xD6, 0xD7, 0xD7, 0xD8, 0xD9, 0xD9, 0xDA, 0xDA, 
    0xDB, 0xDB, 0xDC, 0xDC, 0xDD, 0xDD, 0xDE, 0xDF, 0xDF, 0xE0, 0xE0, 0xE1, 0xE1, 0xE2, 0xE2, 0xE3, 
    0xE3, 0xE4, 0xE4, 0xE5, 0xE5, 0xE6, 0xE6, 0xE7, 0xE7, 0xE8, 0xE8, 0xE9, 0xE9, 0xEA, 0xEA, 0xEA, 
    0xEB, 0xEB, 0xEC, 0xEC, 0xED, 0xED, 0xEE, 0xEE, 0xEF, 0xEF, 0xF0, 0xF0, 0xF0, 0xF1, 0xF1, 0xF2, 
    0xF2, 0xF3, 0xF3, 0xF4, 0xF4, 0xF4, 0xF5, 0xF5, 0xF6, 0xF6, 0xF7, 0xF7, 0xF7, 0xF8, 0xF8, 0xF9, 
    0xF9, 0xF9, 0xFA, 0xFA, 0xFB, 0xFB, 0xFC, 0xFC, 0xFC, 0xFD, 0xFD, 0xFE, 0xFE, 0xFE, 0xFF, 0xFF, 
};


/* music tracks: command 8C argument -> file in the "paprium" folder next to the ROM */
static const char *paprium_track_name(int track)
{
	switch (track)
	{
	case 0x01: return "02 90's Acid Dub Character Select.mp3";
	case 0x02: return "08 90's Dance.mp3";
	case 0x03: return "42 1988 Commercial.mp3";
	case 0x04: return "05 Asian Chill.mp3";
	case 0x05: return "31 Bad Dudes vs Paprium.mp3";
	case 0x06: return "43 Blade FM.mp3";
	case 0x07: return "03 Bone Crusher.mp3";
	case 0x0B: return "26 Club Shuffle.mp3";
	case 0x0C: return "23 Continue.mp3";  /* Continue Fast */
	case 0x0E: return "07 Cool Groove.mp3";
	case 0x0F: return "36 Cyberpunk Ninja.mp3";
	case 0x10: return "35 Cyberpunk Funk.mp3";
	case 0x11: return "30 Cyber Interlude.mp3";
	case 0x12: return "21 Cyborg Invasion.mp3";
	case 0x13: return "44 Dark Alley.mp3";
	case 0x14: return "29 Dark & Power Mad.mp3";
	case 0x15: return "24 Intro.mp3";  /* Dark Bounce */
	case 0x16: return "27 Dark Rock.mp3";
	case 0x17: return "04 Drumbass Boss.mp3";
	case 0x18: return "45 Dubstep Groove.mp3";
	case 0x19: return "15 Electro Acid Funk.mp3";
	case 0x1B: return "28 Evolve.mp3";
	case 0x1C: return "33 Funk Enhanced Mix.mp3";
	case 0x1D: return "41 Game Over.mp3";
	case 0x1E: return "46 Gothic.mp3";
	case 0x20: return "13 Hard Rock.mp3";
	case 0x21: return "22 Hardcore BP1.mp3";
	case 0x22: return "11 Hardcore BP2.mp3";
	case 0x23: return "38 Hardcore BP3.mp3";
	case 0x24: return "40 Score.mp3";  /* High Score */
	case 0x25: return "47 House.mp3";
	case 0x26: return "17 Indie Shuffle.mp3";
	case 0x27: return "25 Indie Break Beat.mp3";
	case 0x28: return "16 Jazzy Shuffle.mp3";
	case 0x2A: return "19 Neo Metal.mp3";
	case 0x2B: return "14 Neon Rider.mp3";
	case 0x2E: return "09 Retro Beat.mp3";
	case 0x2F: return "20 Sadness.mp3";
	case 0x31: return "18 Slow Asian Beat.mp3";
	case 0x32: return "48 Slow Mood.mp3";  /* Slow Mood Ext. ? */
	case 0x33: return "49 Smooth Coords.mp3";
	case 0x34: return "10 Spiral.mp3";
	case 0x35: return "12 Stage Clear.mp3";
	case 0x36: return "32 Summer Breeze.mp3";
	case 0x37: return "06 Techno Beats.mp3";
	case 0x38: return "50 Tension.mp3";
	case 0x39: return "01 Theme of Paprium.mp3";
	case 0x3A: return "39 Ending.mp3";  /* Tough Guy */
	case 0x3B: return "34 Transe.mp3";
	case 0x3C: return "37 Urban.mp3";
	case 0x3D: return "51 Water.mp3";
	case 0x3E: return "52 Waterfront Beat.mp3";
	default: return 0;
	}
}

static void paprium_set_track(int track, int reload)
{
	/* gens64: the track number does not depend on the MP3 files being there */
	paprium_s.music_track = paprium_track_name(track) ? track : 0;
	if (reload)
		paprium_s.mp3_ptr = 0;
	paprium_s.music_tick = 0;
}

static void paprium_music_sheet()
{
	int ch;
	int l = 0, r = 0;

	/* 00-04 = WMMM */
	/* 05, 06, 07 */
	int sections = paprium_s.music_ram[0x09];
	/* 09 */
	int bars = (paprium_s.music_ram[0x0B] ? paprium_s.music_ram[0x0B] : 0x100) + 8;
	/* 0B */
	int cmds = paprium_s.music_ram[0x0D];
	/* 10-29 */
	/* 2A-43 */
	/* 44-5D */
	/* 5E-77 */
	/* 78-97 = title ^ 0xA5 */
	/* 98-B7 = author ^ 0xA5 */
	/* B8-D7 = comment ^ 0xA5 */

	if( paprium_s.music_segment == -1 ) {
		paprium_s.music_track = 0;
		return;
	}

	if( paprium_s.audio_tick < 4 )
		return;
	paprium_s.audio_tick = 0;

	for( ch = 0; ch < 26; ch++ ) {
		int ptr = *(uint16*)(paprium_s.music_ram + 0xD8 + (ch*sections*2) + (paprium_s.music_segment*2));
		int index = paprium_s.music_ram[(ptr + paprium_s.music_section)^1];
		int code = 0, arg = 0;
		int keyon = 0, duration = 0, freq = -1, volume = 0;
		paprium_voice_t *voice = paprium_s.music + ch;

		if( index == 0 )
			goto next;
		ptr += bars + (index-1) * 8;

		for( int lcv = 0; lcv < cmds; lcv++ ) {
			code = *(uint16*)(paprium_s.music_ram + ptr + lcv*2);
			arg = code & 0xFF;
			code >>= 8;

			if( code == 0x00 )
				{} //break;

			else if( code == 0x01 ) {  /* ?? */
				if( ch >= 11-1 ) keyon = code;
				voice->volume = 255 - paprium_volume_table[arg];  /* z80 table ? */
			}

			else if( code == 0x02 )
				voice->panning = arg;

			else if( code == 0x03 ) {
				voice->volume = 255 - paprium_volume_table[arg];  /* z80 table ? */
			}

			else if( code == 0x05 ) {
				voice->volume = 255 - paprium_volume_table[arg];  /* z80 table ? */
			}

			/* velocity */

			else if( code == 0x08 ) {

				freq = arg;
			}

			else if( code == 0x0A ) {
				freq = 0;  /* faster ? */
			}

			else if( code == 0x0B ) {
				//MessageBoxA(0, error_str, "---", 0);
			}

			else if( code == 0x0C ) {
				//MessageBoxA(0, error_str, "---", 0);
			}

			else if( code == 0x0E ) {  /* stop ? */
				keyon = 0;
				voice->size = 0;
			}

			else if( code == 0x0F ) {  /* program */
				voice->program = arg;

				if( ch < 6 ) {  /* YM */
				}

				else if( ch < 10 ) {  /* PSG */
				}

				else
				{  /* wave */
				}

			}
		}

		if( keyon ) {
		}

		if( freq >= 0 ) {
			voice->type = freq;
			//if(ch == 16-1) MessageBoxA(0,"hit","-",0);
		}

next:
		*(uint16*)(paprium_s.ram + ch*4 + 0x1B98) = index ? 0xE0 : 0;  /* L */
		*(uint16*)(paprium_s.ram + ch*4 + 0x1B9A) = index ? 0xE0 : 0;  /* R */
	}

end:
	if( (++paprium_s.music_section) >= bars ) {
		paprium_s.music_section = 0;

		if( (++paprium_s.music_segment) >= sections )
			paprium_s.music_segment = 0;
	}
}

static void paprium_sfx_voice(int *out_l, int *out_r)
{
	int ch;
	int l = 0, r = 0;

	for( ch = 0; ch < 8; ch++ ) {
		paprium_voice_t *voice = paprium_s.sfx + ch;
		const int _rates[] = {1,2,4,5,8,9};  /* 48000, 24000, 12000, 9600, 6000, 5333 */

		int rate = _rates[voice->type >> 4] << 16;  /* 16.16 */
		int depth = voice->type & 0x03;
		/* voice->type & 0xC0; */

		int vol = voice->volume;
		int pan = voice->panning;

		int sample, sample_l, sample_r;

		if( voice->size == 0 ) goto next;

		sample = *(uint8 *)(Rom_Data + paprium_sfx_ptr + (voice->ptr^1));

		if( depth == 1 )
			sample = (((sample & 0xFF) * 65536) / 256) - 32768;

		else if( depth == 2 ) {
			if( voice->count == 0 )
				sample >>= 4;

			sample = (((sample & 0x0F) * 65536) / 16) - 32768;
		}

		sample = (sample * vol) / 0x400;

		sample_l = (sample * ((pan <= 0x80) ? 0x80 : 0x100 - pan)) / 0x80;
		sample_r = (sample * ((pan >= 0x80) ? 0x80 : pan)) / 0x80;

		l += sample_l;
		r += sample_r;

		if( voice->flags & 0x4000 ) {  /* echo */
			if( voice->echo & 1 )
				paprium_s.echo_l[paprium_s.echo_ptr % (48000/6)] += (sample_l * 33) / 100;
			else
				paprium_s.echo_r[paprium_s.echo_ptr % (48000/6)] += (sample_r * 33) / 100;
		}

		if( voice->flags & 0x100 ) {  /* amplify */
			l = (l * 125) / 100;
			r = (r * 125) / 100;
		}

		voice->time++;

		voice->tick += 0x10000;
		voice->tick -= (voice->flags & 0x8000) ? 0x800 : 0;  /* tiny pitch */
		voice->tick -= (voice->flags & 0x2000) ? 0x8000 : 0;  /* huge pitch */

		if( voice->tick >= rate ) {
			voice->tick -= rate;

			voice->count++;
			voice->size--;

			if( voice->count >= depth ) {
				voice->ptr++;

				voice->count = 0;
			}
		}
 

next:
		if( voice->size == 0 ) {
			voice->count = 0;

			if( voice->loop ) {
				uint8 *sfx = paprium_sfx_ptr + Rom_Data;

				voice->ptr = (*(uint16 *)(sfx + voice->num*8) << 16) | (*(uint16 *)(sfx + voice->num*8 + 2));
				voice->size = (*(uint8 *)(sfx + voice->num*8 + 4) << 16) | (*(uint16 *)(sfx + voice->num*8 + 6));
			}
		}
	}

	*out_l += l;
	*out_r += r;
}

static void paprium_decoder_lz_rle(uint src, uint8 *dst)
{
	int size = 0;
	int len, lz, rle, code;

	while(1) {
		int type = Rom_Data[(src++)^1];

		code = type >> 6;
		len = type & 0x3F;

		if( (code == 0) && (len == 0))
			break;

		else if( code == 1 )
			rle = Rom_Data[(src++)^1];

		else if( code == 2 )
			lz = size - Rom_Data[(src++)^1];

		while( len-- > 0 ) {
			switch(code) {
			case 0: dst[(size++)^1] = Rom_Data[(src++)^1]; break;
			case 1: dst[(size++)^1] = rle; break;
			case 2: dst[(size++)^1] = dst[(lz++)^1]; break;
			case 3: dst[(size++)^1] = 0; break;
			}
		}
	}

	paprium_s.decoder_size = size;
}

static void paprium_decoder_lzo(uint src, uint8 *dst)
{
	int size = 0;
	int len, lz, raw;
	int state = 0;

	while(1) {
		int code = Rom_Data[(src++)^1];

		if( code & 0x80 ) goto code_80;
		if( code & 0x40 ) goto code_40;
		if( code & 0x20 ) goto code_20;
		if( code & 0x10 ) goto code_10;

code_00:
		if( state == 0 ) {
			raw = code & 0x0F;

			if( raw == 0 ) {
				int extra = 0;

				while(1) {
					raw = Rom_Data[(src++)^1];
					if( raw ) break;

					extra += 255;
				}

				raw += extra;
				raw += 15;
			}
			raw += 3;

			len = 0;
			state = 4;
			goto copy_loop;
		}

		else if( state < 4 ) {
			raw = code & 0x03;
			lz = (code >> 2) & 0x03;

			lz += Rom_Data[(src++)^1] << 2;
			lz += 1;

			len = 2;
			goto copy_loop;
		}

		else {
			raw = code & 0x03;
			lz = (code >> 2) & 0x03;

			lz += Rom_Data[(src++)^1] << 2;
			lz += 2049;

			len = 3;
			goto copy_loop;
		}

code_10:
		len = code & 0x07;

		if( len == 0 ) {
			int extra = 0;

			while(1) {
				len = Rom_Data[(src++)^1];
				if( len ) break;

				extra += 255;
			}

			len += extra;
			len += 7;
		}
		len += 2;

		lz = ((code >> 3) & 1) << 14;

		code = Rom_Data[(src++)^1];
		raw = code & 0x03;
		lz += code >> 2;

		lz += Rom_Data[(src++)^1] << 6;
		lz += 16384;

		if( lz == 16384 ) break;
		goto copy_loop;

code_20:
		len = code & 0x1F;

		if( len == 0 ) {
			int extra = 0;

			while(1) {
				len = Rom_Data[(src++)^1];
				if( len ) break;

				extra += 255;
			}

			len += extra;
			len += 31;
		}
		len += 2;

		code = Rom_Data[(src++)^1];
		raw = code & 0x03;

		lz = code >> 2;
		lz += Rom_Data[(src++)^1] << 6;
		lz += 1;

		goto copy_loop;

code_40:
		raw = code & 0x03;
		len = ((code >> 5) & 1) + 3;
		lz = (code >> 2) & 0x07;

		lz += Rom_Data[(src++)^1] << 3;
		lz += 1;

		goto copy_loop;

code_80:
		raw = code & 0x03;
		len = ((code >> 5) & 0x03) + 5;
		lz = (code >> 2) & 0x07;

		lz += Rom_Data[(src++)^1] << 3;
		lz += 1;

copy_loop:
		if( len > 0 )
			state = raw;
		else
			state = 4;

		
		lz = size - lz;

		while(1) {
			if( len > 0 ) {
				dst[(size++)^1] = dst[(lz++)^1];
				len--;
			}

			else if( raw > 0 ) {
				dst[(size++)^1] = Rom_Data[(src++)^1];
				raw--;
			}

			else
				break;
		}		
	}

	paprium_s.decoder_size = size;
}

static void paprium_decoder_type(int src, uint8 *dst)
{
	int type = Rom_Data[(src++)^1];

	if( type == 0x80 )
		paprium_decoder_lz_rle(src, dst);

	else if( type == 0x81 )
		paprium_decoder_lzo(src, dst);

	else {
	}
}

static void paprium_decoder(int mode)
{

	int offset = *(uint16 *)(paprium_s.ram + 0x1E10);
	int ptr = (*(uint16 *)(paprium_s.ram + 0x1E12) << 16) + *(uint16 *)(paprium_s.ram + 0x1E14);

	paprium_decoder_type(ptr, paprium_s.decoder_ram + offset);

	paprium_s.decoder_mode = mode;
	paprium_s.decoder_ptr = offset;

}

static void paprium_decoder_copy(int arg)
{
	/* *(uint16 *)(paprium_s.ram + 0x1E10); */
	int offset = *(uint16 *)(paprium_s.ram + 0x1E12);
	int size = *(uint16 *)(paprium_s.ram + 0x1E14);

	paprium_s.decoder_ptr = offset;
	paprium_s.decoder_size = size;

}

static void paprium_sprite(int index)
{
	int lcv, spr_x, spr_y, count;
	int spritePtr, gfxPtr;

	int dmaSize = 0;
	int dmaPtr = *(uint16*) (paprium_s.ram + 0x1F16);

	int spriteCount = *(uint16*) (paprium_s.ram + 0x1F18);

	int anim = *(uint16*) (paprium_s.ram + 0xF80 + index*16);
	int nextAnim = *(uint16*) (paprium_s.ram + 0xF82 + index*16);
	int obj = *(uint16*) (paprium_s.ram + 0xF84 + index*16) & 0x7FFF;
	//*(uint16*) (paprium_s.ram + 0xF86 + index*16);
	int objAttr = *(uint16*) (paprium_s.ram + 0xF88 + index*16);
	int reset = *(uint16*) (paprium_s.ram + 0xF8A + index*16);
	int pos_x = *(uint16*) (paprium_s.ram + 0xF8C + index*16);
	int pos_y = *(uint16*) (paprium_s.ram + 0xF8E + index*16);

	int src = paprium_s.draw_src;
	int vram = paprium_s.draw_dst;
	int flip_h = objAttr & 0x800;
	int animFlags;

	int animPtr = *(uint32*) (paprium_obj_ram + (obj+1)*4);
	int framePtr = paprium_s.obj[index];

	if( index != 0x30 ) {
		//return;
	}

reload:
	if( reset == 1 ) {
		framePtr = *(uint32*) (paprium_obj_ram + animPtr + anim*4);

		*(uint16*) (paprium_s.ram + 0xF8A + index*16) = 0;
	}
	if( (framePtr == 0) || (framePtr == -1) ) {
		*(uint16*) (paprium_s.ram + 0xF80 + index*16) = 0;
		return;
	}

	spritePtr  = paprium_obj_ram[framePtr + 0] << 0;
	spritePtr += paprium_obj_ram[framePtr + 1] << 8;
	spritePtr += paprium_obj_ram[framePtr + 2] << 16;
	animFlags = paprium_obj_ram[framePtr + 3];

	if( spritePtr == 0 ) return;

	framePtr += 4; 

	if( animFlags < 0x80 ) {
		if( nextAnim == 0xFFFF ) {
			int nextFrame;

			nextFrame  = paprium_obj_ram[framePtr + 0] << 0;
			nextFrame += paprium_obj_ram[framePtr + 1] << 8;
			nextFrame += paprium_obj_ram[framePtr + 2] << 16;

			framePtr = nextFrame;
		}
		else if( reset == 0 ) {
			anim = nextAnim;
			reset = 1;

			goto reload;
		}
	}

	paprium_s.obj[index] = framePtr;

	count = paprium_obj_ram[(spritePtr++)^1];
	int flags2 = paprium_obj_ram[(spritePtr++)^1];

	spr_x = pos_x;
	spr_y = pos_y;

	for( lcv = 0; lcv < count; lcv++ ) {
		int misc = ((paprium_obj_ram[spritePtr + 3] >> 4) & 0x0F);
		int size_x = ((paprium_obj_ram[spritePtr + 3] >> 2) & 0x03) + 1;
		int size_y = ((paprium_obj_ram[spritePtr + 3] >> 0) & 0x03) + 1;
		int tile = *(uint16*) (paprium_obj_ram + spritePtr + 4);
		int tileAttr = *(uint16*) (paprium_obj_ram + spritePtr + 6) & ~0x1FF;
		int ofs = *(uint16*) (paprium_obj_ram + spritePtr + 6) & 0x1FF;

		int tilePtr = paprium_tile_ptr + tile*4;
		int tileSize = size_x * size_y * 0x20;

		int sprAttr = 0;

		if( !flip_h )
			spr_x += (int8) paprium_obj_ram[spritePtr + 1];
		else
			spr_x -= (int8) paprium_obj_ram[spritePtr + 1];

		spr_y += (int8) paprium_obj_ram[spritePtr + 0];
		spritePtr += 8;

		if( tile == 0 ) continue;
		if( spriteCount >= 94 ) continue;

		sprAttr  = (tileAttr & 0x8000) ? 0x8000 : (objAttr & 0x8000);
		sprAttr += (tileAttr & 0x6000) ? (tileAttr & 0x6000) : (objAttr & 0x6000);
		sprAttr += (tileAttr & 0x1800) ^ (objAttr & 0x1800);

		if( (spr_y >= 128) && (spr_y + size_y*8 < 368) ) {
			if( (!flip_h && ((spr_x + size_x*8 >= 128) && spr_x < 448)) ||
				 (flip_h && ((spr_x - size_x*8 < 448) && spr_x >= 128)) ) {
				if( spriteCount < 80 ) {
					*(uint16*) (paprium_s.ram + 0xB00 + spriteCount*8) = spr_y & 0x3FF;
					*(uint16*) (paprium_s.ram + 0xB02 + spriteCount*8) = ((size_x-1) << 10) + ((size_y-1) << 8) + (spriteCount+1);
					*(uint16*) (paprium_s.ram + 0xB04 + spriteCount*8) = sprAttr + ((vram / 0x20) & 0x7FF);
					*(uint16*) (paprium_s.ram + 0xB06 + spriteCount*8) = (spr_x - ((!flip_h) ? 0 : size_x*8)) & 0x1FF;

				}
				else {
					if( spriteCount == 80 ) {  /* sprite 0 lock */
						paprium_s.ram[0xD7A] = 1;  /* exp.s list */

						memcpy(paprium_s.exps_ram, paprium_s.ram + 0xB00, 8);
						paprium_s.exps_ram[2] = 14;  /* normal list */

						spriteCount++;
					}

					*(uint16*) (paprium_s.exps_ram + 0 + (spriteCount-80)*8) = spr_y & 0x3FF;
					*(uint16*) (paprium_s.exps_ram + 2 + (spriteCount-80)*8) = ((size_x-1) << 10) + ((size_y-1) << 8) + ((spriteCount-80)+1);
					*(uint16*) (paprium_s.exps_ram + 4 + (spriteCount-80)*8) = sprAttr + ((vram / 0x20) & 0x7FF);
					*(uint16*) (paprium_s.exps_ram + 6 + (spriteCount-80)*8) = (spr_x - ((!flip_h) ? 0 : size_x*8)) & 0x1FF;

				}

				spriteCount++;

			}
		}

		int ptr = paprium_tile_ptr + (*(uint16*)(Rom_Data + tilePtr) << 16) + *(uint16*)(Rom_Data + tilePtr + 2);
		static uint8 tile_ram[0x20000];	/* gens64: room for an overrun */

		paprium_decoder_type(ptr, tile_ram);
		memcpy(paprium_s.ram + src, tile_ram + ofs * 0x20, tileSize);

		//if( !tiledupe && dmaPtr < dmalimit && vram < vramlimit && tile < tilelimit)

		*(uint16*) (paprium_s.ram + 0x1400 + dmaPtr*16) = 0x8F02;
		*(uint16*) (paprium_s.ram + 0x1402 + dmaPtr*16) = 0x9300 + ((tileSize >> 1) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x1404 + dmaPtr*16) = 0x9500 + ((src >> 1) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x1406 + dmaPtr*16) = 0x9400 + ((tileSize >> 9) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x1408 + dmaPtr*16) = 0x9700;
		*(uint16*) (paprium_s.ram + 0x140A + dmaPtr*16) = 0x9600 + ((src >> 9) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x140C + dmaPtr*16) = 0x4000 + (vram & 0x3FFF);
		*(uint16*) (paprium_s.ram + 0x140E + dmaPtr*16) = 0x0080 + (vram >> 14);

		*(uint16*) (paprium_s.ram + 0x1F16) = ++dmaPtr;
		*(uint16*) (paprium_s.ram + 0x1F18) = spriteCount;

		src += tileSize;
		dmaSize += tileSize;
		vram += tileSize;
	}

	paprium_s.draw_src = src;
	paprium_s.draw_dst = vram;
}

static void paprium_sprite_init(int arg)
{

	memset(paprium_s.ram + 0x1F20, 0, 14*8);

}

static void paprium_sprite_start(int arg)
{

	int count = *(uint16*)(paprium_s.ram + 0x1F18);

	*(uint16*) (paprium_s.ram + 0x1F16) = 0x0001;  /* dma count */

	*(uint16*) (paprium_s.ram + 0x1400) = 0x8f02;
	*(uint16*) (paprium_s.ram + 0x1402) = 0x9340;
	*(uint16*) (paprium_s.ram + 0x1404) = 0x9580;
	*(uint16*) (paprium_s.ram + 0x1406) = 0x9401;
	*(uint16*) (paprium_s.ram + 0x1408) = 0x9700;
	*(uint16*) (paprium_s.ram + 0x140a) = 0x9605;
	*(uint16*) (paprium_s.ram + 0x140c) = 0x7000;
	*(uint16*) (paprium_s.ram + 0x140e) = 0x0083;

	if( count < 80 )
		memcpy(paprium_s.ram + 0x1F20, paprium_s.ram + 0xB00, 14*8);
	else
		memcpy(paprium_s.ram + 0x1F20, paprium_s.exps_ram, 14*8);

	paprium_s.draw_src = 0x2000;
	paprium_s.draw_dst = 0x200;

}

static void paprium_sprite_stop(int arg)
{

	int count = *(uint16*)(paprium_s.ram + 0x1F18);

	if( count == 0 ) {
		memset(paprium_s.ram + 0xB00, 0, 8);
		memset(paprium_s.exps_ram, 0, 8);
	}
	else if( count <= 80 ) {
		paprium_s.ram[0xB02 + (count-1)*8] = 0;

		if( count <= 14 )
			paprium_s.exps_ram[2 + (count-1)*8] = 0;
	}
	else
		paprium_s.exps_ram[2 + (count-81)*8] = 0;

	if( arg == 2 )
		*(uint16*) (paprium_s.ram + 0x1F1C) = 1;  /* exp.s - force draw */

}

static void paprium_sprite_pause(int arg)
{

	int count = *(uint16*)(paprium_s.ram + 0x1F18);

	if( count == 0 )
		memset(paprium_s.ram + 0xB00, 0, 8);

}

static void paprium_scaler_init(int arg)
{
	int row, col, out;
	static uint8 temp[0x800];

	int ptr = (*(uint16 *)(paprium_s.ram + 0x1E10) << 16) + *(uint16 *)(paprium_s.ram + 0x1E12);
	paprium_decoder_type(ptr, temp);

	out = 0;

	for( col = 0; col < 64; col++ ) {
		for( row = 0; row < 64; row++ ) {
			if( col & 1 )
				paprium_s.scaler_ram[out] = temp[row*32 + (col/2)^1] & 0x0F;
			else
				paprium_s.scaler_ram[out] = temp[row*32 + (col/2)^1] >> 4;

			out++;
		}
	}

}

static void paprium_scaler(int arg)
{
	int row, col;

	int left = *(uint16 *)(paprium_s.ram + 0x1E10);
	int right = *(uint16 *)(paprium_s.ram + 0x1E12);
	int scale = *(uint16 *)(paprium_s.ram + 0x1E14);
	int ptr = *(uint16 *)(paprium_s.ram + 0x1E16);
	int step = 64 * 0x10000 / scale;
	int ptr_frac = 0;

	for( col = 0; col < 128; col++ ) {
		int base = (col & 4) ? 0x600 : 0x200;
		int out = ((col / 8) * 64) + ((col & 2) / 2);

		for( row = 0; row < 64; row += 2 ) {
			if( (col >= left) && (col < right) ) {
				if(col & 1)
					paprium_s.ram[base + out^1] += paprium_s.scaler_ram[row*64 + ptr] & 0x0F;
				else
					paprium_s.ram[base + out^1] = paprium_s.scaler_ram[row*64 + ptr] << 4;
			}
			else if( (col & 1) == 0 )
				paprium_s.ram[base + out^1] = 0;

			out += 2;
		}

		if( (col >= left) && (col < right) && (ptr < 64) ) {
			ptr_frac += 0x10000;

			while( ptr_frac >= step ) {
				ptr++;
				ptr_frac -= step;
			}
		}
	}

}

static void paprium_sram_read(int bank)
{

	int offset = *(uint16 *)(paprium_s.ram + 0x1E10);

	if( (bank >= 1) && (bank <= 4) )
		memcpy(paprium_s.ram + offset, SRAM + ((bank-1) * 0x780), 0x780);

}

static void paprium_sram_write(int bank)
{

	/* *(uint16 *)(paprium_s.ram + 0x1E10) */
	int offset = *(uint16 *)(paprium_s.ram + 0x1E12);

	if( (bank >= 1) && (bank <= 4) )
		memcpy(SRAM + ((bank-1) * 0x780), paprium_s.ram + offset, 0x780);

}

static void paprium_mapper(int arg)
{

	memcpy(paprium_s.ram + 0x8000, Rom_Data + 0x8000, 0x8000);  /* troll */

}

static void paprium_boot(int arg)
{

	paprium_music_ptr = (*(uint16*)(paprium_s.ram + 0x1E10) << 16) + *(uint16*)(paprium_s.ram + 0x1E12);
	/* (*(uint16*)(paprium_s.ram + 0x1E14) << 16) + *(uint16*)(paprium_s.ram + 0x1E16); */
	paprium_wave_ptr = (*(uint16*)(paprium_s.ram + 0x1E18) << 16) + *(uint16*)(paprium_s.ram + 0x1E1A);
	/* (*(uint16*)(paprium_s.ram + 0x1E1C) << 16) + *(uint16*)(paprium_s.ram + 0x1E1E); */
	paprium_sfx_ptr = (*(uint16*)(paprium_s.ram + 0x1E20) << 16) + *(uint16*)(paprium_s.ram + 0x1E22);
	paprium_sprite_ptr = (*(uint16*)(paprium_s.ram + 0x1E24) << 16) + *(uint16*)(paprium_s.ram + 0x1E26);
	paprium_tile_ptr = (*(uint16*)(paprium_s.ram + 0x1E28) << 16) + *(uint16*)(paprium_s.ram + 0x1E2A);

	/* paprium_wave_unpack(paprium_wave_ptr, paprium_wave_ram); */
	paprium_decoder_type(paprium_sprite_ptr, paprium_obj_ram);

	paprium_s.decoder_size = 0;

}

static void paprium_EC(int arg)
{

}

static void paprium_music(int track)
{

	track &= 0x7F;

	int ptr = (*(uint16*)(Rom_Data + paprium_music_ptr + track*4)<<16) + (*(uint16*)(Rom_Data + paprium_music_ptr + track*4 + 2));
	paprium_decoder_type(paprium_music_ptr + ptr, paprium_s.music_ram);
	paprium_s.decoder_size = 0;

	paprium_s.music_section = 0;
	paprium_s.music_segment = 0;
	paprium_s.audio_tick = 0;

	memset(paprium_s.music, 0, sizeof(paprium_s.music));
	for( int ch = 0; ch < 26; ch++ ) {
		paprium_voice_t *voice = paprium_s.music + ch;

		voice->panning = 0x80;
		voice->volume = 0x80;
		voice->program = paprium_s.music_ram[0x2A + ch^1];
	}

	paprium_set_track(track, 1);
}

static void paprium_music_volume(int level)
{

	paprium_s.music_volume = level;
	/* *(uint16 *)(paprium_s.ram + 0x1E10); */
	/* *(uint16 *)(paprium_s.ram + 0x1E12); */

}

static void paprium_music_setting(int flag)
{

	if( flag == 8 )
		paprium_s.music_segment = -1;

	else if( flag == 0 )
		paprium_s.music_segment = -1;

}

static void paprium_music_special(int flag)
{

	if( flag == 1 ) {
		//*(uint16 *)(paprium_s.ram + 0x1E10)
		//*(uint16 *)(paprium_s.ram + 0x1E12)

	}

	else if( flag == 2 ) {
		//*(uint16 *)(paprium_s.ram + 0x1E10)  /* 4 = crisis, 0 = normal ? */

	}

	else if( flag == 4 ) {
		//*(uint16 *)(paprium_s.ram + 0x1E10)  /* 81 = blu pill */

	}

	else if( flag == 6 ) {  /* sax man */
		//*(uint16 *)(paprium_s.ram + 0x1E10)
		//*(uint16 *)(paprium_s.ram + 0x1E12)
		//*(uint16 *)(paprium_s.ram + 0x1E14)

	}

	else if( flag == 7 ) {  /* blu pill - stage title */
		//*(uint16 *)(paprium_s.ram + 0x1E10)

	}

}

static void paprium_audio_setting(int flags)
{

	paprium_s.audio_flags = flags;

	paprium_s.ram[0x1801] = flags & 0x01;  /* dac */

	paprium_s.ram[0x1800]  = (flags & 0x01) ? 0x80 : 0x00;  /* dac */
	paprium_s.ram[0x1800] += (flags & 0x02) ? 0x40 : 0x00;  /* ntsc */

}

static void paprium_sfx_volume(int level)
{

	paprium_s.sfx_volume = level;
	/* *(uint16 *)(paprium_s.ram + 0x1E10); */
	/* *(uint16 *)(paprium_s.ram + 0x1E12); */

}

static void paprium_cmd(int data)
{
	int cmd = data >> 8;
	int lcv;

	data &= 0xFF;

	switch(cmd) {
	case 0x81:
	case 0x83:
	case 0x95:
	case 0x96:
	case 0xA4:
	case 0xA9:
	case 0xB6:
		goto print_0;

	case 0xD0:
		goto print_2;
		goto print_5;

	case 0xE7:  /* T-574120-0 = after demo loop */
		goto print_9;

	case 0xD5:  /* secret room */
	case 0xEC:
		goto print_2;

	case 0x84: paprium_mapper(data); break;
	case 0x88: paprium_audio_setting(data); break;
	case 0x8C: paprium_music(data); break;
	case 0x8D: paprium_music_setting(data); break;
	case 0xAD: paprium_sprite(data); break;
	case 0xAE: paprium_sprite_start(data); break;
	case 0xAF: paprium_sprite_stop(data); break;
	case 0xB0: paprium_sprite_init(data); break;
	case 0xB1: paprium_sprite_pause(data); break;
	case 0xC6: paprium_boot(data); break;
	case 0xC9: paprium_music_volume(data); break;
	case 0xCA: paprium_sfx_volume(data); break;
	case 0xD1: goto sfx_play;
	case 0xD2: goto sfx_off;
	case 0xD3: goto sfx_loop;
	case 0xD6: paprium_music_special(data); break;
	case 0xDA: paprium_decoder(data); break;
	case 0xDB: paprium_decoder_copy(data); break;
	case 0xDF: paprium_sram_read(data); break;
	case 0xE0: paprium_sram_write(data); break;
	//case 0xF2: paprium_block_viewer(data); break;
	case 0xF4: paprium_scaler_init(data); break;
	case 0xF5: paprium_scaler(data); break;

	default: goto print_x;
	}

done:
	*(uint16*)(paprium_s.ram + 0x1FEA) &= 0x7FFF;
	return;

print_0:
	goto done;

print_1:
	goto done;

print_2:
	goto done;

print_4:
	goto done;

print_5:
	goto done;

print_9:
	goto done;

print_x:
	goto done;

sfx_play:
	{
		int chan, vol, pan, flags;
		int ptr, size, type;
		int lcv;
		int newch = 0, maxtime = 0;
		const int rates[] = {1,2,4,5,8,9};
		uint8 *sfx = paprium_sfx_ptr + Rom_Data;

		chan = *(uint16 *)(paprium_s.ram + 0x1E10);
		vol = *(uint16 *)(paprium_s.ram + 0x1E12);
		pan = *(uint16 *)(paprium_s.ram + 0x1E14);
		flags = *(uint16 *)(paprium_s.ram + 0x1E16);

		ptr = (*(uint16 *)(sfx + data*8) << 16) | (*(uint16 *)(sfx + data*8 + 2));
		size = (*(uint8 *)(sfx + data*8 + 4) << 16) | (*(uint16 *)(sfx + data*8 + 6));
		type = *(uint8 *)(sfx + data*8 + 5);

		for( lcv = 0; lcv < 8; lcv++, chan >>= 1 ) {
			if( (chan & 1) == 0 ) continue;

			if( paprium_s.sfx[lcv].size ) {
				if( maxtime < paprium_s.sfx[lcv].time ) {
					maxtime = paprium_s.sfx[lcv].time;
					newch = lcv;
				}
				continue;
			}

			newch = lcv;
			break;
		}

		paprium_s.sfx[newch].volume = vol;
		paprium_s.sfx[newch].panning = pan;
		paprium_s.sfx[newch].type = type;

		paprium_s.sfx[newch].num = data;
		paprium_s.sfx[newch].flags = flags;

		paprium_s.sfx[newch].loop = 0;
		paprium_s.sfx[newch].count = 0;
		paprium_s.sfx[newch].time = 0;
		paprium_s.sfx[newch].tick = 0;
		paprium_s.sfx[newch].decay = 0;

		if( flags & 0x4000 )
			paprium_s.sfx[newch].echo = (paprium_s.echo_pan++) & 1;

		paprium_s.sfx[newch].ptr = ptr;
		paprium_s.sfx[newch].start = ptr;
		paprium_s.sfx[newch].size = size;
	}
	goto done;

sfx_loop:
	{
		int lcv;

		for( lcv = 0; lcv < 8; lcv++, data >>= 1 ) {
			if( (data & 1) == 0 ) continue;

			paprium_s.sfx[lcv].volume = *(uint16 *)(paprium_s.ram + 0x1E10);
			paprium_s.sfx[lcv].panning = *(uint16 *)(paprium_s.ram + 0x1E12);
			paprium_s.sfx[lcv].decay = *(uint16 *)(paprium_s.ram + 0x1E14);

			paprium_s.sfx[lcv].loop = 1;

			break;
		}
	}
	goto done;

sfx_off:
	{
		int flags;

		flags = *(uint16 *)(paprium_s.ram + 0x1E10);

		if(0) {}
		//else if( flags == 6 ) {}
		else if( flags == 0 ) {}
		else {} //MessageBoxA(0, "Sfx_Off flags", "Warn", MB_OK );

		for( lcv = 0; lcv < 8; lcv++ ) {
			if( !(data & (1 << lcv) ) ) continue;

			if( flags == 0 ) {
				paprium_s.sfx[lcv].size = 0;
			}

			paprium_s.sfx[lcv].decay = flags;
			paprium_s.sfx[lcv].loop = 0;

			break;
		}
	}
	goto done;
}


/* ======================= gens64: sound frame ======================= */

/* music: step 2 (MP3 tracks); silent here, the track position is not visible to the game */
static void paprium_music_out(int *out_l, int *out_r)
{
	*out_l = 0;
	*out_r = 0;
}

/* as paprium_audio() of the GPGX code, for a given number of 48 kHz samples */
static void paprium_audio(int samples)
{
	int lcv;

	paprium_s.audio_tick++;

	for( lcv = 0; lcv < samples; lcv++ ) {
		int l = 0, r = 0;

		paprium_s.echo_l[paprium_s.echo_ptr] = 0;
		paprium_s.echo_r[paprium_s.echo_ptr] = 0;

		paprium_music_out(&l, &r);
		paprium_sfx_voice(&l, &r);

		paprium_s.echo_ptr = (paprium_s.echo_ptr+1) % (48000/6);

		l += paprium_s.echo_l[paprium_s.echo_ptr];
		r += paprium_s.echo_r[paprium_s.echo_ptr];

		l = (l * paprium_s.sfx_volume) / 0x100;
		r = (r * paprium_s.sfx_volume) / 0x100;

		if( paprium_s.audio_flags & 0x08 ) {  /* gain */
			l = l * 4 / 2;
			r = r * 4 / 2;
		}

		if( l > 32767 ) l = 32767;
		else if( l < -32768 ) l = -32768;

		if( r > 32767 ) r = 32767;
		else if( r < -32768 ) r = -32768;

		if (lcv < PAPRIUM_MAX_SAMPLES)
		{
			paprium_out[0][lcv] = l;
			paprium_out[1][lcv] = r;
		}

		paprium_s.out_l = l;
		paprium_s.out_r = r;
	}
	paprium_out_count = samples < PAPRIUM_MAX_SAMPLES ? samples : PAPRIUM_MAX_SAMPLES;

	/* writes the channel levels at 1B98 that the game reads */
	paprium_music_sheet();

	pap.track_last = paprium_s.music_track;
}

void Paprium_Frame(int pal)
{
	/* 48000 samples a second; a frame is 262 (NTSC) or 313 (PAL) lines of 3420 master clocks */
	uint32 clock = pal ? 53203424u : 53693175u;
	uint32 per_frame = (pal ? 313u : 262u) * 3420u;
	unsigned long long acc = (unsigned long long)pap.sample_acc + (unsigned long long)per_frame * 48000u;
	int samples = (int)(acc / clock);
	pap.sample_acc = (uint32)(acc % clock);
	paprium_audio(samples);
#ifdef PAPRIUM_TRACE
	trace_frame++;
	{ int i, n = *(uint16*)(paprium_s.ram + 0x1F16); char b[4096]; int l = 0;
	  for (i = 0; i < n && i < 120; i++) { uint16 *e = (uint16*)(paprium_s.ram + 0x1400 + i*16); l += sprintf(b + l, " %04X:%X", ((e[2]&0xFF) | ((e[5]&0xFF)<<8)) << 1, (e[1]&0xFF) | ((e[3]&0xFF)<<8)); }
	  TRACE("%u frame list n=%d%s\n", trace_frame, n, b); }
#endif
}

/* ======================= gens64: memory ======================= */

static uint32 paprium_r8(uint32 address)
{
	int data = paprium_s.ram[address^1];

	if( address == 0x1800 )
		data = 0;

	else if( address >= 0x1880 && address < 0x1b00 ) {
		data = (int)((paprium_rand() >> 16) & 0xFF);	/* gens64: was rand() */
	}

	return data;
}

static uint32 paprium_r16(uint32 address)
{
	int data = 0;

	if( (address == 0xC000) && (paprium_s.decoder_size > 0) ) {
		int max = 0x4000, size;	/* gens64: max was left unset for other modes */

		if( paprium_s.decoder_mode == 2 )
			max = 0x4000;

		else if( paprium_s.decoder_mode == 7 )
			max = 0x800;

		size = (paprium_s.decoder_size > max) ? max : paprium_s.decoder_size;

		memcpy(paprium_s.ram + 0xC000, paprium_s.decoder_ram + paprium_s.decoder_ptr, size);

		paprium_s.decoder_ptr += size;
		paprium_s.decoder_size -= size;
	}

	switch( address ) {
	case 0x1FE4:
		data = 0xFFFF;
		data &= ~(1<<2);
		data &= ~(1<<6);
		break;

	case 0x1FE6:
		data = 0xFFFF;
		data &= ~(1<<14);
		data &= ~(1<<8);  /* sram */
		data &= ~(1<<9);  /* sram */
		break;

	case 0x1FEA:
		data = 0xFFFF;
		data &= ~(1<<15);
		break;

	default:
		data = *(uint16 *)(paprium_s.ram + address);
		break;
	}

	return data;
}

static void paprium_w8(uint32 address, uint32 data)
{
	paprium_s.ram[address^1] = data;
}

static void paprium_w16(uint32 address, uint32 data)
{
	*(uint16 *)(paprium_s.ram + address) = data;

	if( address == 0x1FEA ) {
#ifdef PAPRIUM_TRACE
		if (dma_n) { if (run_len) runs_len += sprintf(runs + runs_len, " %04X:%X", run_start, run_len); TRACE("%u   dma %u words%s\n", trace_frame, dma_n, runs); dma_n = 0; dma_lo = ~0u; dma_hi = 0; runs_len = 0; runs[0] = 0; run_len = 0; run_last = 0; }
#endif
		{ extern int VDP_Current_Line; TRACE("L%03d ", VDP_Current_Line); }
		TRACE("%u pc=%06X cmd %04X  1E10: %04X %04X %04X %04X\n", trace_frame, main68k_readPC(), data,
			*(uint16 *)(paprium_s.ram + 0x1E10), *(uint16 *)(paprium_s.ram + 0x1E12), *(uint16 *)(paprium_s.ram + 0x1E14), *(uint16 *)(paprium_s.ram + 0x1E16));
		paprium_cmd(data);
	}
}

/* 000000-9FFFFF: the chip's RAM, then the ROM, linear (no bank registers, no SRAM window) */
static unsigned int read_byte(unsigned int a)
{
	if (a < 0x10000)
		return paprium_r8(a);
	return Rom_Data[(a & Rom_Mask) ^ 1];
}
static unsigned int read_word(unsigned int a)
{
	if (a < 0x10000)
		return paprium_r16(a);
	return *(uint16 *)(Rom_Data + (a & Rom_Mask & ~1u));
}
/* 000000-0FFFFF: writes reach the RAM only */
static void write_byte(unsigned int a, unsigned int d)
{
	if (a < 0x10000)
		paprium_w8(a, d & 0xFF);
}
static void write_word(unsigned int a, unsigned int d)
{
	if (a < 0x10000)
		paprium_w16(a & 0xFFFE, d & 0xFFFF);
}

/* A00000-A7FFFF read and A00000-AFFFFF write: A14101 is a register of the chip (TMSS) */
typedef unsigned int (*rd_fn)(unsigned int);
typedef void (*wr_fn)(unsigned int, unsigned int);
static rd_fn io_read_byte;
static wr_fn io_write_byte;
static unsigned int io_rb(unsigned int a)
{
	if (a == 0xA14101)
		return (unsigned int)pap.tmss & 0xFF;
	return io_read_byte(a);
}
static void io_wb(unsigned int a, unsigned int d)
{
	if (a == 0xA14101)
	{
		pap.tmss = (int)(d & 0xFF);
		return;
	}
	io_write_byte(a, d);
}

void Paprium_Install(void **rb, void **rw, void **wb, void **ww)
{
	int i;
	for (i = 0; i < 20; i++)
	{
		rb[i] = (void *)read_byte;
		rw[i] = (void *)read_word;
	}
	wb[0] = (void *)write_byte;
	ww[0] = (void *)write_word;
	if (rb[20] != (void *)io_rb)
	{
		io_read_byte = (rd_fn)rb[20];
		rb[20] = (void *)io_rb;
	}
	if (wb[10] != (void *)io_wb)
	{
		io_write_byte = (wr_fn)wb[10];
		wb[10] = (void *)io_wb;
	}
}

int Paprium_Fetch(struct STARSCREAM_PROGRAMREGION *f)
{
	f[0].lowaddr = 0x000000;
	f[0].highaddr = 0x00FFFF;
	f[0].offset = GA(paprium_s.ram);
	f[1].lowaddr = 0x010000;
	f[1].highaddr = 0x7FFFFF;
	f[1].offset = GA(Rom_Data);
	f[2].lowaddr = 0x800000;	/* mirror */
	f[2].highaddr = 0x9FFFFF;
	f[2].offset = GA(Rom_Data) - 0x800000;
	return 3;
}

unsigned int Paprium_DMA_Word(unsigned int a)
{
#ifdef PAPRIUM_TRACE
	if (1) { dma_n++; if (a < dma_lo) dma_lo = a; if (a > dma_hi) dma_hi = a;
		if (a != run_last + 2) { if (run_len && runs_len < 4000) runs_len += sprintf(runs + runs_len, " %X:%X", run_start, run_len); run_start = a; run_len = 0; }
		run_len++; run_last = a; }
#endif
	return read_word(a & 0xFFFFFE);
}

/* ======================= gens64: setup ======================= */

int Paprium_Detect(void)
{
	static const char id[] = "T-574120-00";
	int i;
	if (Rom_Size < 0x800000)
		return 0;
	for (i = 0; i < 11; i++)
		if (Rom_Data[(0x183 + i) ^ 1] != (uint8)id[i])
			return 0;
	return 1;
}

/* paprium_init() of the GPGX code (run on every reset there too) */
void Paprium_Reset(void)
{
	int ptr;

	memset(&paprium_box, 0, sizeof(paprium_box));
	memset(&pap, 0, sizeof(pap));
	memcpy(paprium_s.ram, Rom_Data, 0x10000);
	pap.tmss = 1;
	pap.rng = 0x2545F491;	/* gens64: fixed seed */

	ptr = (*(uint16*)(Rom_Data + 0xaf77c) << 16) + *(uint16*)(Rom_Data + 0xaf77e);

	paprium_music_ptr = (*(uint16*)(Rom_Data + 0x10054)<< 16) + *(uint16*)(Rom_Data + 0x10056);
	paprium_wave_ptr = (*(uint16*)(Rom_Data + ptr + 0x774)<< 16) + *(uint16*)(Rom_Data + ptr + 0x776);
	paprium_sfx_ptr = (*(uint16*)(Rom_Data + ptr + 0x778)<< 16) + *(uint16*)(Rom_Data + ptr + 0x77a);
	paprium_sprite_ptr = (*(uint16*)(Rom_Data + 0x10014)<< 16) + *(uint16*)(Rom_Data + 0x10016);
	paprium_tile_ptr = (*(uint16*)(Rom_Data + ptr + 0x780)<< 16) + *(uint16*)(Rom_Data + ptr + 0x782);

	paprium_decoder_type(paprium_sprite_ptr, paprium_obj_ram);
	paprium_s.decoder_size = 0;

	*(uint16*)(paprium_s.ram + 0x192) = 0x3634;  /* 6-button, multitap */

	paprium_s.ram[0x1D1D^1] = 0x04;  /* rom ok - dynamic patch */
	paprium_s.ram[0x1D2C^1] = 0x67;

	/* boot hack */
	*(uint16*) (paprium_s.ram + 0x1560) = 0x4EF9;
	*(uint16*) (paprium_s.ram + 0x1562) = 0x0001;
	*(uint16*) (paprium_s.ram + 0x1564) = 0x0100;

	/* WM text - pre-irq delay (ROM patch) */
	*(uint16*)(Rom_Data + 0xb9094) = 0x2079;
	*(uint16*)(Rom_Data + 0xb9096) = 0x000a;
	*(uint16*)(Rom_Data + 0xb9098) = 0xf85c;

	*(uint16*)(Rom_Data + 0xb909a) = 0x20bc;
	*(uint16*)(Rom_Data + 0xb909c) = 0x0000;
	*(uint16*)(Rom_Data + 0xb909e) = 0x0003;

	*(uint16*)(Rom_Data + 0xb90a0) = 0x20bc;
	*(uint16*)(Rom_Data + 0xb90a2) = 0x0000;
	*(uint16*)(Rom_Data + 0xb90a4) = 0x0003;

	*(uint16*)(Rom_Data + 0xb90a6) = 0x20bc;
	*(uint16*)(Rom_Data + 0xb90a8) = 0x0000;
	*(uint16*)(Rom_Data + 0xb90aa) = 0x0003;

	*(uint16*)(Rom_Data + 0xb90ac) = 0x20bc;
	*(uint16*)(Rom_Data + 0xb90ae) = 0x0000;
	*(uint16*)(Rom_Data + 0xb90b0) = 0x0003;

	*(uint16*)(Rom_Data + 0xb90b2) = 0x20bc;
	*(uint16*)(Rom_Data + 0xb90b4) = 0x0000;
	*(uint16*)(Rom_Data + 0xb90b6) = 0x0003;

	/* gens64: emulator check at 8A4F4 -> BCC82: writes F0FF to CRAM and reads it back. The real VDP
	   keeps only the colour bits (Genesis Plus GX too); Gens returns the whole word, the game then
	   takes itself for an emulator, skips setting up its interrupt vectors in the chip's RAM and
	   crashes. Here the check always answers "real console" (moveq #0,d0; rts). */
	*(uint16*)(Rom_Data + 0x8a4f4) = 0x7000;
	*(uint16*)(Rom_Data + 0x8a4f6) = 0x4e75;

	paprium_s.echo_pan = 0;	/* gens64: was rand() */

	paprium_s.music_segment = -1;
	pap.track_last = 0;
}

/* ======================= gens64: savestate ======================= */

typedef char paprium_state_fits[(sizeof(struct paprium_t) + sizeof(pap) + 16 <= PAPRIUM_STATE_SIZE) ? 1 : -1];

void Paprium_Save(unsigned char *data)
{
	memset(data, 0, PAPRIUM_STATE_SIZE);
	memcpy(data, "PAPR", 4);
	memcpy(data + 16, &paprium_s, sizeof(struct paprium_t));
	memcpy(data + 16 + sizeof(struct paprium_t), &pap, sizeof(pap));
}

void Paprium_Load(const unsigned char *data)
{
	if (memcmp(data, "PAPR", 4))
		return;
	memcpy(&paprium_s, data + 16, sizeof(struct paprium_t));
	memcpy(&pap, data + 16 + sizeof(struct paprium_t), sizeof(pap));
	/* the sprite data comes from the ROM at the pointer the game gave */
	paprium_decoder_type(paprium_sprite_ptr, paprium_obj_ram);
}

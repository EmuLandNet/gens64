/* gens64: YX5200-24SS MP3 player (DFPlayer Mini chip) on the KAISER WAVE cartridge board:
   Death & Lead, The Secret Of The Four Winds (docs/yx5200.md).

   Behaviour follows Genesis Plus GX (core/cart_hw/yx5200.c, Eke-Eke 2025): the game bit-bangs
   10-byte DFPlayer commands at 9600 baud into the RX line (bit 0 of /TIME writes to even
   addresses), the player plays NN.mp3 from the ROM's folder. Nothing is read back, so the player
   does not change the emulation: movies sync with or without it. The music is decoded with
   minimp3 (third_party/minimp3, public domain), resampled linearly to the sound rate and added to
   the frame's sound like the Sega CD audio. */

#include <stdio.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD		/* same decoding on x86 and x86-64 */
#include "../../third_party/minimp3/minimp3_ex.h"

#include "yx5200.h"

extern unsigned char Rom_Data[];
extern unsigned int Rom_Size;
extern int Sound_Rate;
extern char Recent_Rom[][1024];

#define RX_SIZE 10
#define MAX_TRACK 2999
#define MAX_VOLUME 30

int YX5200_On;

static struct
{
	unsigned char rxCycle, rxCounter, rxBuffer[RX_SIZE];
	unsigned char playing;	/* playback running (0: stopped or paused) */
	unsigned char loop;		/* repeat the track */
	unsigned char volume;	/* 0-30 */
	unsigned char dac;		/* audio output on */
	unsigned short track;	/* loaded track (0: none) */
} yx;

static FILE *mp3_file;
static mp3dec_ex_t mp3;
static int mp3_open;
static unsigned int played;		/* input frames (samples per channel) consumed */
/* linear resampling: output between s0 and s1, frac = position in 16.16 */
static int s0[2], s1[2];
static unsigned int frac;
static short dec_buf[2304 * 2];
static int dec_len, dec_pos;	/* samples (not frames) in dec_buf */

static size_t read_cb(void *buf, size_t size, void *user)
{
	return fread(buf, 1, size, (FILE *)user);
}
static int seek_cb(uint64_t pos, void *user)
{
	return fseek((FILE *)user, (long)pos, SEEK_SET);
}
static mp3dec_io_t mp3_io = {read_cb, NULL, seek_cb, NULL};

/* original header byte (Rom_Data is byteswapped) */
static unsigned char hdr(unsigned int a)
{
	return Rom_Data[a ^ 1];
}

static unsigned short real_checksum(void)
{
	unsigned short sum = 0;
	unsigned int i;
	for (i = 0x200; i + 1 < Rom_Size; i += 2)
		sum = (unsigned short)(sum + (hdr(i) << 8 | hdr(i + 1)));
	return sum;
}

static void reset_resampler(void)
{
	s0[0] = s0[1] = s1[0] = s1[1] = 0;
	frac = 0;
	dec_len = dec_pos = 0;
}

static void unload_track(void)
{
	if (mp3_open)
	{
		mp3dec_ex_close(&mp3);
		mp3_open = 0;
	}
	if (mp3_file)
	{
		fclose(mp3_file);
		mp3_file = NULL;
	}
	yx.playing = 0;
	yx.track = 0;
	played = 0;
	reset_resampler();
}

/* folder of the loaded ROM (the archive's folder for an archive), with the trailing separator */
static void rom_folder(char *dir, size_t size)
{
	char *p, *q;
	strncpy(dir, Recent_Rom[0], size - 1);
	dir[size - 1] = 0;
	if ((p = strchr(dir, '|')) != NULL)
		*p = 0;
	p = strrchr(dir, '\\');
	q = strrchr(dir, '/');
	if (q > p)
		p = q;
	if (p)
		p[1] = 0;
	else
		dir[0] = 0;
}

static void load_track(unsigned int index, int loop)
{
	static const char *fmt[4] = {"%s%01u.mp3", "%s%02u.mp3", "%s%03u.mp3", "%s%04u.mp3"};
	char dir[1024], name[1100];
	int i;

	unload_track();
	rom_folder(dir, sizeof(dir));
	for (i = 0; i < 4; i++)
	{
		snprintf(name, sizeof(name), fmt[i], dir, index);
		if ((mp3_file = fopen(name, "rb")) == NULL)
			continue;
		mp3_io.read_data = mp3_io.seek_data = mp3_file;
		if (mp3dec_ex_open_cb(&mp3, &mp3_io, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN) == 0 &&
			mp3.info.channels >= 1 && mp3.info.channels <= 2 && mp3.info.hz > 0)
		{
			mp3_open = 1;
			yx.track = (unsigned short)index;
			yx.playing = 1;
			yx.loop = (unsigned char)loop;
		}
		else
		{
			mp3dec_ex_close(&mp3);
			fclose(mp3_file);
			mp3_file = NULL;
		}
		break;	/* like GPGX: the first file found decides */
	}
}

void YX5200_Close(void)
{
	unload_track();
	YX5200_On = 0;
}

void YX5200_Reset(void)
{
	unload_track();
	memset(&yx, 0, sizeof(yx));
	yx.volume = MAX_VOLUME;
	yx.dac = 1;
}

void YX5200_Init(void)
{
	YX5200_Close();
	/* both games keep the SGDK sample header: only the checksums tell them apart (as in GPGX) */
	if (Rom_Size >= 0x200 && hdr(0x180) == 'G' && hdr(0x181) == 'M')
	{
		char product[12];
		unsigned short checksum = (unsigned short)(hdr(0x18E) << 8 | hdr(0x18F));
		int i;
		for (i = 0; i < 11; i++)
			product[i] = (char)hdr(0x183 + i);
		product[11] = 0;
		if (strstr(product, "00000000-00"))
		{
			unsigned short real = 0;
			if (checksum == 0x2284 || checksum == 0x45C1)
				real = real_checksum();
			if ((checksum == 0x2284 && real == 0x8DCF) ||	/* Death & Lead */
				(checksum == 0x45C1 && real == 0xC613))		/* The Secret Of The Four Winds */
				YX5200_On = 1;
		}
	}
	YX5200_Reset();
}

static void process_cmd(void)
{
	unsigned int arg = (unsigned int)yx.rxBuffer[5] << 8 | yx.rxBuffer[6];
	switch (yx.rxBuffer[3])	/* format and checksum are not checked (GPGX) */
	{
	case 0x01:	/* next track */
		if (yx.track > 0 && yx.track < MAX_TRACK)
			load_track(yx.track + 1, yx.loop);
		break;
	case 0x02:	/* previous track */
		if (yx.track > 1)
			load_track(yx.track - 1, yx.loop);
		break;
	case 0x03:	/* play track (repeated) */
		if (arg > 0 && arg <= MAX_TRACK)
			load_track(arg, 1);
		break;
	case 0x04:
		if (yx.volume < MAX_VOLUME)
			yx.volume++;
		break;
	case 0x05:
		if (yx.volume > 0)
			yx.volume--;
		break;
	case 0x06:
		if (arg <= MAX_VOLUME)
			yx.volume = (unsigned char)arg;
		break;
	case 0x08:	/* "single repeat": played once, as GPGX assumes */
		if (arg > 0 && arg <= MAX_TRACK)
			load_track(arg, 0);
		break;
	case 0x0C:
		YX5200_Reset();
		break;
	case 0x0D:	/* resume */
		if (yx.track)
			yx.playing = 1;
		break;
	case 0x0E:	/* pause */
		yx.playing = 0;
		break;
	case 0x16:	/* stop */
		unload_track();
		break;
	case 0x19:	/* loop on/off during playback */
		if (yx.playing)
			yx.loop = (yx.rxBuffer[6] & 1) ^ 1;
		break;
	case 0x1A:	/* DAC on/off */
		yx.dac = (yx.rxBuffer[6] & 1) ^ 1;
		break;
	default:
		break;
	}
}

void YX5200_Write(unsigned int bit)
{
	bit &= 1;
	if (yx.rxCycle == 0)
	{
		if (!bit)	/* start bit */
		{
			yx.rxBuffer[yx.rxCounter] = 0;
			yx.rxCycle = 1;
		}
	}
	else if (yx.rxCycle < 9)	/* data, LSB first */
	{
		yx.rxBuffer[yx.rxCounter] |= (unsigned char)(bit << (yx.rxCycle - 1));
		yx.rxCycle++;
	}
	else
	{
		if (bit)	/* stop bit */
		{
			if (++yx.rxCounter == RX_SIZE)
			{
				process_cmd();
				yx.rxCounter = 0;
			}
		}
		yx.rxCycle = 0;
	}
}

/* next input frame into s1; 0 at the end of the track (stopped) */
static int next_frame(void)
{
	int ch = mp3.info.channels;
	if (dec_pos >= dec_len)
	{
		dec_len = (int)mp3dec_ex_read(&mp3, dec_buf, sizeof(dec_buf) / sizeof(dec_buf[0]) / ch * ch);
		dec_pos = 0;
		if (dec_len < ch)
		{
			if (yx.loop && mp3.last_error == 0)
			{
				mp3dec_ex_seek(&mp3, 0);
				played = 0;
				dec_len = (int)mp3dec_ex_read(&mp3, dec_buf, sizeof(dec_buf) / sizeof(dec_buf[0]) / ch * ch);
			}
			if (dec_len < ch)
			{
				unload_track();
				return 0;
			}
		}
	}
	s0[0] = s1[0];
	s0[1] = s1[1];
	s1[0] = dec_buf[dec_pos];
	s1[1] = dec_buf[dec_pos + ch - 1];
	dec_pos += ch;
	played++;
	return 1;
}

void YX5200_Update(int **buf, int length)
{
	unsigned int step;
	int i, vol;

	if (!YX5200_On || !yx.playing || !mp3_open || Sound_Rate <= 0)
		return;
	step = (unsigned int)(((unsigned long long)mp3.info.hz << 16) / (unsigned int)Sound_Rate);
	vol = (yx.dac && buf) ? yx.volume : 0;
	for (i = 0; i < length; i++)
	{
		frac += step;
		while (frac >= 0x10000)
		{
			frac -= 0x10000;
			if (!next_frame())
				return;
		}
		if (vol)
		{
			int f = (int)frac;
			int l = s0[0] + (((s1[0] - s0[0]) * f) >> 16);
			int r = s0[1] + (((s1[1] - s0[1]) * f) >> 16);
			buf[0][i] += l * vol / MAX_VOLUME;
			buf[1][i] += r * vol / MAX_VOLUME;
		}
	}
}

void YX5200_Save(unsigned char *d)
{
	memset(d, 0, YX5200_STATE_SIZE);
	if (!YX5200_On)
		return;
	d[0] = 'Y';
	d[1] = (unsigned char)yx.track;
	d[2] = (unsigned char)(yx.track >> 8);
	d[3] = (unsigned char)((yx.playing & 1) | (yx.loop & 1) << 1 | (yx.dac & 1) << 2);
	d[4] = yx.volume;
	d[5] = (unsigned char)played;
	d[6] = (unsigned char)(played >> 8);
	d[7] = (unsigned char)(played >> 16);
	d[8] = (unsigned char)(played >> 24);
	d[9] = (unsigned char)frac;
	d[10] = (unsigned char)(frac >> 8);
}

void YX5200_Load(const unsigned char *d)
{
	unsigned int track, pos;
	if (!YX5200_On)
		return;
	/* a command half-received when the state was saved is lost (the serial state is not saved) */
	yx.rxCycle = yx.rxCounter = 0;
	if (!d || d[0] != 'Y')
	{
		unload_track();
		return;
	}
	track = d[1] | (unsigned int)d[2] << 8;
	pos = d[5] | (unsigned int)d[6] << 8 | (unsigned int)d[7] << 16 | (unsigned int)d[8] << 24;
	if (track == 0 || track > MAX_TRACK)
		unload_track();
	else if (track != yx.track || !mp3_open)
		load_track(track, 0);	/* the same track stays open: its seek index is kept */
	if (yx.track)
	{
		/* back two frames: s0 and s1 as they were, then the same interpolation phase */
		unsigned int back = pos < 2 ? pos : 2;
		reset_resampler();
		mp3dec_ex_seek(&mp3, (uint64_t)(pos - back) * mp3.info.channels);
		played = pos - back;
		while (back-- && next_frame())
			;
		frac = d[9] | (unsigned int)d[10] << 8;
	}
	yx.playing = (d[3] & 1) && yx.track;
	yx.loop = (d[3] >> 1) & 1;
	yx.dac = (d[3] >> 2) & 1;
	yx.volume = d[4] <= MAX_VOLUME ? d[4] : MAX_VOLUME;
}

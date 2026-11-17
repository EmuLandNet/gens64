/* gens64: Sega CD images in MAME's CHD format (docs/segacd-formats.md), after Genesis Plus GX
   (core/cd_hw/cdd.c, Eke-Eke), with libchdr (third_party/libchdr).
   The TOC comes from the CHD track metadata; sectors are read hunk by hunk (one hunk cached).
   A CHD frame is 2352 bytes of sector + 96 of subcode; audio is stored big-endian. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <retro_inline.h>
#include <libchdr/chd.h>
#include <libchdr/cdrom.h>
#include "cd_chd.h"

#define FRAME 2448	/* CD_FRAME_SIZE: 2352 + 96 */
#define TRACK_PADDING 4	/* CD_TRACK_PADDING */

int CHD_On = 0;

static chd_file *chd;
static unsigned char *hunk;
static unsigned int hunkbytes;
static int hunknum = -1;
static int data_size;	/* 2048 (MODE1) or 2352 (MODE1_RAW/MODE2_RAW) */

static struct
{
	int start, end;	/* LBA */
	long long offset;	/* CHD byte offset of LBA 0 for this track: offset + lba * FRAME */
	int audio;
} trk[100];
static int ntracks, toc_end;

void CHD_Close(void)
{
	if (chd)
		chd_close(chd);
	chd = NULL;
	free(hunk);
	hunk = NULL;
	hunknum = -1;
	ntracks = 0;
	CHD_On = 0;
}

int CHD_Open(const char *name)
{
	const chd_header *head;
	int sectors = 0;
	static const char fmt1[] = "TRACK:%d TYPE:%15s SUBTYPE:%15s FRAMES:%d";
	static const char fmt2[] = "TRACK:%d TYPE:%15s SUBTYPE:%15s FRAMES:%d PREGAP:%d PGTYPE:%15s PGSUB:%15s POSTGAP:%d";

	CHD_Close();
	data_size = 0;
	toc_end = 0;
	if (chd_open(name, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE)
	{
		chd = NULL;
		return -1;
	}
	head = chd_get_header(chd);
	if (!head || head->hunkbytes == 0 || head->hunkbytes % FRAME)
	{
		CHD_Close();
		return -1;
	}
	hunkbytes = head->hunkbytes;
	hunk = (unsigned char *)malloc(hunkbytes);
	if (!hunk)
	{
		CHD_Close();
		return -1;
	}

	for (ntracks = 0; ntracks < 99; ntracks++)
	{
		char meta[256], type[16], subtype[16], pgtype[16], pgsub[16];
		uint32_t len = 0;
		int tracknum = 0, frames = 0, pregap = 0, postgap = 0;
		type[0] = subtype[0] = pgtype[0] = pgsub[0] = 0;
		if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, ntracks, meta, sizeof(meta) - 1, &len, 0, 0) == CHDERR_NONE)
		{
			if (len >= sizeof(meta))
				break;
			meta[len] = 0;
			if (sscanf(meta, fmt2, &tracknum, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap) != 8)
				break;
		}
		else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, ntracks, meta, sizeof(meta) - 1, &len, 0, 0) == CHDERR_NONE)
		{
			if (len >= sizeof(meta))
				break;
			meta[len] = 0;
			if (sscanf(meta, fmt1, &tracknum, type, subtype, &frames) != 4)
				break;
		}
		else
			break;
		if (tracknum != ntracks + 1 || frames < 0 || pregap < 0 || postgap < 0)
			break;

		if (ntracks)
		{
			if (strcmp(type, "AUDIO"))	/* only the first track can be data */
				break;
			trk[ntracks].start = toc_end + pregap;
			trk[ntracks].audio = 1;
		}
		else
		{
			if (!strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE2_RAW"))
				data_size = 2352;
			else if (!strcmp(type, "MODE1"))
				data_size = 2048;
			else
				break;	/* audio first track: not a Sega CD disc */
			trk[0].start = 0;
			trk[0].audio = 0;
		}
		/* pause not in the CHD ("V" type: it is in the file) */
		if (pgtype[0] != 'V')
			pregap = 0;
		trk[ntracks].end = trk[ntracks].start + frames - pregap;
		trk[ntracks].offset = (long long)(sectors + pregap - trk[ntracks].start) * FRAME;
		toc_end = trk[ntracks].end + postgap;
		sectors += ((frames + TRACK_PADDING - 1) / TRACK_PADDING) * TRACK_PADDING;
	}

	if (!data_size || !ntracks || toc_end > 99 * 60 * 75)
	{
		CHD_Close();
		return -1;
	}
	CHD_On = 1;
	return 0;
}

int CHD_Tracks(void) { return ntracks; }
int CHD_Track_Start(int i) { return trk[i].start; }
int CHD_Track_End(int i) { return trk[i].end; }
int CHD_Track_Audio(int i) { return trk[i].audio; }
int CHD_TOC_End(void) { return toc_end; }

static const unsigned char *frame_at(long long ofs)
{
	int n;
	if (!chd || ofs < 0)
		return NULL;
	n = (int)(ofs / hunkbytes);
	if (n != hunknum)
	{
		if (chd_read(chd, n, hunk) != CHDERR_NONE)
		{
			hunknum = -1;
			return NULL;
		}
		hunknum = n;
	}
	return hunk + (ofs % hunkbytes);
}

int CHD_Read_Data(int lba, unsigned char *out)
{
	const unsigned char *f;
	if (lba < 0)
		lba = 0;
	if (lba >= trk[0].end)
		lba = trk[0].end - 1;
	f = frame_at(trk[0].offset + (long long)lba * FRAME);
	if (!f)
	{
		memset(out, 0, 2048);
		return -1;
	}
	memcpy(out, f + (data_size == 2048 ? 0 : 16), 2048);	/* RAW: skip the 16-byte sync/header */
	return 0;
}

int CHD_Read_Audio(int i, int lba, unsigned char *out)
{
	const unsigned char *f;
	int k;
	if (i <= 0 || i >= ntracks || lba < trk[i].start || lba >= trk[i].end ||
		!(f = frame_at(trk[i].offset + (long long)lba * FRAME)))
	{
		memset(out, 0, 2352);	/* pause / out of the track: silence */
		return -1;
	}
	for (k = 0; k < 2352; k += 2)	/* big-endian 16-bit samples */
	{
		out[k] = f[k + 1];
		out[k + 1] = f[k];
	}
	return 0;
}

int CHD_Header(unsigned char *out, int len)
{
	const unsigned char *f = frame_at(trk[0].offset);
	if (!f)
		return -1;
	memcpy(out, f + (data_size == 2048 ? 0 : 16) + 0x100, len);
	return 0;
}

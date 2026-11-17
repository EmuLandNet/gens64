/* gens64: YX5200-24SS MP3 player on the KAISER WAVE cartridge board */
#ifndef GENS64_YX5200_H
#define GENS64_YX5200_H

#ifdef __cplusplus
extern "C" {
#endif

extern int YX5200_On;	/* the loaded ROM has the player (set by YX5200_Init): 1 Death & Lead, 2 Four Winds */

/* after a Genesis ROM is loaded (byteswapped ROM, Rom_Size set) */
void YX5200_Init(void);
/* console reset: player reset, playback stopped */
void YX5200_Reset(void);
/* /TIME write to an even address: bit 0 of the data is the player's serial RX line */
void YX5200_Write(unsigned int bit);
/* mix the music into the frame's sound (buf NULL: sound off, the track still advances) */
void YX5200_Update(int **buf, int length);
/* savestate: 11 bytes ('Y' + state) in the Genesis block padding */
#define YX5200_STATE_SIZE 11
void YX5200_Save(unsigned char *data);
void YX5200_Load(const unsigned char *data);	/* data NULL: the state has no 'Y' block */
/* ROM closed */
void YX5200_Close(void);

#ifdef __cplusplus
}
#endif

#endif

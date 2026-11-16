#include "shared.h"
#include <stdarg.h>
t_config config;
char GG_ROM[256], AR_ROM[256], SK_ROM[256], SK_UPMEM[256], GG_BIOS[256], MD_BIOS[256], CD_BIOS_EU[256], CD_BIOS_US[256], CD_BIOS_JP[256], MS_BIOS_US[256], MS_BIOS_EU[256], MS_BIOS_JP[256];
char g_rom_dir[256];
static void logf_(enum retro_log_level l, const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
retro_log_printf_t log_cb = logf_;
FILE *ref_trace; unsigned ref_frame;
static uint32_t bitmap_data_[720 * 576];
static int16 soundbuf[4096 * 2];
unsigned int pad_state;
void osd_input_update(void) { input.pad[0] = pad_state; }
int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
  FILE *f = fopen(filename, "rb"); int n;
  if (!f) return 0;
  n = fread(buffer, 1, maxsize, f); fclose(f);
  if (extension) { const char *e = strrchr(filename, '.'); strncpy(extension, e ? e + 1 : "", 3); extension[3] = 0; }
  return n;
}
void ROMCheatUpdate(void) {}
int main(int argc, char **argv)
{
  int frames = atoi(argv[2]), i;
  const char *shots = argc > 3 ? argv[3] : NULL;
  memset(&config, 0, sizeof(config));
  config.psg_preamp = 150; config.fm_preamp = 100; config.hq_fm = 1; config.hq_psg = 1; config.filter = 1; config.lp_range = 0x9999;
  config.low_freq = 880; config.high_freq = 5000; config.lg = config.mg = config.hg = 100; config.ym2612 = YM2612_DISCRETE; config.ym2413 = 2;
  config.addr_error = 1; config.add_on = HW_ADDON_AUTO; config.enhanced_vscroll_limit = 8;
#ifdef USE_PER_SOUND_CHANNELS_CONFIG
  for (i = 0; i < 4; i++) config.psg_ch_volumes[i] = 100; for (i = 0; i < 6; i++) config.md_ch_volumes[i] = 100; for (i = 0; i < 9; i++) config.sms_fm_ch_volumes[i] = 100;
#endif
  input.system[0] = SYSTEM_GAMEPAD; input.system[1] = SYSTEM_GAMEPAD;
  for (i = 0; i < MAX_INPUTS; i++) config.input[i].padtype = DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B;
  memset(&bitmap, 0, sizeof(bitmap)); bitmap.width = 720; bitmap.height = 576; bitmap.pitch = 720 * 4; bitmap.data = (uint8_t *)bitmap_data_;
  strcpy(g_rom_dir, "/nonexistent");
  ref_trace = fopen("ref_trace.txt", "w");
  if (load_rom(argv[1]) <= 0) { fprintf(stderr, "load failed\n"); return 1; }
  audio_init(48000, 0);
  system_init();
  system_reset();
  for (ref_frame = 0; ref_frame < (unsigned)frames; ref_frame++)
  {
    pad_state = 0;
    if (getenv("PRESS_START")) { unsigned p = atoi(getenv("PRESS_START")); if (ref_frame >= p && (ref_frame - p) % 120 < 6) pad_state = INPUT_START; }
    if (getenv("PRESS_AT")) { char b[256], *t; strcpy(b, getenv("PRESS_AT")); for (t = strtok(b, ","); t; t = strtok(NULL, ",")) { unsigned p = atoi(t); if (ref_frame >= p && ref_frame < p + 12) pad_state = INPUT_START; } }
    system_frame_gen(0);
    audio_update(soundbuf);
    if (ref_frame % 30 == 29) fprintf(ref_trace, "%u frame pc=%06X\n", ref_frame + 1, m68k_get_reg(M68K_REG_PC));
    if (shots && (ref_frame + 1) % 300 == 0)
    {
      char n[256]; FILE *o; int x, y;
      sprintf(n, "%s_%05u.ppm", shots, ref_frame + 1);
      o = fopen(n, "wb"); fprintf(o, "P6 %d %d 255\n", bitmap.viewport.w, bitmap.viewport.h);
      for (y = 0; y < bitmap.viewport.h; y++) for (x = 0; x < bitmap.viewport.w; x++) { uint32_t c = bitmap_data_[(y + bitmap.viewport.y) * 720 + x + bitmap.viewport.x]; fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
      fclose(o);
    }
  }
  fclose(ref_trace);
  return 0;
}
#include <zlib.h>
uint8_t cart_size;

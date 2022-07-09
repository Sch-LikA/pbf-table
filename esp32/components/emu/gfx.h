//Bindings for graphics output of pbf emu
/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */

#include <stdint.h>

#define INPUT_SPRING 1
#define INPUT_LFLIP 2
#define INPUT_RFLIP 3
#define INPUT_TILT 4
#define INPUT_F1 5
#define INPUT_F2 6
#define INPUT_F3 7
#define INPUT_F4 8
#define INPUT_TEST 9
#define INPUT_RELEASE 0x80
#define INPUT_RAWSCANCODE 0x100

void gfx_init();
int gfx_get_key();
int gfx_get_plunger();
void gfx_show(uint8_t *buf, uint32_t *pal, int h, int w, int scroll);
int gfx_frame_done();

typedef void(*audio_cb_t)(void* userdata, uint8_t* stream, int len);
void audio_init(int samprate, audio_cb_t cb);
void audio_lock();
void audio_unlock();
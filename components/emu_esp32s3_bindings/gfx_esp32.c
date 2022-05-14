#include <stdio.h>
#include <esp_log.h>
#include "gfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lcd.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cpu_addr_space.h"
#include "io.h"

static SemaphoreHandle_t rdy_sem;

void gfxinit_task(void *arg) {
	io_init();
	lcd_init();

	xSemaphoreGive(rdy_sem);
	vTaskDelete(NULL);
}



void gfx_init() {
	rdy_sem=xSemaphoreCreateBinary();
	//We really want the gfx to be on the other core.
	xTaskCreatePinnedToCore(&gfxinit_task, "gfx", 16*1024, NULL, 2, NULL, 1);
	xSemaphoreTake(rdy_sem, portMAX_DELAY);
	vSemaphoreDelete(rdy_sem);
}

int old_btns;

int gfx_get_key() {
	const int keys[]={INPUT_LFLIP,INPUT_RFLIP,INPUT_F1,INPUT_SPRING};
	int new_btns=io_get_btn_bitmap();
	for (int i=0; i<4; i++) {
		if ((new_btns^old_btns)&(1<<i)) {
			int r=keys[i];
			if (old_btns&(1<<i)) r|=INPUT_RELEASE;
			old_btns=(old_btns^(1<<i));
			return r;
		}
	}
	return 0;
}



uint64_t last_frame;

uint64_t fps_ts;
int fps_frames;
int skipped;

int current_frame=0;

int gfx_frame_done() {
	return (lcd_get_frame()!=current_frame);
}

#define FRAME_TIME (1000000/60)

void gfx_show(uint8_t *buf, uint32_t *pal, int h, int w, int scroll) {
	uint64_t ts=esp_timer_get_time();
	if (ts-fps_ts > (1000000*5)) {
		printf("%d fps\n", fps_frames/5);
		printf("Free internal: %d\n", heap_caps_get_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL));
		cpu_addr_dump_hitctr();
		fps_frames=0;
		fps_ts=ts;
	}
	fps_frames++;

	lcd_show(buf, pal, h, w, scroll-32);
	current_frame=lcd_get_frame();
	last_frame=ts;
}

int gfx_get_plunger() {
	return io_get_plunger();
}


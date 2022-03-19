#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "cpu.h"
#include <string.h>
#include "gfx.h"
#include "cpu_addr_space.h"
#include "ibxm/ibxm.h"
#include "hexdump.h"

//Note: this code assumes a little-endian host machine. It messes up when run on a big-endian one.

#define SAMP_RATE 44100
struct module *music_module;
struct replay *music_replay;
int *music_mixbuf;
int music_mixbuf_len;
int music_mixbuf_left;

int key_pressed=0;

//from http://www.delorie.com/djgpp/doc/exe/
typedef struct {
  uint16_t signature; /* == 0x5a4D */
  uint16_t bytes_in_last_block;
  uint16_t blocks_in_file;
  uint16_t num_relocs;
  uint16_t header_paragraphs;
  uint16_t min_extra_paragraphs;
  uint16_t max_extra_paragraphs;
  uint16_t ss;
  uint16_t sp;
  uint16_t checksum;
  uint16_t ip;
  uint16_t cs;
  uint16_t reloc_table_offset;
  uint16_t overlay_number;
} mz_hdr_t;

typedef struct {
  uint16_t offset;
  uint16_t segment;
} mz_reloc_t;

#define REG_AX cpu.regs.wordregs[regax]
#define REG_BX cpu.regs.wordregs[regbx]
#define REG_CX cpu.regs.wordregs[regcx]
#define REG_DX cpu.regs.wordregs[regdx]
#define REG_BP cpu.regs.wordregs[regbp]
#define REG_DS cpu.segregs[regds]
#define REG_ES cpu.segregs[reges]

int load_mz(const char *exefile, int load_start_addr);

int do_trace=0;
#define DO_TRACE 0
#if DO_TRACE
typedef struct __attribute__((packed)) {
	uint16_t cs;
	uint16_t ip;
	uint16_t ax;
	uint16_t null;
} trace_t;
#define TRACECT (3*1024*1024*2ULL)
trace_t *tracemem=NULL;
int tracepos=0;
//diff at 01B2 65A0
void exec_cpu(int count) {
	if (!tracemem) tracemem=calloc(TRACECT, sizeof(trace_t));
	if (!do_trace) {
		exec86(count);
		return;
	}
	for (int i=0; i<count; i++) {
		exec86(1);
		tracemem[tracepos].cs=cpu.segregs[regcs];
		tracemem[tracepos].ip=cpu.ip;
		tracemem[tracepos].ax=REG_AX;
		tracemem[tracepos].null=REG_DX;
		tracepos++;
		if (tracepos>=TRACECT) {
			FILE *f=fopen("tracelog.bin", "w");
			if (!f) {
				perror("tracelog");
			} else {
				fwrite(tracemem, sizeof(trace_t), TRACECT, f);
				fclose(f);
			}
			printf("Trace complete.\n");
			exit(0);
		}
	}
}
#else
#define exec_cpu(count) exec86(count)
#endif


void dump_regs() {
	printf("AX %04X  SI %04X  ES %04X\n", cpu.regs.wordregs[regax], cpu.regs.wordregs[regsi], cpu.segregs[reges]);
	printf("BX %04X  DI %04X  CS %04X\n", cpu.regs.wordregs[regbx], cpu.regs.wordregs[regdi], cpu.segregs[regcs]);
	printf("CX %04X  SP %04X  SS %04X\n", cpu.regs.wordregs[regcx], cpu.regs.wordregs[regsp], cpu.segregs[regss]);
	printf("DX %04X  BP %04X  DS %04X\n", cpu.regs.wordregs[regdx], cpu.regs.wordregs[regbp], cpu.segregs[regds]);
}

void dump_caller() {
	int sp_addr=cpu.regs.wordregs[regsp]+cpu.segregs[regss]*0x10;
	int seg=cpu_addr_space_read8(sp_addr+2)+(cpu_addr_space_read8(sp_addr+3)<<8);
	int off=cpu_addr_space_read8(sp_addr)+(cpu_addr_space_read8(sp_addr+1)<<8);
	printf("Caller: %04X:%04X\n", seg, off);
}

void timing() {
}

uint8_t vram[256*1024];
uint32_t pal[255];
int vga_seq_addr;
int vga_mask;
int vga_pal_idx;
int vga_crtc_addr;
int vga_hor=200, vga_ver=320;
int vga_startaddr;
uint8_t vga_gcregs[8];
int vga_gcindex;
uint8_t vga_latch[4];
int in_retrace;

static int keycode[]={
	0,
	0x50, //down
	0x2A, //lshift
	0x36, //rshift
	0x39, //space
	0x3b,
	0x3c,
	0x3d,
	0x3e,
	0, //test key
};

uint8_t portin(uint16_t port) {
	if (port==0x60) {
		int kc=keycode[key_pressed&0x7F];
		if (key_pressed&INPUT_RELEASE) kc|=0x80;
		key_pressed=0;
		printf("Keycode %x\n", kc);
		return kc;
	} else if (port==0x61) {
		return 0; //nothing wrong
	} else if (port==0x3CF) {
		return vga_gcregs[vga_gcindex];
	} else if (port==0x3C5) {
		//ignore, there's very little we use in there anyway
	} else if (port==0x3DA) {
//		printf("In retrace: %d\n", in_retrace);
		return in_retrace?9:0;
	} else {
		printf("Port read 0x%X\n", port);
		static int n=0;
		return n++;
	}
	return 0;
}

uint8_t portin16(uint16_t port) {
	printf("16-bit port read 0x%X\n", port);
}


void portout(uint16_t port, uint8_t val) {
	if (port==0x3c4) {
		vga_seq_addr=val&3;
	} else if (port==0x3c5) {
		if (vga_seq_addr==2) {
			vga_mask=val&0xf;
		}
	} else if (port==0x3c8) {
		vga_pal_idx=val*4;
	} else if (port==0x3c9) {
		int col=vga_pal_idx>>2;
		int attr=vga_pal_idx&3;
		val<<=2;  //vga palette is 6 bit per color
		if (attr==0) pal[col]=(pal[col]&0x00ffff)|(val<<16);
		if (attr==1) pal[col]=(pal[col]&0xff00ff)|(val<<8);
		if (attr==2) pal[col]=(pal[col]&0xffff00)|(val<<0);
		if (attr==2) vga_pal_idx+=2; else vga_pal_idx++;
	} else if (port==0x3ce) {
		vga_gcindex=val&7;
	} else if (port==0x3cf) {
//		if (val!=vga_gcregs[vga_gcindex]) {
//			printf("0x3C7 idx %d -> %02X\n", vga_gcindex, val);
//		}
		vga_gcregs[vga_gcindex]=val;
	} else if (port==0x3D4) {
		vga_seq_addr=val&31;
	} else if (port==0x3D5) {
		if (vga_seq_addr==1) {
			vga_hor=(val+1)*4;
		} else if (vga_seq_addr==0xC) {
			vga_startaddr=(vga_startaddr&0xff)|(val<<8);
		} else if (vga_seq_addr==0xD) {
			vga_startaddr=(vga_startaddr&0xff00)|(val);
		} else if (vga_seq_addr==0x13) {
			vga_ver=val*8;
		} else {
			printf("port 3d5 idx 0x%X val 0x%X\n", vga_seq_addr, val);
		}
	} else if (port==0x61) {
		//Keyboard port; ignore
	} else {
		printf("Port 0x%X val 0x%02X\n", port, val);
	}
}

void portout16(uint16_t port, uint16_t val) {
//	printf("Port 0x%X val 0x%04X\n", port, val);
	portout(port, val&0xff);
	portout(port+1, val>>8);
}

void vga_mem_write(int addr, uint8_t data, mem_chunk_t *ch) {
	//Pinball Fantasies uses Mode X.
	int vaddr=addr-0xA0000;
	if ((vga_gcregs[5]&3)==0) {
		if (vga_mask&1) vram[vaddr*4+0]=data;
		if (vga_mask&2) vram[vaddr*4+1]=data;
		if (vga_mask&4) vram[vaddr*4+2]=data;
		if (vga_mask&8) vram[vaddr*4+3]=data;
	} else if ((vga_gcregs[5]&3)==1) {
		for (int i=0; i<4; i++) vram[vaddr*4+i]=vga_latch[i];
	}
}

uint8_t vga_mem_read(int addr, mem_chunk_t *ch) {
	int vaddr=addr-0xA0000;
	for (int i=0; i<4; i++) vga_latch[i]=vram[vaddr*4+i];
	//note: what if multiple bits in mask are enabled?
	if (vga_mask&1) return vram[vaddr*4+0];
	if (vga_mask&2) return vram[vaddr*4+1];
	if (vga_mask&4) return vram[vaddr*4+2];
	if (vga_mask&8) return vram[vaddr*4+3];
}

void init_vga_mem() {
	cpu_addr_space_map_cb(0xa0000, 0x10000, vga_mem_write, vga_mem_read, NULL);
}

void hook_interrupt_call(uint8_t intr);

void intr_table_writefn(int addr, uint8_t data, mem_chunk_t *ch) {
	//this is supposed to be rom
	printf("Aiee, write to rom segment? (Adr %05X, data %02X)\n", addr, data);
	exit(1);
}

uint8_t intr_table_readfn(int addr, mem_chunk_t *ch) {
	int intr=(addr-0xf0000);
	if (intr<256) hook_interrupt_call(intr);
	return 0xcf; //IRET
}

void init_intr_table() {
	//Set up interrupt table so each interrupt i vectors to address (F000:i)
	for (int i=0; i<256; i++) {
		cpu_addr_space_write8(i*4+2, 0x00);
		cpu_addr_space_write8(i*4+3, 0xf0);
		cpu_addr_space_write8(i*4+0, i);
		cpu_addr_space_write8(i*4+1, 0);
	}
	//Set up some address space in ROM that the interrupt table vectors point at by default.
	//We take a read from the first 256 addresses as an interrupt request for that irq.
	cpu_addr_space_map_cb(0xf0000, 2048, intr_table_writefn, intr_table_readfn, NULL);
}

int cb_raster_seg=0, cb_raster_off=0;
int cb_blank_seg=0, cb_blank_off=0;
int cb_jingle_seg=0, cb_jingle_off=0;

int mouse_x=0;
int mouse_y=0;
int mouse_btn=0;
int mouse_down=0;

int dos_program_exited=0;

void hook_interrupt_call(uint8_t intr) {
//	printf("Intr 0x%X\n", intr);
//	dump_regs();
	if (intr==0x21 && (REG_AX>>8)==0x4A) {
		printf("Resize block\n");
		//resize block; always allow as there's nothing in memory anyway
		REG_AX=0;
		cpu.cf=0;
	} else if (intr==0x21 && (REG_AX>>8)==0x9) {
		printf("Message\n");
	} else if (intr==0x21 && (REG_AX>>8)==0xE) {
		printf("Set default drive %c\n", 'A'+(REG_DX&0xff));
		REG_AX=0x0E00+26;
	} else if (intr==0x21 && (REG_AX>>8)==0x31) {
		printf("Terminate and Stay Resident\n");
		dos_program_exited=1;
	} else if (intr==0x21 && (REG_AX>>8)==0x3B) {
		printf("Set current dir\n");
		cpu.cf=0;
	} else if (intr==0x21 && (REG_AX>>8)==0x3D) {
		printf("Open file\n");
		cpu.cf=1;
	} else if (intr==0x21 && (REG_AX>>8)==0x3E) {
		printf("Close file\n");
		cpu.cf=1;
	} else if (intr==0x21 && (REG_AX>>8)==0x3F) {
		printf("Read from file\n");
		cpu.cf=1;
	} else if (intr==0x21 && (REG_AX>>8)==0x4B) {
		printf("Exec program\n");
		dos_program_exited=1;
		cpu.cf=1;
	} else if (intr==0x10 && (REG_AX>>8)==0) {
		printf("Set video mode %x\n", REG_AX&0xff);
	} else if (intr==0x65) {
		//internal keyhandler: START.ASM:363
		//ax=0: first_time_ask (ret ah=first_time, bl=bannumber)
		//ax=0xffff: save bl to bannumber
		//ax=0x100: LOKALA TOGGLAREN => RESIDENTA TOGGLAREN
		//ax=0x200: RESIDENTA TOGGLAREN => LOKALA TOGGLAREN
		//ax00x12: text input
		//others: simply return scan code without side effects
		//Returns scan code in ax
		REG_AX=0;
		printf("Key handler\n");
		dump_regs();
	} else if (intr==0x33) {
		//Mouse interrupt
		if ((REG_AX&0xff)==0) {
			REG_AX=0xFFFF; //installed
			REG_BX=2; //2 buttons
		} else if ((REG_AX&0xff)==8) {
			printf("Mouse: def vert cursor range %d to %d\n", REG_CX, REG_DX);
		} else if ((REG_AX&0xff)==0xf) {
			printf("Mouse: def mickey/pixel hor %d vert %d\n", REG_CX, REG_DX);
		} else if ((REG_AX&0xff)==3) {
			if (mouse_down) mouse_y-=5;
			REG_BX=mouse_btn;
			REG_CX=mouse_x; //col
			REG_DX=mouse_y; //row
//			printf("read mouse %d,%d\n", mouse_x, mouse_y);
			mouse_btn=0;
		} else if ((REG_AX&0xff)==4) {
			//position cursor cx:dx
			mouse_x=REG_CX;
			mouse_y=REG_DX;
//			printf("pos mouse %d,%d\n", mouse_x, mouse_y);
		}
	} else if (intr==0x66 && (REG_AX&0xff)==8) {
//		printf("audio: fill music buffer\n");
		REG_AX=0;  //Note: this seems to indicate *something*... but I have no clue how this affects the code? Super-odd.
//		dump_caller();
	} else if (intr==0x66 && (REG_AX&0xff)==16) { 
		//AL=FORCE POSITION, BX=THE POSITION ret: AL=OLD POSITION
		printf("audio: play jingle in bx: 0x%X\n", REG_BX);
		audio_lock();
		int old_pos=replay_get_sequence_pos(music_replay);
		replay_set_sequence_pos(music_replay, REG_BX);
		audio_unlock();
		REG_AX=old_pos;
	} else if (intr==0x66 && (REG_AX&0xff)==21) {
		printf("audio: get driver caps, AL returns bitfield? Nosound.drv returns 0xa.\n");
		//Note: bit 3 is tested for nosound
//		REG_AX=10; //nosound driver returns this
		REG_AX=0; //nosound driver returns this
	} else if (intr==0x66 && (REG_AX&0xff)==22) {
		printf("audio: ?get amount of data in buffers into dx.ax?\n");
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==17) {
		printf("audio: play sound effect in cl,bl,dl at volume bh\n");
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==18) {
		int adr=(REG_DS*0x10)+REG_DX;
		char name[64]={0};
		for (int i=0; i<64; i++) name[i]=cpu_addr_space_read8(adr+i);
		printf("audio: load mod in ds:dx: %s\n", name);
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==4) {
		printf("audio: play module ?idx is in bx: 0x%X? at rate cx %d (0 if default)\n", REG_BX, REG_CX);
//		audio_lock();
//		replay_set_sequence_pos(music_replay, REG_BX-1);
//		audio_unlock();
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==15) {
		printf("audio: stop play\n");
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==6) {
		printf("set vol in cx?\n");
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==0) {
		printf("deinit sound device\n");
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==11) {
		//note: far call. BL also is... something.
		printf("init vblank interrupt to es:dx\n");
		cb_blank_seg=REG_ES;
		cb_blank_off=REG_DX;
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==12) {
		//note: far call
		printf("init raster interrupt to es:dx, raster in bl or cx?\n");
		cb_raster_seg=REG_ES;
		cb_raster_off=REG_DX;
		REG_AX=0;
	} else if (intr==0x66 && (REG_AX&0xff)==19) {
		printf("init jingle handler to es:dx\n");
		cb_jingle_seg=REG_ES;
		cb_jingle_off=REG_DX;
		REG_AX=0;
	} else {
		printf("Unhandled interrupt 0x%X\n", intr);
		dump_regs();
	}
}

int cpu_hlt_handler() {
	printf("CPU halted!\n");
	exit(1);
}

//Note: Assumes the PSP starts 256 bytes before load_start_addr
//Returns amount of data loaded.
int load_mz(const char *exefile, int load_start_addr) {
	FILE *f=fopen(exefile, "r");
	if (!f) {
		perror(exefile);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	int size=ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *exe=malloc(size);
	fread(exe, 1, size, f);
	fclose(f);
	
	mz_hdr_t *hdr=(mz_hdr_t*)exe;
	if (hdr->signature!=0x5a4d) {
		printf("Not an exe file!\n");
		exit(1);
	}
	int exe_data_start = hdr->header_paragraphs*16;
	printf("mz: data starts at %d\n", exe_data_start);
	cpu_addr_space_map_cow(&exe[exe_data_start], load_start_addr, size-exe_data_start);

	mz_reloc_t *relocs=(mz_reloc_t*)&exe[hdr->reloc_table_offset];
	for (int i=0; i<hdr->num_relocs; i++) {
		int adr=relocs[i].segment*0x10+relocs[i].offset;
		int w=cpu_addr_space_read8(load_start_addr+adr);
		w|=cpu_addr_space_read8(load_start_addr+adr+1)<<8;
		w=w+(load_start_addr/0x10);
		cpu_addr_space_write8(load_start_addr+adr, w&0xff);
		cpu_addr_space_write8(load_start_addr+adr+1, w>>8);
//		printf("Fixup at %x\n", load_start_addr+adr);
	}
	cpu.segregs[regds]=(load_start_addr-256)/0x10;
	cpu.segregs[reges]=(load_start_addr-256)/0x10;
	cpu.segregs[regcs]=(load_start_addr/0x10)+hdr->cs;
	cpu.segregs[regss]=(load_start_addr/0x10)+hdr->ss;
	cpu.regs.wordregs[regax]=0; //should be related to psp, but we don't emu that.
	cpu.regs.wordregs[regsp]=hdr->sp;
	cpu.ip=hdr->ip;
	printf("Exe load done. CPU is set up to start at %04X:%04X (%06X).\n", cpu.segregs[regcs], cpu.ip, cpu.segregs[regcs]*0x10+cpu.ip);
	for (int i=0; i<32; i++) printf("%02X ", cpu_addr_space_read8(cpu.segregs[regcs]*0x10+cpu.ip+i));
	printf("\n");
	return size-exe_data_start;
}

uint16_t force_callback(int seg, int off, int axval) {
	uint16_t ret;
	struct cpu cpu_backup;
	//Save existing registers
	cpu_backup=cpu;
	//set cs:ip to address of callback
	cpu.segregs[regcs]=seg;
	cpu.ip=off;
	cpu.regs.wordregs[regax]=axval;
	//increase sp by four, as if there was a call that pushed the address of the calling function
	//on the stack
	cpu.regs.wordregs[regsp]=cpu.regs.wordregs[regsp]-4;
	int stack_addr=cpu.segregs[regss]*0x10+cpu.regs.wordregs[regsp];
	cpu_addr_space_write8(stack_addr, 0x00);
	cpu_addr_space_write8(stack_addr+1, 0x00);
	cpu_addr_space_write8(stack_addr+2, 0x00);
	cpu_addr_space_write8(stack_addr+3, 0x00);

	//Run the callback until we can see the return address has been popped; this
	//indicates a ret happened.
//	while (cpu.regs.wordregs[regsp]<=cpu_backup.regs.wordregs[regsp]) exec_cpu(1);
	while (cpu.segregs[regcs]!=0 && cpu.ip!=0) exec_cpu(1);
	ret=cpu.regs.wordregs[regax];
	//Restore registers
	cpu=cpu_backup;
	return ret;
}

void music_init(const char *fname) {
	FILE *f=fopen(fname, "r");
	if (!f) {
		perror(fname);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	int len=ftell(f);
	fseek(f, 0, SEEK_SET);
	struct data mod_data;
	mod_data.buffer=malloc(len);
	fread(mod_data.buffer, 1, len, f);
	mod_data.length=len;
	fclose(f);
	char msg[64];
	music_module=module_load(&mod_data, msg);
	if (music_module==NULL) {
		printf("%s: %s\n", fname, msg);
		exit(1);
	}
	music_replay=new_replay(music_module, SAMP_RATE, 0);
	music_mixbuf=malloc(calculate_mix_buf_len(SAMP_RATE)*4);
	music_mixbuf_left=0;
	music_mixbuf_len=0;
}

static void fill_stream_buf(int16_t *stream, int *mixbuf, int len) {
	for (int i=0; i<len; i++) {
		int v=mixbuf[i];
		if (v<-32768) v=-32768;
		if (v>32767) v=32767;
		stream[i]=v;
	}
}

static void audio_cb(void* userdata, uint8_t* streambytes, int len) {
	uint16_t *stream=(uint16_t*)streambytes;
	len=len/2; //because bytes -> words
	int pos=0;
	if (len==0) return;
	if (music_mixbuf_left!=0) {
		pos=music_mixbuf_left;
		fill_stream_buf(stream, &music_mixbuf[music_mixbuf_len-music_mixbuf_left], music_mixbuf_left);
	}
	music_mixbuf_left=0;
	while (pos!=len) {
		music_mixbuf_len=replay_get_audio(music_replay, (int*)music_mixbuf);
		int cplen=music_mixbuf_len;
		if (pos+cplen>len) {
			cplen=len-pos;
			music_mixbuf_left=music_mixbuf_len-cplen;
		}
		fill_stream_buf(&stream[pos], music_mixbuf, cplen);
		pos+=cplen;
	}
}

int dos_timer=0;
void inc_dos_timer() {
	dos_timer++;
	cpu_addr_space_write8(0x46c, dos_timer);
	cpu_addr_space_write8(0x46c, dos_timer>>8);
}


int main(int argc, char** argv) {
	cpu_addr_space_init();
	init_intr_table();
	init_vga_mem();
	gfx_init();
	music_init("../data/TABLE1.MOD");
	init_audio(SAMP_RATE, audio_cb);

	cpu_addr_space_write8(0x463, 0xd4);
	cpu_addr_space_write8(0x464, 0x3);


#if 1
//	load_mz("../data_dos/PF.EXE", (640-20)*1024);
//	while(!dos_program_exited) exec_cpu(1);
//	dos_program_exited=0;
	load_mz("../data_dos/NOSOUND.SDR", (640-10)*1024);
	while(!dos_program_exited) exec_cpu(1);
	dos_program_exited=0;
#endif

#if 0
	load_mz("../data_dos/NOSOUND.SDR", 0x10000);
	while(!dos_program_exited) exec_cpu(1);
	cpu_addr_space_dump();
	cpu_addr_dump_file("nosound.dump", 0x10000, 4*1024);
	exit(0);
#endif

	load_mz("../data/TABLE1.PRG", 0x10000);
	
//	uint8_t watch_mem[256];
//	int watch_addr=0x1db6*0x10+0x2f00;

	int frame=0;
	while(1) {
		exec_cpu(10000);
		intcall86(8); //pit interrupt
		inc_dos_timer();
		if (cb_raster_seg!=0) force_callback(cb_raster_seg, cb_raster_off, 0);
		exec_cpu(10000);
		intcall86(8); //pit interrupt
		inc_dos_timer();
		in_retrace=1;
		if (cb_blank_seg!=0) force_callback(cb_blank_seg, cb_blank_off, 0);
		exec_cpu(10000);
		intcall86(8); //pit interrupt
		inc_dos_timer();
		in_retrace=0;
		frame++;
		if (frame==100) do_trace=1;
#ifdef DOTRACE
		if (frame=60) {
			while (cpu.ifl==0) exec_cpu(10);
			key_pressed=INPUT_F1;
			intcall86(9);
		}
#endif

//		printf("hor %d ver %d start addr %d\n", vga_hor, vga_ver, vga_startaddr);
//		cpu_addr_space_dump();
		gfx_show(vram, pal, vga_ver, 607);
		if (has_looped(music_replay)) {
			printf("Music has looped. Doing jingle callback...\n");
			uint16_t ret=force_callback(cb_jingle_seg, cb_jingle_off, 0);
			audio_lock();
			replay_set_sequence_pos(music_replay, ret&0xff);
			printf("Jingle callback returned 0x%X\n", ret);
			audio_unlock();
		}

		int key;
		while ((key=gfx_get_key())>0) {
			if (key==INPUT_SPRING) {
				mouse_down=1;
			} else if (key==(INPUT_SPRING|INPUT_RELEASE)) {
				mouse_down=0;
				mouse_btn=1;
			} else if (key==INPUT_TEST) {
				uint16_t ret=force_callback(cb_jingle_seg, cb_jingle_off, 0);
				audio_lock();
				replay_set_sequence_pos(music_replay, ret&0xff);
				audio_unlock();
				printf("Jingle callback returned 0x%X\n", ret);
			} else {
				//wait till interrupts are enabled (not in int)
				while (cpu.ifl==0) exec_cpu(10);
				key_pressed=key;
				intcall86(9);
			}
		}
	}
	return 0;
}
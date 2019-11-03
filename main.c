#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mach_defines.h"
#include "sdk.h"
#include "gfx_load.h"
#include "cache.h"

//The bgnd.png image got linked into the binary of this app, and these two chars are the first
//and one past the last byte of it.
extern char _binary_badgetris_bgnd_png_start;
extern char _binary_badgetris_bgnd_png_end;
extern char _binary_flappy_tileset_png_start;
extern char _binary_flappy_tileset_png_end;

//Pointer to the framebuffer memory.
uint8_t *fbmem;
static FILE *console;

#define FB_WIDTH 512
#define FB_HEIGHT 320
#define FB_PAL_OFFSET 256

extern volatile uint32_t MISC[];
#define MISC_REG(i) MISC[(i)/4]
extern volatile uint32_t GFXREG[];
#define GFX_REG(i) GFXREG[(i)/4]

uint32_t *GFXSPRITES = (uint32_t *)0x5000C000;

//Used to debounce buttons
#define BUTTON_READ_DELAY		15

//Borrowed this from lcd.c until a better solution comes along :/
static void __INEFFICIENT_delay(int n) {
	for (int i=0; i<n; i++) {
		for (volatile int t=0; t<(1<<11); t++);
	}
}

//Wait until all buttons are released
static inline void __button_wait_for_press() {
	while (MISC_REG(MISC_BTN_REG) == 0);
}

//Wait until all buttons are released
static inline void __button_wait_for_release() {
	while (MISC_REG(MISC_BTN_REG));
}

static inline void __sprite_set(int index, int x, int y, int size_x, int size_y, int tile_index, int palstart) {
	x+=64;
	y+=64;
	GFXSPRITES[index*2]=(y<<16)|x;
	GFXSPRITES[index*2+1]=size_x|(size_y<<8)|(tile_index<<16)|((palstart/4)<<25);
}

//Helper function to set a tile on layer a
static inline void __tile_a_set(uint8_t x, uint8_t y, uint32_t index) {
	GFXTILEMAPA[y*GFX_TILEMAP_W+x] = index;
}

//Helper function to set a tile on layer b
static inline void __tile_b_set(uint8_t x, uint8_t y, uint32_t index) {
	GFXTILEMAPB[y*GFX_TILEMAP_W+x] = index;
}

//Helper function to move tile layer 1
static inline void __tile_a_translate(int dx, int dy) {
	GFX_REG(GFX_TILEA_OFF)=(dy<<16)+(dx &0xffff);
}

//Helper function to move tile layer b
static inline void __tile_b_translate(int dx, int dy) {
	GFX_REG(GFX_TILEB_OFF)=(dy<<16)+(dx &0xffff);
}

uint32_t counter60hz(void) {
	return GFX_REG(GFX_VBLCTR_REG);
}

void main(int argc, char **argv) {
	//Allocate framebuffer memory
	fbmem=malloc(320*512/2);

	for (uint8_t i=0; i<16;i++) {
		MISC_REG(MISC_LED_REG)=(i<<i);
		__INEFFICIENT_delay(100);
	}


	//Set up the framebuffer address.
	GFX_REG(GFX_FBADDR_REG)=((uint32_t)fbmem)&0xFFFFFF;
	//We're going to use a pitch of 512 pixels, and the fb palette will start at 256.
	GFX_REG(GFX_FBPITCH_REG)=(FB_PAL_OFFSET<<GFX_FBPITCH_PAL_OFF)|(512<<GFX_FBPITCH_PITCH_OFF);
	//Blank out fb while we're loading stuff.
	GFX_REG(GFX_LAYEREN_REG)=0;

	//Load up the default tileset and font.
	//ToDo: loading pngs takes a long time... move over to pcx instead.
	printf("Loading tiles...\n");
	int gfx_tiles_err = gfx_load_tiles_mem(GFXTILES, &GFXPAL[0], &_binary_flappy_tileset_png_start, (&_binary_flappy_tileset_png_end-&_binary_flappy_tileset_png_start));
	printf("Tiles initialized err=%d\n", gfx_tiles_err);


	//The IPL leaves us with a tileset that has tile 0 to 127 map to ASCII characters, so we do not need to
	//load anything specific for this. In order to get some text out, we can use the /dev/console device
	//that will use these tiles to put text in a tilemap. It uses escape codes to do so, see
	//ipl/gloss/console_out.c for more info.
	//Note that without the setvbuf command, no characters would be printed until 1024 characters are
	//buffered.
	console=fopen("/dev/console", "w");
	setvbuf(console, NULL, _IOLBF, 1024); //make console line buffered
	if (console==NULL) {
		printf("Error opening console!\n");
	}

	//Now, use a library function to load the image into the framebuffer memory. This function will also set up the palette entries,
	//we tell it to start writing from entry 0.
	//PAL offset changes the colors that the 16-bit png maps to?
	gfx_load_fb_mem(fbmem, &GFXPAL[FB_PAL_OFFSET], 4, 512, &_binary_badgetris_bgnd_png_start, (&_binary_badgetris_bgnd_png_end-&_binary_badgetris_bgnd_png_start));

	//Flush the memory region to psram so the GFX hw can stream it from there.
	cache_flush(fbmem, fbmem+FB_WIDTH*FB_HEIGHT);

	//Copied from IPL not sure why yet
	GFX_REG(GFX_LAYEREN_REG)=GFX_LAYEREN_FB|GFX_LAYEREN_TILEB|GFX_LAYEREN_TILEA|GFX_LAYEREN_SPR;
	GFXPAL[FB_PAL_OFFSET+0x100]=0x00ff00ff; //Note: For some reason, the sprites use this as default bgnd. ToDo: fix this...
	GFXPAL[FB_PAL_OFFSET+0x1ff]=0x40ff00ff; //so it becomes this instead.

	//This makes sure not to read button still pressed from badge menu selection
	__button_wait_for_release();

	//Set map to tilemap B, clear tilemap, set attr to 0
	//Not sure yet what attr does, but tilemap be is important as it will have the effect of layering
	//on top of our scrolling game
	fprintf(console, "\0331M\033C\0330A\n");
	//Note that without the newline at the end, all printf's would stay in the buffer.


	//Clear both tilemaps
	memset(GFXTILEMAPA,0,0x4000);
	memset(GFXTILEMAPB,0,0x4000);
	//Clear sprites that IPL may have loaded
	memset(GFXSPRITES,0,0x4000);

	//The user can still see nothing of this graphics goodness, so let's re-enable the framebuffer and
	//tile layer A (the default layer for the console).
	//Normal FB enabled (vice 8 bit) because background is loaded into the framebuffer above in 4 bit mode.
	//TILEA is where text is printed by default
	 GFX_REG(GFX_LAYEREN_REG)=GFX_LAYEREN_FB|GFX_LAYEREN_TILEA|GFX_LAYEREN_TILEB|GFX_LAYEREN_SPR;

	/********************************************************************************
	 * Put your user code in there, return when it's time to exit back to bage menu *
	 * *****************************************************************************/
	uint8_t tilegrid_width = 30;
	uint8_t tilegrid_height = 20;

	uint8_t horizontal_index = 15;
	uint8_t vertical_index = 10;

	//Fill tile layer A with a patterened tile
	for (uint8_t y=0; y<tilegrid_height; y++) {
		for (uint8_t x=0; x<tilegrid_width; x++)
		{
			__tile_a_set(x,y,150);
		}
	}

	//Show a tile the user can move with the arrows
	__tile_b_set(horizontal_index,vertical_index,176);

	//We're using a timing trick to make sure we don't read one button press as a multiples
	uint32_t next_button_read_time = counter60hz() + BUTTON_READ_DELAY;

	//Print some text to the display. "\n" must be here or text will not be shown!
	//The "\033" is an escape character. Here we're using substition to move the cursor
	//to tile locations 8X and 18Y before writing our message.
	//This message is written to tile laye B and will be overwritten if you move the ball to that line
	fprintf(console,"\033%dX\033%dY%s",8,18,"Use the D-pad!\n");

	while(1)
	{
		//Watch for button input

		//Never read buttons more than once every 250ms
		if (counter60hz() > next_button_read_time) {
			next_button_read_time = counter60hz() + BUTTON_READ_DELAY;

			
			//Check each button and react accordingly
			if ((MISC_REG(MISC_BTN_REG) & (BUTTON_UP))) {
				if (vertical_index > 0) {
					__tile_b_set(horizontal_index,vertical_index,0);
					--vertical_index;
					__tile_b_set(horizontal_index,vertical_index,176);
				}
			}
			if ((MISC_REG(MISC_BTN_REG) & BUTTON_LEFT)) {
				if (horizontal_index > 0) {
					__tile_b_set(horizontal_index,vertical_index,0);
					--horizontal_index;
					__tile_b_set(horizontal_index,vertical_index,176);
				}
			}
			if ((MISC_REG(MISC_BTN_REG) & BUTTON_RIGHT)) {
				if (horizontal_index < tilegrid_width-1) {
					__tile_b_set(horizontal_index,vertical_index,0);
					++horizontal_index;
					__tile_b_set(horizontal_index,vertical_index,176);
				}
			}
			if ((MISC_REG(MISC_BTN_REG) & BUTTON_DOWN)) {
				if (vertical_index < tilegrid_height-1) {
					__tile_b_set(horizontal_index,vertical_index,0);
					++vertical_index;
					__tile_b_set(horizontal_index,vertical_index,176);
				}
			}
			if ((MISC_REG(MISC_BTN_REG) & BUTTON_SELECT)) {	return;	}
		}
	}
}

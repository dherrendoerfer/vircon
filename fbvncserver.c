/*
 * fbvncserver.c
 * This file is part of the vircon virtual console driver and service.
 * Copyright (C) 2015 Dirk Herrendoerfer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This project is an adaptation of the original fbvncserver.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/keyboard.h>
#include <linux/input.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

/*****************************************************************************/

static char FB_DEVICE[256] = "/dev/fb0";
static char KBD_DEVICE[256] = "auto";
static char TOUCH_DEVICE[256] = "auto";
static struct fb_var_screeninfo scrinfo;
static struct fb_var_screeninfo scrinfo_m;
static int fbfd = -1;
static int kbdfd = -1;
static int touchfd = -1;
static unsigned short int *fbmmap = MAP_FAILED;
static size_t fbmmap_size;
static unsigned short int *vncbuf;
static unsigned short int *fbbuf;
static int mpid = 0;

static int shutdown_set = 0;

#define VNC_PORT 5901

static char pidfile[]="/var/run/fbvncserver.pid";

static rfbScreenInfoPtr vncscr;
in_addr_t vncaddr;

static int xmin, xmax;
static int ymin, ymax;

/* Defines if the mouse should move softly with many updates or if the
 * mouse acts like a touch device only moving upon each button update */
static int mousemode = 0;

/* No idea, just copied from fbvncserver as part of the frame differerencing
 * algorithm.  I will probably be later rewriting all of this. */
static struct varblock_t
{
	int min_i;
	int min_j;
	int max_i;
	int max_j;
	int r_offset;
	int g_offset;
	int b_offset;
	int rfb_xres;
	int rfb_maxy;
} varblock;

/*****************************************************************************/

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl);
static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl);

/*****************************************************************************/

static void init_fb(void)
{
	size_t pixels;
	size_t bytespp;

	if ((fbfd = open(FB_DEVICE, O_RDONLY)) == -1) {
		fprintf(stderr, "cannot open fb device %s\n", FB_DEVICE);
		exit(EXIT_FAILURE);
	}

	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo) != 0) {
		fprintf(stderr, "ioctl error\n");
		exit(EXIT_FAILURE);
	}

	pixels = scrinfo.xres * scrinfo.yres;
	bytespp = scrinfo.bits_per_pixel / 8;

	fprintf(stderr, "xres=%d, yres=%d, xresv=%d, yresv=%d, xoffs=%d, yoffs=%d, bpp=%d\n", 
	  (int)scrinfo.xres, (int)scrinfo.yres,
	  (int)scrinfo.xres_virtual, (int)scrinfo.yres_virtual,
	  (int)scrinfo.xoffset, (int)scrinfo.yoffset,
	  (int)scrinfo.bits_per_pixel);

	fbmmap_size = pixels * bytespp;
	fbmmap = mmap(NULL, fbmmap_size, PROT_READ, MAP_SHARED, fbfd, 0);

	if (fbmmap == MAP_FAILED) {
		fprintf(stderr, "mmap failed\n");
		exit(EXIT_FAILURE);
	}
}

static void cleanup_fb(void)
{
	if(fbfd != -1) {
	        munmap(fbmmap, fbmmap_size);
		close(fbfd);
	}
}


static int find_evdev(char *usename)
{
	int rfd, i;
        char device[32] = "";
	char name[256] = "Unknown";
	
	for (i=0 ; i<16 ; i++) {
		sprintf(device,"/dev/input/event%i",i);
		
        	if ((rfd = open (device, O_RDONLY)) != -1) {
			ioctl (rfd, EVIOCGNAME (sizeof (name)), name);
			close(rfd);

			if (!strcmp (name,usename)) {
				return i;
			}
		}
	}
	return 0;
}

static int kfd;
static int nr_keys = 0;

static int keymap_index[MAX_NR_KEYMAPS];	/* inverse of good_keymap */
static int good_keymap[MAX_NR_KEYMAPS], keymapnr, allocct;

static int is_a_console(int fd) {
	char arg;
	arg = 0;
	return (ioctl(fd, KDGKBTYPE, &arg) == 0 && ((arg == KB_101) || (arg == KB_84)));
}

static int open_a_console(const char *fnam) {
	int fd;
	/*
	 * * For ioctl purposes we only need some fd and permissions
	 * * do not matter. But setfont:activatemap() does a write.
	 * */
	fd = open(fnam, O_RDWR);
	if (fd < 0 && errno == EACCES)
		fd = open(fnam, O_WRONLY);
	if (fd < 0 && errno == EACCES)
		fd = open(fnam, O_RDONLY);
	if (fd < 0)
	return -1;
	if (!is_a_console(fd)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int has_key(int n, int t) {
	struct kbentry ke;
	int ret;
	ke.kb_table = t;	/* plain map is always present */
	ke.kb_index = n;
	ret = !ioctl(kfd, KDGKBENT, (unsigned long)&ke);
#ifdef DEBUG
	if (ret) {
		 fprintf (stdout, " got key value %04X for key %02X\n",ke.kb_value,n);
	}
#endif
	return ret;
}

static unsigned short get_key_sym(int n, int t) {
	struct kbentry ke;
	int ret;
	ke.kb_table = t;	/* plain map is always present */
	ke.kb_index = n;
	ret = !ioctl(kfd, KDGKBENT, (unsigned long)&ke);

	if (ret) {
		 return ke.kb_value;
	}

	return 0;
}


static void find_nr_keys(void) {
	nr_keys = (has_key(255,0) ? 256 : has_key(127,0) ? 128 : 112);
}

static void init_kbd()
{
	char name[256] = "unknown";
        int i;

	if((kbdfd = open(KBD_DEVICE, O_RDWR)) == -1) {
		fprintf(stderr, "cannot open kbd device %s\n", KBD_DEVICE);
		exit(EXIT_FAILURE);
	}

	ioctl (kbdfd, EVIOCGNAME (sizeof (name)), name);
#ifdef DEBUG
        fprintf (stdout, "  using device \"%s\"\n",name );
        fprintf (stdout, "Learning keys\n");
#endif
	kfd = open_a_console("/dev/tty0");

#ifdef DEBUG
      	if (kfd >= 0)
		fprintf (stdout, "  got console.\n");
#endif
	find_nr_keys();

#ifdef DEBUG
        fprintf (stdout, " got %d keys\n",nr_keys);
#endif
}

static void cleanup_kbd()
{
	if(kbdfd != -1) {
		close(kbdfd);
	}
	if(kfd != -1) {
		close(kfd);
	}
}

static void init_touch()
{
	char name[256] = "unknown";
    	struct input_absinfo info;

        if((touchfd = open(TOUCH_DEVICE, O_RDWR)) == -1) {
                fprintf(stderr, "cannot open touch device %s\n", TOUCH_DEVICE);
                exit(EXIT_FAILURE);
        }

	ioctl (touchfd, EVIOCGNAME (sizeof (name)), name);

    	// Get the Range of X and Y
    	if(ioctl(touchfd, EVIOCGABS(ABS_X), &info)) {
        	fprintf(stderr, "cannot get ABS_X info, %s\n", strerror(errno));
        	exit(EXIT_FAILURE);
    	}
    	xmin = info.minimum;
    	xmax = info.maximum;

    	if(ioctl(touchfd, EVIOCGABS(ABS_Y), &info)) {
        	fprintf(stderr, "cannot get ABS_Y, %s\n", strerror(errno));
        	exit(EXIT_FAILURE);
    	}
    	ymin = info.minimum;
    	ymax = info.maximum;

#ifdef DEBUG
        fprintf (stdout, "  using device \"%s\"\n",name );
    	fprintf (stdout, "  X info min:%i max:%i\n",xmin,xmax );
    	fprintf (stdout, "  Y info min:%i max:%i\n",ymin,ymax );
#endif
}

static void cleanup_touch()
{
	if(touchfd != -1) {
		close(touchfd);
	}
}

/*****************************************************************************/
int write_pid ()
{
  FILE *f;
  int fd;
  int pid;

  if ( ((fd = open(pidfile, O_RDWR|O_CREAT, 0644)) == -1)
       || ((f = fdopen(fd, "r+")) == NULL) ) {
      fprintf(stderr, "Can't open or create %s.\n", pidfile);
      return 0;
  }

 /* It is already somewhat safe to put up the pidfile
 * if we got past creating the server socket, that
 * should somewhat inicate we are the only one on the
 * system
 **/

  pid = getpid();
  if (!fprintf(f,"%d\n", pid)) {
      fprintf(stderr, "Can't write pid.\n");
      close(fd);
      return 0;
  }
  fflush(f);
  close(fd);

  return pid;
}

int remove_pid ()
{
  return unlink (pidfile);
}
/*****************************************************************************/

static void init_fb_server(int argc, char **argv)
{
	int bitsPerSample;
#ifdef DEBUG
	fprintf(stdout, "Initializing VNC server...\n");
#endif
	/* Allocate the VNC server buffer to be managed (not manipulated) by 
	 * libvncserver. */
	vncbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(vncbuf != NULL);

	/* Allocate the comparison buffer for detecting drawing updates from frame
	 * to frame. */
	fbbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(fbbuf != NULL);

	if (scrinfo.bits_per_pixel == 16) {
		bitsPerSample = 5;
        	vncscr = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, 5, 2, (scrinfo.bits_per_pixel / 8));
	}
	else if (scrinfo.bits_per_pixel == 24) { 
		bitsPerSample = 8;
		vncscr = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, 8, 3, 4);
	    	vncscr->serverFormat.bitsPerPixel = 32;
   		vncscr->serverFormat.depth = 24;
	}
	else if (scrinfo.bits_per_pixel == 32) { 
		bitsPerSample = 8;
		vncscr = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, 8, 3, 4);
	    	vncscr->serverFormat.bitsPerPixel = 32;
   		vncscr->serverFormat.depth = 32;
	}
	assert(vncscr != NULL);

	vncscr->desktopName = "Vircon Screen";
	vncscr->frameBuffer = (char *)vncbuf;
	vncscr->alwaysShared = FALSE;
	vncscr->httpDir = NULL;
	vncscr->port = VNC_PORT;
	vncscr->listenInterface = vncaddr;

#ifdef DEBUG
	fprintf(stdout, "	red.offset: %d\n", (int)scrinfo.red.offset);
	fprintf(stdout, "	red.length: %d\n", (int)scrinfo.red.length);
	fprintf(stdout, "	green.offset: %d\n", (int)scrinfo.green.offset);
	fprintf(stdout, "	green.length: %d\n", (int)scrinfo.green.length);
	fprintf(stdout, "	blue.offset: %d\n", (int)scrinfo.blue.offset);
	fprintf(stdout, "	blue.length: %d\n", (int)scrinfo.blue.length);

	fprintf(stdout, "	vncscr->serverFormat.redMax: %d\n", (int)vncscr->serverFormat.redMax);
	fprintf(stdout, "	vncscr->serverFormat.greenMax: %d\n", (int)vncscr->serverFormat.greenMax);
	fprintf(stdout, "	vncscr->serverFormat.blueMax: %d\n", (int)vncscr->serverFormat.blueMax);
	fprintf(stdout, "	vncscr->serverFormat.redShift: %d\n", (int)vncscr->serverFormat.redShift);
	fprintf(stdout, "	vncscr->serverFormat.greenShift: %d\n", (int)vncscr->serverFormat.greenShift);
	fprintf(stdout, "	vncscr->serverFormat.blueShift: %d\n", (int)vncscr->serverFormat.blueShift);
#endif
	vncscr->kbdAddEvent = keyevent;
	vncscr->ptrAddEvent = ptrevent;

	rfbInitServer(vncscr);

	if (vncscr->listenSock==-1) {
        	fprintf(stderr, "cannot start server.\n");
        	exit(EXIT_FAILURE);
	}

	/* Write the pid file */
	write_pid();

	/* Mark as dirty since we haven't sent any updates at all yet. */
	rfbMarkRectAsModified(vncscr, 0, 0, scrinfo.xres, scrinfo.yres);

	/* FB to RFB copying */
	varblock.r_offset = scrinfo.red.offset + scrinfo.red.length - bitsPerSample;
	varblock.g_offset = scrinfo.green.offset + scrinfo.green.length - bitsPerSample;
	varblock.b_offset = scrinfo.blue.offset + scrinfo.blue.length - bitsPerSample;
	varblock.rfb_xres = scrinfo.yres;
	varblock.rfb_maxy = scrinfo.xres - 1;
}

static void changeResolution()
{
	size_t pixels;
	size_t bytespp;

#ifdef DEBUG
	fprintf(stdout, "Changing resolution.\n");
#endif
	/* Clean up the old mapping and buffers*/
	free(vncbuf);
	free(fbbuf);

	munmap(fbmmap, fbmmap_size);
	
	/* Get the new screen layout information */
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo) != 0) {
		printf("ioctl error\n");
		exit(EXIT_FAILURE);
	}

	pixels = scrinfo.xres * scrinfo.yres;
	bytespp = scrinfo.bits_per_pixel / 8;

#ifdef DEBUG
	printf("Mapping new fb.\n");
	fprintf(stdout, "xres=%d, yres=%d, xresv=%d, yresv=%d, xoffs=%d, yoffs=%d, bpp=%d\n", 
	  (int)scrinfo.xres, (int)scrinfo.yres,
	  (int)scrinfo.xres_virtual, (int)scrinfo.yres_virtual,
	  (int)scrinfo.xoffset, (int)scrinfo.yoffset,
	  (int)scrinfo.bits_per_pixel);
#endif
	/* Map the new framebuffer into memory */
	
	fbmmap_size = pixels * bytespp;
	fbmmap = mmap(NULL, fbmmap_size, PROT_READ, MAP_SHARED, fbfd, 0);

	if (fbmmap == MAP_FAILED) {
		fprintf(stderr, "mmap failed\n");
		exit(EXIT_FAILURE);
	}

	/* Allocate the VNC server buffer to be managed (not manipulated) by 
	 * libvncserver. */
	vncbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(vncbuf != NULL);

	/* Allocate the comparison buffer for detecting drawing updates from frame
	 * to frame. */
	fbbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(fbbuf != NULL);

	/* Tell libvncserver that the resolution has changed. */

	//rfbNewFramebuffer (rfbScreenInfoPtr rfbScreen, char *framebuffer, int width, int height, int bitsPerSample, int samplesPerPixel, int bytesPerPixel)
	rfbNewFramebuffer(vncscr, (char *)vncbuf, scrinfo.xres, scrinfo.yres, 5, 2, (scrinfo.bits_per_pixel / 8));

#ifdef DEBUG
	printf("Change resolution complete.\n");
#endif
}

/*****************************************************************************/
static void injectKeyEvent(uint16_t code, uint16_t value)
{
    	struct input_event ev;

    	memset(&ev, 0, sizeof(ev));
    	gettimeofday(&ev.time,0);
    	ev.type = EV_KEY;
    	ev.code = code;
    	ev.value = value;
    	if(write(kbdfd, &ev, sizeof(ev)) < 0) {
        	fprintf(stderr,"write event failed, %s\n", strerror(errno));
    	}

#ifdef DEBUG
    	printf("injectKey (%d, %d)\n", code , value);    
#endif
    	gettimeofday(&ev.time,0);
    	ev.type = EV_SYN;
    	ev.code = 0;
    	ev.value = 0;
    	if(write(kbdfd, &ev, sizeof(ev)) < 0) {
       	 	fprintf(stderr,"write event failed, %s\n", strerror(errno));
    	}
}

static int keysym2scancode(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    	int scancode = 0;
    	int code = (int)key;

    	if (code>='0' && code<='9') {
        	scancode = (code & 0xF) - 1;
        	if (scancode<0) scancode += 10;
        	scancode += KEY_1;
    	} else if (code>=0xFF50 && code<=0xFF58) {
        	static const uint16_t map[] =
             		{  KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
                	   KEY_PAGEUP , KEY_PAGEDOWN , KEY_END, 0 };
        	scancode = map[code & 0xF];
    	} else if (code>=0xFFBE && code<=0xFFC9) {
        	static const uint16_t map[] =
             		{  KEY_F1, KEY_F2, KEY_F3, KEY_F4,  KEY_F5,  KEY_F6,
                	   KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12 };
        	scancode = map[code - 0xFFBE];
    	} else if (code>=0xFFE1 && code<=0xFFEE) {
        	static const uint16_t map[] =
             		{ 0,  KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                	  KEY_LEFTCTRL, KEY_RIGHTCTRL,
                	  KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                	  0,0,
                	  KEY_LEFTALT, KEY_RIGHTALT,
                	  0, 0, 0, 0 };
        	scancode = map[code & 0xF];
    	} else if ((code>='A' && code<='Z') || (code>='a' && code<='z')) {
        	static const uint16_t map[] = {
                	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
                	KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
                	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
                	KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
                	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
        	scancode = map[(code & 0x5F) - 'A'];
    	} else {
        	switch (code) 
		{
            		case 0xFE03:    scancode = 92;   			break;
            		case 0xFF08:    scancode = KEY_BACKSPACE;   		break;
//            case 0xFF63:    scancode = KEY_INSERT;   break;
            		case 0xFF1B:    scancode = KEY_ESC;        		break;
            		case 0xFF09:    scancode = KEY_TAB;         		break;
            		case 0xFF0D:    scancode = KEY_ENTER;       		break;
            		case 0xFFFF:    scancode = KEY_DELETE;      		break;
            		case 0xFFC8:    rfbShutdownServer(cl->screen,TRUE);	break; // F11            
        	}
        	if (!scancode) {
	    		int i;
	    		unsigned short kcode;
	    		/* Hunt them down */
	    		for (i=0;i<nr_keys;i++) {
	        		/* Looking in keytable for normal keys */
				kcode = get_key_sym(i,0);
	        		if (kcode == code) {
		    			scancode = i;
#ifdef DEBUG
		    			fprintf(stdout, "Search normal\n");
#endif
		    			break;
				}
	        		/* Looking in keytable for keys with shift */
				kcode = get_key_sym(i,1);
	        		if (kcode == code) {
		    			scancode = i;
#ifdef DEBUG
		    			fprintf(stdout, "Search shifted\n");
#endif
		    			break;
				}
	    		}
		}
    	}

    	return scancode;
}

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
	int scancode;

#ifdef DEBUG
	fprintf(stdout, "Got keysym: %04x (state=%d)\n", (unsigned int)key, (int)down);
#endif

	if ((scancode = keysym2scancode(down, key, cl))) {
		injectKeyEvent(scancode, down);
	}
}


static void injectMoveEvent(int x, int y)
{   
    	struct input_event ev;

#ifdef DEBUG    
    	fprintf(stdout, "handleMoveEvent (x=%d, y=%d)\n", x , y);    
#endif

    	if (xmax != 0 && ymax != 0) {
        	x = xmin + (x * (xmax - xmin)) / (scrinfo.xres);
        	y = ymin + (y * (ymax - ymin)) / (scrinfo.yres);
    	}
    
    	memset(&ev, 0, sizeof(ev));

    	gettimeofday(&ev.time,0);
    	ev.type = EV_ABS;
    	ev.code = ABS_X;
    	ev.value = x;
    	if(write(touchfd, &ev, sizeof(ev)) < 0) {
        	fprintf(stderr, "write event failed, %s\n", strerror(errno));
    	}

    	gettimeofday(&ev.time,0);
    	ev.type = EV_ABS;
    	ev.code = ABS_Y;
    	ev.value = y;
    	if(write(touchfd, &ev, sizeof(ev)) < 0) {
        	fprintf(stderr, "write event failed, %s\n", strerror(errno));
    	}

    	gettimeofday(&ev.time,0);
    	ev.type = EV_SYN;
    	ev.code = 0;
    	ev.value = 0;
    	if(write(touchfd, &ev, sizeof(ev)) < 0) {
        	fprintf(stderr, "write event failed, %s\n", strerror(errno));
    	}
}

static void injectWheelEvent(int z, int x, int y)
{
    	struct input_event ev;

#ifdef DEBUG
    	printf("handleTouchEvent (x=%d, y=%d, inc=%d)\n", x , y, z);    
#endif

    	// Move the pointer first 
    	injectMoveEvent(x,y);

    	memset(&ev, 0, sizeof(ev));

    	// Then send a BTN_XXXX
    	gettimeofday(&ev.time,0);
    	ev.type = EV_REL;
    	ev.code = REL_WHEEL;
    	ev.value = z;
    	if(write(touchfd, &ev, sizeof(ev)) < 0) {
        	fprintf(stderr, "write event failed, %s\n", strerror(errno));
    	}

    	gettimeofday(&ev.time,0);
    	ev.type = EV_SYN;
    	ev.code = 0;
    	ev.value = 0;
    	if(write(touchfd, &ev, sizeof(ev)) < 0) {
        	fprintf(stderr, "write event failed, %s\n", strerror(errno));
    	}

#ifdef DEBUG
    	fprintf(stdout, "injectWheelEvent (x=%d, y=%d, inc=%d)\n", x , y, z);    
#endif
}

static void injectTouchEvent(int down, int button, int x, int y)
{
    	static const uint16_t map[] = { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_FORWARD, BTN_BACK};
    	struct input_event ev;

#ifdef DEBUG    
    	fprintf(stdout, "handleTouchEvent (x=%d, y=%d, button=%d, down=%d)\n", x , y, button, down);    
#endif

    	memset(&ev, 0, sizeof(ev));

    	// Then send a BTN_XXXX
    	gettimeofday(&ev.time,0);
   	ev.type = EV_KEY;
    	ev.code = map[button];
    	ev.value = down;
    	if(write(touchfd, &ev, sizeof(ev)) < 0) {
       		fprintf(stderr, "write event failed, %s\n", strerror(errno));
    	}

    	/* Move event also adds the SYN */
    	injectMoveEvent(x,y);

#ifdef DEBUG
    	fprintf(stdout, "injectTouchEvent (x=%d, y=%d, down=%d)\n", x , y, down);    
#endif
}

static int prev_x = 0;
static int prev_y = 0;
static int prev_buttonMask = 0;

static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl)
{
	//printf("Got ptrevent: %04x (x=%d, y=%d)\n", buttonMask, x, y);
	if((buttonMask & 1) != 0 && (prev_buttonMask & 1) == 0 ) {
		// Simulate left mouse event as touch event
		injectTouchEvent(1, 0, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	else if((prev_buttonMask & 1) != 0 && (buttonMask & 1) == 0 ) {
		// Simulate left mouse event as touch event
		injectTouchEvent(0, 0, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	if((buttonMask & 2) != 0 && (prev_buttonMask & 2) == 0 ) {
		// Simulate middle mouse event as touch event
		injectTouchEvent(1, 1, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	else if((prev_buttonMask & 2) != 0 && (buttonMask & 2) == 0 ) {
		// Simulate middle mouse event as touch event
		injectTouchEvent(0, 1, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	if((buttonMask & 4) != 0 && (prev_buttonMask & 4) == 0 ) {
		// Simulate right mouse event as touch event
		injectTouchEvent(1, 2, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	else if((prev_buttonMask & 4) != 0 && (buttonMask & 4) == 0 ) {
		// Simulate right mouse event as touch event
		injectTouchEvent(0, 2, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	if((buttonMask & 8) != 0 && (prev_buttonMask & 8) == 0 ) {
		// Simulate right mouse event as touch event
		//injectTouchEvent(1, 3, x, y);
		injectWheelEvent(1, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	else if((prev_buttonMask & 8) != 0 && (buttonMask & 8) == 0 ) {
		// Simulate right mouse event as touch event
		//injectTouchEvent(0, 3, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	if((buttonMask & 16) != 0 && (prev_buttonMask & 16) == 0 ) {
		// Simulate right mouse event as touch event
		//injectTouchEvent(1, 4, x, y);
		injectWheelEvent(-1, x, y);
                prev_buttonMask = buttonMask;
		return;
	}
	else if((prev_buttonMask & 16) != 0 && (buttonMask & 16) == 0 ) {
		// Simulate right mouse event as touch event
		//injectTouchEvent(0, 4, x, y);
                prev_buttonMask = buttonMask;
		return;
	}

	if (mousemode) {
		// Simulate mouse movement
     	   	if ( x != prev_x || y != prev_y ) {
			injectMoveEvent(x,y);
			prev_x = x;
			prev_y = y;
		}
	}
}

static int readScreenInfo_m()
{
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo_m) != 0) {
		fprintf(stderr, "ioctl error\n");
		exit(EXIT_FAILURE);
	}

	if (scrinfo.xres != scrinfo_m.xres || 
	    scrinfo.yres != scrinfo_m.yres ||
	    scrinfo.bits_per_pixel != scrinfo_m.bits_per_pixel ) {
		return 1;
	}

	return 0;
}

#define PIXEL_FB_TO_RFB(p,r,g,b) ((p>>r)&0x1f001f)|(((p>>g)&0x1f001f)<<5)|(((p>>b)&0x1f001f)<<10)

static int update_screen(void)
{
	unsigned int *f, *c, *r;
	int x, y, line_changed,lines_unchanged,changes_pending;

	/* Check if the framebuffer resolution was changed */
	if (readScreenInfo_m()) {
		return 3;  //screen changed
	}
	
	varblock.min_i = varblock.min_j = 9999;
	varblock.max_i = varblock.max_j = -1;

	f = (unsigned int *)fbmmap;        /* -> framebuffer         */
	c = (unsigned int *)fbbuf;         /* -> compare framebuffer */
	r = (unsigned int *)vncbuf;        /* -> remote framebuffer  */

	line_changed = 0;

	for (y = 0; y < scrinfo.yres; y++)
	{
		/* Compare every 2 pixels at a time, assuming that changes are likely
		 * in pairs. */
		for (x = 0; x < scrinfo.xres; x += 2) {
			unsigned int pixel = *f;

			if (pixel != *c) {
				*c = pixel;

				line_changed = 1;
				changes_pending = 1;
				lines_unchanged = 0;

				*r = PIXEL_FB_TO_RFB(pixel,
				  varblock.r_offset, varblock.g_offset, varblock.b_offset);

				if (x < varblock.min_i)
					varblock.min_i = x;
				else {
					if (x > varblock.max_i)
						varblock.max_i = x;

					if (y > varblock.max_j)
						varblock.max_j = y;
					else if (y < varblock.min_j)
						varblock.min_j = y;
				}
			}

			f++, c++;
			r++;
		}

		if (!line_changed)
			lines_unchanged++;

		if (lines_unchanged > 5 && changes_pending) {
			rfbMarkRectAsModified(vncscr, varblock.min_i, varblock.min_j,
                  				varblock.max_i + 2, varblock.max_j + 1);
			changes_pending = 0;
			varblock.min_i = varblock.min_j = 9999;
			varblock.max_i = varblock.max_j = -1;
		}
	}

	if (varblock.min_i < 9999) {
		if (varblock.max_i < 0)
			varblock.max_i = varblock.min_i;

		if (varblock.max_j < 0)
			varblock.max_j = varblock.min_j;

#ifdef DEBUG
		fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
		  (varblock.max_i+2) - varblock.min_i, (varblock.max_j+1) - varblock.min_j,
		  varblock.min_i, varblock.min_j);
#endif
		rfbMarkRectAsModified(vncscr, varblock.min_i, varblock.min_j,
		  varblock.max_i + 2, varblock.max_j + 1);

		rfbProcessEvents(vncscr, 10000);
	}

	return 0;
}

/*****************************************************************************/
void sig_handler(int signo)
{
  	if (signo == SIGINT || signo == SIGTERM) {
    		printf("received SIGNAL\n");
		shutdown_set = 1;
		if (vncscr->clientHead == NULL) {
			remove_pid();
			exit(0);
		}
	}
}

/*****************************************************************************/

void print_usage(char **argv)
{
	fprintf(stdout, "%s [-k device] [-t device] [-h]\n"
		"-k device: keyboard device node, default is autodetect 'vircon keyboard'\n"
		"-t device: touch device node, default is autodetect 'vircon mouse'\n"
		"-f device: fb device node, default is /dev/fb0\n"
		"-m : mouse/touch mode, default is touch\n"
		"-l : only offer connections on localhost interface, default is all\n"
		"-d : don't become daemon process, run in foreground\n"
		"-h : print this help\n",argv[0]);
}

int main(int argc, char **argv)
{
	int daemonize = 1;
	/*Per default: listen on all adresses*/
	char vnc_ip_addr[64] = "0.0.0.0";

	if(argc > 1) {
		int i=1;
		while(i < argc) {
			if(*argv[i] == '-') {
				switch(*(argv[i] + 1))
				{
					case 'h':
						print_usage(argv);
						exit(0);
						break;
					case 'm':
						mousemode=1;
						break;
					case 'd':
						daemonize=0;
						break;
					case 'l':
						sprintf(vnc_ip_addr,"127.0.0.1");
						break;
					case 'k':
						i++;
						strcpy(KBD_DEVICE, argv[i]);
						break;
					case 't':
						i++;
						strcpy(TOUCH_DEVICE, argv[i]);
						break;
					case 'f':
						i++;
						strcpy(FB_DEVICE, argv[i]);
						break;
				}
			}
			i++;
		}
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR)
		printf("can't catch SIGINT\n");
	if (signal(SIGTERM, sig_handler) == SIG_ERR)
		printf("can't catch SIGTERM\n");
	if (signal(SIGKILL, sig_handler) == SIG_ERR)
		printf("can't catch SIGKILL\n");

	/* Input devices auto discovery (when using vircon) */
	if ( strncmp("auto" ,KBD_DEVICE, 4) == 0 ) {
		int devnum = 0;
		devnum = find_evdev("vircon keyboard");
		if ( devnum ) {
			sprintf(KBD_DEVICE,"/dev/input/event%i", devnum);
#ifdef DEBUG
			fprintf(stdout, "found vircon KBD device: %s\n",KBD_DEVICE);
#endif
		}
	}

	if ( strncmp("auto" ,TOUCH_DEVICE, 4) == 0 ) {
		int devnum = 0;
		devnum = find_evdev("vircon mouse");
		if ( devnum ) {
			sprintf(TOUCH_DEVICE,"/dev/input/event%i", devnum);
#ifdef DEBUG
			fprintf(stdout, "found vircon MOUSE device: %s\n",TOUCH_DEVICE);	
#endif
		}
	}

	/* Bail out if autodetect failed */
	if (!strncmp("auto" ,TOUCH_DEVICE, 4) || !strncmp("auto" ,KBD_DEVICE, 4)) {
		printf("Error. Could not detect mouse or keyboard device.");
		exit(1);
	}

	printf("Initializing framebuffer device  %s ...\n", FB_DEVICE);
	init_fb();
	printf("Initializing keyboard device %s ...\n", KBD_DEVICE);
	init_kbd();
	printf("Initializing touch device %s ...\n", TOUCH_DEVICE);
	init_touch();

	printf("Initializing VNC server:\n");
	printf("	width:  %d\n", (int)scrinfo.xres);
	printf("	height: %d\n", (int)scrinfo.yres);
	printf("	bpp:    %d\n", (int)scrinfo.bits_per_pixel);
	printf("	port:   %d\n", (int)VNC_PORT);

	vncaddr = inet_addr(vnc_ip_addr);
	printf("	addr:   %s\n", vnc_ip_addr);

	/* Daemonize */

	if (daemonize) {
		int i;
		mpid=fork();
		if (mpid<0) 
			exit(1);
		if (mpid>0) 
			exit(0); 

		setsid(); 
	
		i=open("/dev/null",O_RDWR); 
		dup(i); 
		dup(i); 

		signal(SIGPIPE, SIG_IGN);
	}

	init_fb_server(argc, argv);

	/* Implement our own event loop to detect changes in the framebuffer. */
	while (!shutdown_set) {
		while (vncscr->clientHead == NULL)
			rfbProcessEvents(vncscr, 100000);

		rfbProcessEvents(vncscr, 100000);
		if (update_screen() == 3) {
			/* Resolution or color scheme changed */
#ifdef DEBUG
			fprintf(stdout, "VNC server needs re-init()\n");	
#endif
			changeResolution();
		}
	}

	rfbShutdownServer(vncscr ,TRUE);

	printf("Cleaning up...\n");
	cleanup_fb();
	cleanup_kbd();
	cleanup_touch();

	remove_pid();
}

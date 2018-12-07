/*
 *  linux/drivers/video/vircon.c -- Virtual console device
 *    Copyright (C) 2015 Dirk Herrendoerfer 
 *  
 *  Vircon provides a virtual console consisting of a virtual framebuffer,
 *  a virtual keyboard, and a virtual mouse input device.
 *  It is intended to be used with the fbvncserver service. 
 *
 *    This drivers framebuffer implementation is based on vfb.c by:
 *      Copyright (C) 2002 James Simmons
 *	Copyright (C) 1997 Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/fs.h>
#include <linux/input.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/fb.h>
#include <linux/init.h>

struct input_dev *virmouse_input_dev;
static struct platform_device *virmouse_dev; /* Device structure */

struct input_dev *virkbd_input_dev;
static struct platform_device *virkbd_dev;

/*
 *  RAM we reserve for the frame buffer. This defines the maximum screen
 *  size
 *
 *  The default can be overridden if the driver is compiled as a module
 */

#define VIDEOMEMSIZE	(4*1024*1024)	/* 2 MB */

static void *videomemory;
static u_long videomemorysize = VIDEOMEMSIZE;
module_param(videomemorysize, ulong, 0);

/**********************************************************************
 *
 * Memory management
 *
 **********************************************************************/
static void *rvmalloc(unsigned long size)
{
	void *mem;
	unsigned long adr;

	size = PAGE_ALIGN(size);
	mem = vmalloc_32(size);
	if (!mem)
		return NULL;

	memset(mem, 0, size); /* Clear the ram out, no junk to the user */
	adr = (unsigned long) mem;
	while (size > 0) {
		SetPageReserved(vmalloc_to_page((void *)adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return mem;
}

static void rvfree(void *mem, unsigned long size)
{
	unsigned long adr;

	if (!mem)
		return;

	adr = (unsigned long) mem;
	while ((long) size > 0) {
		ClearPageReserved(vmalloc_to_page((void *)adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}
	vfree(mem);
}

static struct fb_var_screeninfo vircon_default = {
	.xres =		640,
	.yres =		480,
	.xres_virtual =	640,
	.yres_virtual =	480,
	.bits_per_pixel = 8,
	.red =		{ 0, 8, 0 },
      	.green =	{ 0, 8, 0 },
      	.blue =		{ 0, 8, 0 },
      	.activate =	FB_ACTIVATE_TEST,
      	.height =	-1,
      	.width =	-1,
      	.pixclock =	20000,
      	.left_margin =	64,
      	.right_margin =	64,
      	.upper_margin =	32,
      	.lower_margin =	32,
      	.hsync_len =	64,
      	.vsync_len =	2,
      	.vmode =	FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo vircon_fix = {
	.id =		"Virtual FB",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.xpanstep =	1,
	.ypanstep =	1,
	.ywrapstep =	1,
	.accel =	FB_ACCEL_NONE,
};

static bool vircon_enable __initdata = 0;	/* disabled by default */
module_param(vircon_enable, bool, 0);

static int vircon_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info);
static int vircon_set_par(struct fb_info *info);
static int vircon_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info);
static int vircon_pan_display(struct fb_var_screeninfo *var,
			   struct fb_info *info);
static int vircon_mmap(struct fb_info *info,
		    struct vm_area_struct *vma);

static struct fb_ops vircon_ops = {
	.fb_read        = fb_sys_read,
	.fb_write       = fb_sys_write,
	.fb_check_var	= vircon_check_var,
	.fb_set_par	= vircon_set_par,
	.fb_setcolreg	= vircon_setcolreg,
	.fb_pan_display	= vircon_pan_display,
	.fb_fillrect	= sys_fillrect,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
	.fb_mmap	= vircon_mmap,
};

    /*
     *  Internal routines
     */

static u_long get_line_length(int xres_virtual, int bpp)
{
	u_long length;

	length = xres_virtual * bpp;
	length = (length + 31) & ~31;
	length >>= 3;
	return (length);
}

    /*
     *  Setting the video mode has been split into two parts.
     *  First part, xxxfb_check_var, must not write anything
     *  to hardware, it should only verify and adjust var.
     *  This means it doesn't alter par but it does use hardware
     *  data from it to check this var. 
     */

static int vircon_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info)
{
	u_long line_length;

	/*
	 *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
	 *  as FB_VMODE_SMOOTH_XPAN is only used internally
	 */

	if (var->vmode & FB_VMODE_CONUPDATE) {
		var->vmode |= FB_VMODE_YWRAP;
		var->xoffset = info->var.xoffset;
		var->yoffset = info->var.yoffset;
	}

	/*
	 *  Some very basic checks
	 */
	if (!var->xres)
		var->xres = 1;
	if (!var->yres)
		var->yres = 1;
	if (var->xres > var->xres_virtual)
		var->xres_virtual = var->xres;
	if (var->yres > var->yres_virtual)
		var->yres_virtual = var->yres;
	if (var->bits_per_pixel <= 1)
		var->bits_per_pixel = 1;
	else if (var->bits_per_pixel <= 8)
		var->bits_per_pixel = 8;
	else if (var->bits_per_pixel <= 16)
		var->bits_per_pixel = 16;
	else if (var->bits_per_pixel <= 24)
		var->bits_per_pixel = 24;
	else if (var->bits_per_pixel <= 32)
		var->bits_per_pixel = 32;
	else
		return -EINVAL;

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	/*
	 *  Memory limit
	 */
	line_length =
	    get_line_length(var->xres_virtual, var->bits_per_pixel);
	if (line_length * var->yres_virtual > videomemorysize)
		return -ENOMEM;

	/*
	 * Now that we checked it we alter var. The reason being is that the video
	 * mode passed in might not work but slight changes to it might make it 
	 * work. This way we let the user know what is acceptable.
	 */
	switch (var->bits_per_pixel) {
	case 1:
	case 8:
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 16:		/* RGBA 5551 */
		if (var->transp.length) {
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 5;
			var->blue.offset = 10;
			var->blue.length = 5;
			var->transp.offset = 15;
			var->transp.length = 1;
		} else {	/* RGB 565 */
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 6;
			var->blue.offset = 11;
			var->blue.length = 5;
			var->transp.offset = 0;
			var->transp.length = 0;
		}
		break;
	case 24:		/* RGB 888 */
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 32:		/* RGBA 8888 */
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		var->transp.offset = 24;
		var->transp.length = 8;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;

	return 0;
}

/* This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the 
 * change in par. For this driver it doesn't do much. 
 */
static int vircon_set_par(struct fb_info *info)
{
	info->fix.line_length = get_line_length(info->var.xres_virtual,
						info->var.bits_per_pixel);
	return 0;
}

    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     */

static int vircon_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

	/* Directcolor:
	 *   var->{color}.offset contains start of bitfield
	 *   var->{color}.length contains length of bitfield
	 *   {hardwarespecific} contains width of RAMDAC
	 *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
	 *   RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Pseudocolor:
	 *    var->{color}.offset is 0 unless the palette index takes less than
	 *                        bits_per_pixel bits and is stored in the upper
	 *                        bits of the pixel value
	 *    var->{color}.length is set so that 1 << length is the number of available
	 *                        palette entries
	 *    cmap is not used
	 *    RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Truecolor:
	 *    does not use DAC. Usually 3 are present.
	 *    var->{color}.offset contains start of bitfield
	 *    var->{color}.length contains length of bitfield
	 *    cmap is programmed to (red << red.offset) | (green << green.offset) |
	 *                      (blue << blue.offset) | (transp << transp.offset)
	 *    RAMDAC does not exist
	 */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 8:
			break;
		case 16:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	return 0;
}

    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

static int vircon_pan_display(struct fb_var_screeninfo *var,
			   struct fb_info *info)
{
	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset >= info->var.yres_virtual ||
		    var->xoffset)
			return -EINVAL;
	} else {
		if (var->xoffset + info->var.xres > info->var.xres_virtual ||
		    var->yoffset + info->var.yres > info->var.yres_virtual)
			return -EINVAL;
	}
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;
	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;
	return 0;
}

    /*
     *  Most drivers don't need their own mmap function 
     */

static int vircon_mmap(struct fb_info *info,
		    struct vm_area_struct *vma)
{
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long page, pos;

	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
		return -EINVAL;
	if (size > info->fix.smem_len)
		return -EINVAL;
	if (offset > info->fix.smem_len - size)
		return -EINVAL;

	pos = (unsigned long)info->fix.smem_start + offset;

	while (size > 0) {
		page = vmalloc_to_pfn((void *)pos);
		if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED)) {
			return -EAGAIN;
		}
		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	return 0;

}

#ifndef MODULE
/*
 * The virtual framebuffer driver is only enabled if explicitly
 * requested by passing 'video=vircon:' (or any actual options).
 */
static int __init vircon_setup(char *options)
{
	char *this_opt;

	vircon_enable = 0;

	if (!options)
		return 1;

	vircon_enable = 1;

	if (!*options)
		return 1;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;
		/* Test disable for backwards compatibility */
		if (!strcmp(this_opt, "disable"))
			vircon_enable = 0;
	}
	return 1;
}
#endif  /*  MODULE  */

    /*
     *  Initialisation
     */

static int vircon_probe(struct platform_device *dev)
{
	struct fb_info *info;
	int retval = -ENOMEM;

	/*
	 * For real video cards we use ioremap.
	 */
	if (!(videomemory = rvmalloc(videomemorysize)))
		return retval;

	/*
	 * VFB must clear memory to prevent kernel info
	 * leakage into userspace
	 * VGA-based drivers MUST NOT clear memory if
	 * they want to be able to take over vgacon
	 */
	memset(videomemory, 0, videomemorysize);

	info = framebuffer_alloc(sizeof(u32) * 256, &dev->dev);
	if (!info)
		goto err;

	info->screen_base = (char __iomem *)videomemory;
	info->fbops = &vircon_ops;

	retval = fb_find_mode(&info->var, info, NULL,
			      NULL, 0, NULL, 16);

	if (!retval || (retval == 4))
		info->var = vircon_default;
	vircon_fix.smem_start = (unsigned long) videomemory;
	vircon_fix.smem_len = videomemorysize;
	info->fix = vircon_fix;
	info->pseudo_palette = info->par;
	info->par = NULL;
	info->flags = FBINFO_DEFAULT;

	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0)
		goto err1;

	retval = register_framebuffer(info);
	if (retval < 0)
		goto err2;
	platform_set_drvdata(dev, info);

	printk("Virtual frame buffer device, using %ldK of video memory\n",
		videomemorysize >> 10);
	return 0;
err2:
	fb_dealloc_cmap(&info->cmap);
err1:
	framebuffer_release(info);
err:
	rvfree(videomemory, videomemorysize);
	return retval;
}

static int vircon_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);

	if (info) {
		unregister_framebuffer(info);
		rvfree(videomemory, videomemorysize);
		fb_dealloc_cmap(&info->cmap);
		framebuffer_release(info);
	}
	return 0;
}

static struct platform_driver vircon_driver = {
	.probe	= vircon_probe,
	.remove = vircon_remove,
	.driver = {
		.name	= "vircon",
	},
};

static struct platform_device *vircon_device;

static int __init virconfb_init(void)
{
	int ret = 0;

#ifndef MODULE
	char *option = NULL;

	if (fb_get_options("vircon", &option))
		return -ENODEV;
	vircon_setup(option);
#endif

	if (!vircon_enable)
		return -ENXIO;

	ret = platform_driver_register(&vircon_driver);

	if (!ret) {
		vircon_device = platform_device_alloc("vircon", 0);

		if (vircon_device)
			ret = platform_device_add(vircon_device);
		else
			ret = -ENOMEM;

		if (ret) {
			platform_device_put(vircon_device);
			platform_driver_unregister(&vircon_driver);
		}
	}

	return ret;
}

/* Input Driver Initializing */
int __init virmouse_init(void)
{
        /* Register a platform device */
        virmouse_dev = platform_device_register_simple("virmouse", -1, NULL, 0);
        if (IS_ERR(virmouse_dev)){
                printk ("virmouse_init: error\n");
                return PTR_ERR(virmouse_dev);
        }

        /* Allocate an input device data structure */
        virmouse_input_dev = input_allocate_device();
        if (!virmouse_input_dev) {
                printk("Bad input_allocate_device()\n");
                return -ENOMEM;
        }

        input_alloc_absinfo(virmouse_input_dev);

	virmouse_input_dev->name = "vircon mouse";

        /* Announce that the virtual mouse will generate absolute coordinates */
        set_bit(EV_ABS, virmouse_input_dev->evbit);
        set_bit(ABS_X, virmouse_input_dev->absbit);
        set_bit(ABS_Y, virmouse_input_dev->absbit);

        /* Announce key event */
        set_bit(EV_KEY, virmouse_input_dev->evbit);
        set_bit(BTN_LEFT, virmouse_input_dev->keybit);
        set_bit(BTN_MIDDLE, virmouse_input_dev->keybit);
        set_bit(BTN_RIGHT, virmouse_input_dev->keybit);

	/* Scroll wheel needs this*/
        set_bit(EV_REL, virmouse_input_dev->evbit);
        set_bit(REL_WHEEL, virmouse_input_dev->relbit);

        set_bit(BTN_FORWARD, virmouse_input_dev->keybit);
        set_bit(BTN_BACK, virmouse_input_dev->keybit);

        /* Register with the input subsystem */
        if(input_register_device(virmouse_input_dev))
		printk("input_register_device failed.\n");
	input_set_abs_params(virmouse_input_dev, ABS_X, 0, 32767, 0, 0);
	input_set_abs_params(virmouse_input_dev, ABS_Y, 0, 32767, 0, 0);

        /* print messages in the dmesg */
        printk("Virtual Mouse Driver Initialized.\n");

        return 0;
}

#define KEYMAP_SIZE 512
static unsigned short atkbd_set2_keycode[KEYMAP_SIZE] = {
          0, 67, 65, 63, 61, 59, 60, 88,  0, 68, 66, 64, 62, 15, 41,117,
          0, 56, 42, 93, 29, 16,  2,  0,  0,  0, 44, 31, 30, 17,  3,  0,
          0, 46, 45, 32, 18,  5,  4, 95,  0, 57, 47, 33, 20, 19,  6,183,
          0, 49, 48, 35, 34, 21,  7,184,  0,  0, 50, 36, 22,  8,  9,185,
          0, 51, 37, 23, 24, 11, 10,  0,  0, 52, 53, 38, 39, 25, 12,  0,
          0, 89, 40,  0, 26, 13,  0,  0, 58, 54, 28, 27,  0, 43,  0, 85,
          0, 86, 91, 90, 92,  0, 14, 94,  0, 79,124, 75, 71,121,  0,  0,
         82, 83, 80, 76, 77, 72,  1, 69, 87, 78, 81, 74, 55, 73, 70, 99,

          0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        217,100,255,  0, 97,165,  0,  0,156,  0,  0,  0,  0,  0,  0,125,
        173,114,  0,113,  0,  0,  0,126,128,  0,  0,140,  0,  0,  0,127,
        159,  0,115,  0,164,  0,  0,116,158,  0,172,166,  0,  0,  0,142,
        157,  0,  0,  0,  0,  0,  0,  0,155,  0, 98,  0,  0,163,  0,  0,
        226,  0,  0,  0,  0,  0,  0,  0,  0,255, 96,  0,  0,  0,143,  0,
          0,  0,  0,  0,  0,  0,  0,  0,  0,107,  0,105,102,  0,  0,112,
        110,111,108,112,106,103,  0,119,  0,118,109,  0, 99,104,119,  0,
          0,  0,  0, 65, 99,
};

/* Driver Initializing */
int __init virkbd_init(void)
{
	int i;

        /* Register a platform device */
        virkbd_dev = platform_device_register_simple("virkbd", -1, NULL, 0);
        if (IS_ERR(virkbd_dev)){
                printk ("virkbd_init: error\n");
                return PTR_ERR(virkbd_dev);
        }

        /* Allocate an input device data structure */
        virkbd_input_dev = input_allocate_device();
        if (!virkbd_input_dev) {
                printk("Bad input_allocate_device()\n");
                return -ENOMEM;
        }

	virkbd_input_dev->name = "vircon keyboard";
        virkbd_input_dev->id.vendor = 0x0001;
        virkbd_input_dev->id.product = 0x01;
        virkbd_input_dev->id.version = 0x01;
 
        /* Announce key event */
        set_bit(EV_KEY, virkbd_input_dev->evbit);

	/* Set a basic keymap */
	virkbd_input_dev->keycode = atkbd_set2_keycode;
        virkbd_input_dev->keycodesize = sizeof(unsigned short);
        virkbd_input_dev->keycodemax =  KEYMAP_SIZE;

        for (i = 0; i < KEYMAP_SIZE; i++) {
                if (atkbd_set2_keycode[i] != KEY_RESERVED &&
                    atkbd_set2_keycode[i] != 255 &&
                    atkbd_set2_keycode[i] < 0xfff8 ) {
	        	__set_bit(atkbd_set2_keycode[i], virkbd_input_dev->keybit);
		}
        }

        /* Register with the input subsystem */
        if (input_register_device(virkbd_input_dev))
		printk("input_register_device failed.\n");

        /* print messages in the dmesg */
        printk("Virtual Keyboard Driver Initialized.\n");

        return 0;
}

/* Driver initializing */
static int __init vircon_init(void)
{
	virconfb_init();
	virkbd_init();
	virmouse_init();

        return 0;
}

module_init(vircon_init);

#ifdef MODULE
/* Driver Uninitializing */
static void __exit virmouse_uninit(void)
{
        /* Unregister from the input subsystem */
        input_unregister_device(virmouse_input_dev);

        /* Unregister driver */
        platform_device_unregister(virmouse_dev);

        return;
}

/* Driver Uninitializing */
static void __exit virkbd_uninit(void)
{
        /* Unregister from the input subsystem */
        input_unregister_device(virkbd_input_dev);

        /* Unregister driver */
        platform_device_unregister(virkbd_dev);

        return;
}

static void __exit vircon_exit(void)
{
	platform_device_unregister(vircon_device);
	platform_driver_unregister(&vircon_driver);
}

/* Driver Uninitializing */
void vircon_uninit(void)
{
	vircon_exit();
	virkbd_uninit();
	virmouse_uninit();

        return;
}

module_exit(vircon_uninit);

MODULE_AUTHOR("Dirk Herrendoerfer <d.herrendoerfer@herrendoerfer.name>");
MODULE_DESCRIPTION("Virtual Console Driver");
MODULE_LICENSE("GPL");
#endif				/* MODULE */


/*
 * uvc_state - USB Video Class (uvcvideo) camera settings and state
 *
 * This file is part of the LavaRnd revival fork (AI-assisted).  It is
 * covered by the GNU Lesser General Public License version 2.1, like the
 * rest of LavaRnd.  See the file COPYING.
 */


#if !defined(__LAVARND_UVC_STATE_H__)
#  define __LAVARND_UVC_STATE_H__


#  include <limits.h>


/*
 * special setting values
 *
 * Control values are in each V4L2 control's own units.  A camera that does
 * not offer a control reports UVC_UNSET for it, and setting it is ignored.
 */
#  define UVC_UNSET (INT_MIN)	/* control not offered / not known */
#  define UVC_MAX (INT_MAX)	/* preset: use the control's maximum */
#  define UVC_AUTO (-1)		/* exposure: automatic */
#  define UVC_FRAME_EXPOSURE (-2)	/* exposure: one full frame period */


/*
 * uvc_state - UVC camera settings and state
 *
 * NOTE: The mask element tells uvc_set() what to set and uvc_check() what
 *       to check.
 */
struct uvc_state {
    /* these values are read/write, in V4L2 control units */
    int bright;		/* V4L2_CID_BRIGHTNESS */
    int contrast;	/* V4L2_CID_CONTRAST */
    int colour;		/* V4L2_CID_SATURATION */
    int sharpness;	/* V4L2_CID_SHARPNESS */
    int gain;		/* V4L2_CID_GAIN */
    int exposure;	/* V4L2_CID_EXPOSURE_ABSOLUTE (100 usec), or UVC_AUTO */
    int dyn_fps;	/* V4L2_CID_EXPOSURE_AUTO_PRIORITY: 1 ==> may drop fps */
    int wb_auto;	/* V4L2_CID_AUTO_WHITE_BALANCE */
    int wb_temp;	/* V4L2_CID_WHITE_BALANCE_TEMPERATURE */
    int backlight;	/* V4L2_CID_BACKLIGHT_COMPENSATION */
    int power_line;	/* V4L2_CID_POWER_LINE_FREQUENCY: 0 ==> off */
    int height;		/* image height in pixels */
    int width;		/* image width in pixels */
    int fps;		/* camera frame rate */
    /* these values are read only */
    int minwidth;	/* minimum width in pixels */
    int maxwidth;	/* maximum width in pixels */
    int minheight;	/* minimum height in pixels */
    int maxheight;	/* maximum height in pixels */
    int bufsize;	/* total size of all mmap buffers */
    int framesize;	/* size of a frame in octets */
    int frames;		/* number of mmap buffers */
    int depth;		/* pixel depth in bits */
    int palette;	/* Video4Linux palette (VIDEO_PALETTE_YUYV) */
    char card[32];	/* V4L2 card name */
    /* special driver/utilities elements */
    unsigned long mask;	/* bit mask of state values to set/change */
    int channel;	/* video input number, usually 0 */
    /* tmp sanity check into - initialized by default, set by uvc_argv() */
    int tmp_top_x;		/* tmp top_x struct opsize value */
    double tmp_min_fract;	/* tmp min_fract struct opsize value */
    int tmp_half_x;		/* tmp half_x struct opsize value */
    double tmp_diff_fract;	/* tmp diff_fract struct opsize value */
};


/*
 * state mask - what is set/modify
 */
#  define UVC_STATE_bright	0x00000001
#  define UVC_STATE_contrast	0x00000002
#  define UVC_STATE_colour	0x00000004
#  define UVC_STATE_sharpness	0x00000008
#  define UVC_STATE_gain	0x00000010
#  define UVC_STATE_exposure	0x00000020
#  define UVC_STATE_dyn_fps	0x00000040
#  define UVC_STATE_wb_auto	0x00000080
#  define UVC_STATE_wb_temp	0x00000100
#  define UVC_STATE_backlight	0x00000200
#  define UVC_STATE_power_line	0x00000400
#  define UVC_STATE_height	0x00000800
#  define UVC_STATE_width	0x00001000
#  define UVC_STATE_fps		0x00002000

#  define UVC_STATE_MASK	0x00003fff

#  define UVC_MASK(name) UVC_STATE_##name
#  define UVC_SET(x,name) ((x) |= UVC_MASK(name))
#  define UVC_CLEAR(x,name) ((x) &= ~UVC_MASK(name))
#  define UVC_TEST(x,name) ((x) & UVC_MASK(name))


#endif /* __LAVARND_UVC_STATE_H__ */

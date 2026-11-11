/*
 * pwc_drvr - perform Philips specific web camera operations
 *
 * @(#) $Revision: 10.5 $
 * @(#) $Id: pwc_drvr.c,v 10.5 2003/11/15 20:37:56 lavarnd Exp $
 *
 * Copyright (c) 2000-2003 by Landon Curt Noll and Simon Cooper.
 * All Rights Reserved.
 *
 * This is open software; you can redistribute it and/or modify it under
 * the terms of the version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This software is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General
 * Public License for more details.
 *
 * The file COPYING contains important info about Licenses and Copyrights.
 * Please read the COPYING file for details about this open software.
 *
 * A copy of version 2.1 of the GNU Lesser General Public License is
 * distributed with calc under the filename COPYING-LGPL.  You should have
 * received a copy with calc; if not, write to Free Software Foundation, Inc.
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * For more information on LavaRnd: http://www.LavaRnd.org
 *
 * Share and enjoy! :-)
 */


/*
 * NOTE: This is the raw interface to a specific camera.  You should be
 *       calling the lavacam_*() routines in camop.c instead of directly
 *       calling these functions.  For example, call lavacam_open() instead
 *       of pwc_open.  See the comment at the top of src/lib/camop.c for
 *       sample code.  See also some of the tools under src/tool as well.
 */


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

#include "LavaRnd/rawio.h"
#include "LavaRnd/lavacam.h"
#include "LavaRnd/lavaerr.h"

#include "LavaRnd/have/cam_videodev.h"
#include "LavaRnd/have/have_pselect.h"

#include <sys/ioctl.h>
#include <linux/videodev2.h>


/*
 * Special PWC driver debugging - not related to LavaRnd debugging
 */
#if defined(LAVA_PWC_DEBUG)
#  define DBG(c) fprintf(stderr, \
			  "Error: %s:%d: %d\n", __FILE__, __LINE__, (c))
#  define DBGW(s) fprintf(stderr, \
			  "Warning: %s:%d: %s\n", __FILE__, __LINE__, (s))
#  define DBGS(s,v) fprintf(stderr, \
			  "Set: %s:%d: %s = %d\n", __FILE__, __LINE__ ,(s) ,(v))
#else
#  define DBG(c)
#  define DBGW(c)
#  define DBGS(s,v)
#endif


/*
 * V4L2 port notes
 *
 * This driver was originally written against Video4Linux 1 and the
 * private VIDIOCPWC* ioctls of the 2003-era pwc 8.x driver.  Linux removed
 * V4L1 in 2.6.38, and the modern pwc driver exposes the same camera
 * settings as V4L2 controls instead.  This file now talks V4L2 only.
 *
 * The struct pwc_state values keep their original V4L1 meaning and range
 * (for example picture values 0..PWC_MAX_PICT, AGC 0..PWC_MAX_AGC) so that
 * command line flags, the LavaRnd optimal settings below, and saved
 * configurations keep working.  They are scaled linearly onto the range
 * each V4L2 control reports via VIDIOC_QUERYCTRL.
 *
 * Mapping (old pwc setting => V4L2 control):
 *
 *	bright		V4L2_CID_BRIGHTNESS
 *	hue		V4L2_CID_HUE (not offered by pwc; ignored)
 *	colour		V4L2_CID_SATURATION
 *	contrast	V4L2_CID_CONTRAST
 *	gamma		V4L2_CID_GAMMA
 *	height/width	VIDIOC_S_FMT (V4L2_PIX_FMT_YUV420)
 *	fps		VIDIOC_S_PARM timeperframe
 *	compress	none (the driver chooses compression; ignored)
 *	agc		V4L2_CID_AUTOGAIN + V4L2_CID_GAIN
 *	speed		V4L2_CID_EXPOSURE_AUTO (if offered) + V4L2_CID_EXPOSURE
 *	white_mode	V4L2_CID_AUTO_WHITE_BALANCE (pwc menu; same 0..4 values)
 *	white_red/blue	V4L2_CID_RED_BALANCE / V4L2_CID_BLUE_BALANCE
 *	on/off (LED)	none (removed from the driver; ignored)
 *	sharpness	pwc private auto contour + contour controls
 *	backlight	V4L2_CID_BACKLIGHT_COMPENSATION
 *	flicker		V4L2_CID_BAND_STOP_FILTER
 *	dyn_noise	pwc private noise reduction control
 *	save/restore	pwc private save/restore user settings buttons
 */


/*
 * pwc driver private V4L2 control IDs
 *
 * These come from drivers/media/usb/pwc/pwc.h in the Linux kernel, which
 * is not exported to userspace headers.
 */
#define PWC_CID_CUSTOM(n)	((V4L2_CID_USER_BASE | 0xf000) + (n))
#define PWC_CID_AUTO_CONTOUR	PWC_CID_CUSTOM(0)
#define PWC_CID_CONTOUR		PWC_CID_CUSTOM(1)
#define PWC_CID_NOISE_REDUCTION	PWC_CID_CUSTOM(2)
#define PWC_CID_SAVE_USER	PWC_CID_CUSTOM(5)
#define PWC_CID_RESTORE_USER	PWC_CID_CUSTOM(6)

#define PWC_V4L2_BUFS (4)	/* number of V4L2 mmap buffers to request */
#define PWC_DQBUF_TRIES (8)	/* max errored buffers to skip per frame */

/* pwc_ctrl_set_scaled() return values */
#define PWC_CTRL_OK (0)		/* control was set */
#define PWC_CTRL_MISSING (-1)	/* driver does not offer this control */
#define PWC_CTRL_FAILED (-2)	/* control exists but could not be set */


/*
 * pwc_xioctl - ioctl that retries when interrupted by a signal
 */
static int
pwc_xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
	ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}


/*
 * pwc_ctrl_query - look up a V4L2 control
 *
 * returns:
 *      0 ==> control exists and is enabled, <0 ==> not available
 */
static int
pwc_ctrl_query(int fd, __u32 id, struct v4l2_queryctrl *q)
{
    memset(q, 0, sizeof(*q));
    q->id = id;
    if (pwc_xioctl(fd, VIDIOC_QUERYCTRL, q) < 0) {
	return -1;
    }
    if (q->flags & V4L2_CTRL_FLAG_DISABLED) {
	return -1;
    }
    return 0;
}


/*
 * pwc_ctrl_get - get the raw value of a V4L2 control
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 */
static int
pwc_ctrl_get(int fd, __u32 id, int *val)
{
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    if (pwc_xioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
	return -1;
    }
    *val = ctrl.value;
    return 0;
}


/*
 * pwc_ctrl_set - set the raw value of a V4L2 control
 *
 * returns:
 *      PWC_CTRL_OK, PWC_CTRL_MISSING or PWC_CTRL_FAILED
 */
static int
pwc_ctrl_set(int fd, __u32 id, int val)
{
    struct v4l2_queryctrl q;
    struct v4l2_control ctrl;

    if (pwc_ctrl_query(fd, id, &q) < 0) {
	return PWC_CTRL_MISSING;
    }
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = val;
    if (pwc_xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
	return PWC_CTRL_FAILED;
    }
    return PWC_CTRL_OK;
}


/*
 * pwc_scale_to_ctrl - map a value in [0, full] onto a control's range
 */
static int
pwc_scale_to_ctrl(int val, int full, const struct v4l2_queryctrl *q)
{
    long long span = (long long)q->maximum - (long long)q->minimum;
    long long v;

    if (val < 0) {
	val = 0;
    } else if (val > full) {
	val = full;
    }
    v = q->minimum + ((long long)val * span + full / 2) / full;
    if (q->step > 1) {
	v = q->minimum + ((v - q->minimum + q->step / 2) / q->step) * q->step;
    }
    if (v > q->maximum) {
	v = q->maximum;
    }
    return (int)v;
}


/*
 * pwc_scale_from_ctrl - map a control value onto [0, full]
 */
static int
pwc_scale_from_ctrl(int val, int full, const struct v4l2_queryctrl *q)
{
    long long span = (long long)q->maximum - (long long)q->minimum;

    if (span <= 0) {
	return 0;
    }
    return (int)((((long long)val - q->minimum) * full + span / 2) / span);
}


/*
 * pwc_ctrl_get_scaled - get a control value scaled onto [0, full]
 *
 * returns:
 *      0 ==> OK, <0 ==> control not available
 */
static int
pwc_ctrl_get_scaled(int fd, __u32 id, int full, int *out)
{
    struct v4l2_queryctrl q;
    int val;

    if (pwc_ctrl_query(fd, id, &q) < 0 || pwc_ctrl_get(fd, id, &val) < 0) {
	return -1;
    }
    *out = pwc_scale_from_ctrl(val, full, &q);
    return 0;
}


/*
 * pwc_ctrl_set_scaled - set a control from a value in [0, full]
 *
 * returns:
 *      PWC_CTRL_OK, PWC_CTRL_MISSING or PWC_CTRL_FAILED
 */
static int
pwc_ctrl_set_scaled(int fd, __u32 id, int full, int val)
{
    struct v4l2_queryctrl q;

    if (pwc_ctrl_query(fd, id, &q) < 0) {
	return PWC_CTRL_MISSING;
    }
    return pwc_ctrl_set(fd, id, pwc_scale_to_ctrl(val, full, &q));
}


/*
 * pwc_identify - verify that the device is a V4L2 capture device run by pwc
 *
 * given:
 *      cam_fd      open camera descriptor
 *      vcap        where to place the V4L2 capability information
 *      camtype     where to place the Philips camera type, or 0 if unknown
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 *
 * NOTE: The V4L1 pwc driver reported the numeric Philips camera type
 *       (730, 740, ...).  The V4L2 driver does not, unless the card name
 *       happens to be of the form "Philips %d webcam".
 */
static int
pwc_identify(int cam_fd, struct v4l2_capability *vcap, int *camtype)
{
    __u32 caps;		/* capabilities of this device node */

    memset(vcap, 0, sizeof(*vcap));
    if (pwc_xioctl(cam_fd, VIDIOC_QUERYCAP, vcap) < 0) {
	return LAVACAM_ERR_NOCAM;
    }
    if (vcap->capabilities & V4L2_CAP_DEVICE_CAPS) {
	caps = vcap->device_caps;
    } else {
	caps = vcap->capabilities;
    }
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
	return LAVACAM_ERR_NOCAM;
    }
    if (strncmp((char *)vcap->driver, "pwc", sizeof(vcap->driver)) != 0) {
	/* not run by the Philips webcam driver */
	return LAVACAM_ERR_TYPE;
    }
    *camtype = 0;
    if (sscanf((char *)vcap->card, "Philips %d webcam", camtype) < 1 ||
	*camtype < 0) {
	*camtype = 0;
    }
    return LAVACAM_ERR_OK;
}


/*
 * pwc_stream_stop - stop V4L2 streaming and release mmapped buffers
 */
static void
pwc_stream_stop(int cam_fd, struct opsize *siz)
{
    struct v4l2_requestbuffers req;	/* buffer release request */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i;

    (void)pwc_xioctl(cam_fd, VIDIOC_STREAMOFF, &type);
    for (i = 0; i < siz->nbufs; ++i) {
	if (siz->buf_start[i] != NULL && siz->buf_start[i] != MAP_FAILED) {
	    (void)munmap(siz->buf_start[i], siz->buf_len[i]);
	}
	siz->buf_start[i] = NULL;
	siz->buf_len[i] = 0;
    }
    siz->nbufs = 0;
    siz->cur_buf = -1;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    (void)pwc_xioctl(cam_fd, VIDIOC_REQBUFS, &req);
}


/*
 * pwc_stream_start - mmap V4L2 capture buffers, queue them, start streaming
 *
 * given:
 *      cam_fd      open camera descriptor
 *      siz         pointer to operation size and buffer structure
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 *
 * NOTE: On success siz->image and siz->chaos point into the first buffer.
 *       They are moved to the most recent frame by pwc_get_frame().
 */
static int
pwc_stream_start(int cam_fd, struct opsize *siz)
{
    struct v4l2_requestbuffers req;	/* buffer allocation request */
    struct v4l2_buffer buf;		/* buffer description */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i;

    siz->nbufs = 0;
    siz->cur_buf = -1;

    /*
     * ask the driver for capture buffers
     */
    memset(&req, 0, sizeof(req));
    req.count = PWC_V4L2_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (pwc_xioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
	DBG(LAVACAM_ERR_NOMMAP);
	return LAVACAM_ERR_NOMMAP;
    }
    if (req.count > VIDEO_MAX_FRAME) {
	req.count = VIDEO_MAX_FRAME;
    }

    /*
     * mmap and queue each buffer
     */
    for (i = 0; i < (int)req.count; ++i) {
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = i;
	if (pwc_xioctl(cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
	    pwc_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOMMAP);
	    return LAVACAM_ERR_NOMMAP;
	}
	if ((int)buf.length < siz->framesize) {
	    pwc_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOSIZE);
	    return LAVACAM_ERR_NOSIZE;
	}
	siz->buf_len[i] = buf.length;
	siz->buf_start[i] = mmap(NULL, buf.length, PROT_READ, MAP_SHARED,
				 cam_fd, buf.m.offset);
	siz->nbufs = i + 1;
	if (siz->buf_start[i] == MAP_FAILED) {
	    siz->buf_start[i] = NULL;
	    pwc_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOMMAP);
	    return LAVACAM_ERR_NOMMAP;
	}
	if (pwc_xioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
	    pwc_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOMMAP);
	    return LAVACAM_ERR_NOMMAP;
	}
    }

    /*
     * start capturing
     */
    if (pwc_xioctl(cam_fd, VIDIOC_STREAMON, &type) < 0) {
	pwc_stream_stop(cam_fd, siz);
	DBG(LAVACAM_ERR_NOMMAP);
	return LAVACAM_ERR_NOMMAP;
    }
    siz->image = siz->buf_start[0];
    siz->chaos = (u_int8_t *)siz->image + siz->mmap_lavaoff;
    return LAVACAM_ERR_OK;
}


/*
 * LavaRnd entropy optimal values
 *
 * These magic numbers were determine by experiment with a Philips 730
 * (also known as a Logitech ProCam 3000) and the Philips 740.
 */
static struct pwc_state cam_730740 = {  		/* guess 730==740 */
    /* these values are read/write */
    PWC_MAX_PICT,		/* max brightness level */
    PWC_MAX_PICT,		/* max hue */
    PWC_MAX_PICT,		/* max colour saturation */
    PWC_MAX_PICT,		/* max image contrast level */
    PWC_MAX_PICT,		/* max brightness gamma */
    120,			/* image height in pixels */
    160,			/* image width in pixels */
    30,				/* camera frame rate */
    PWC_MIN_COMPRESSION,	/* no camera compression */
    PWC_WB_FL,			/* Florescent white balance mode */
    31488,			/* manual white balance red level - ignored */
    21248,			/* manual white balance blue level - ignored */
    0,				/* no LED on time */
    0,				/* all LED off time */
    PWC_MAX_SHARPNESS,		/* max electronic sharpness */
    0,				/* no back-light compression */
    0,				/* do not suppress flicker */
    PWC_MIN_DYNNOISE,		/* no dynamic noise reduction */
    /* there values are write only */
    PWC_MAX_AGC,		/* manual gain control */
    PWC_MAX_SPEED,		/* shutter speed (larger==slower) */
    /* these values are read only */
    0,				/* numerical camera subtype */
    0,				/* minimum width in pixels */
    0,				/* maximum width in pixels */
    0,				/* minimum height in pixels */
    0,				/* maximum height in pixels */
    0,				/* current white balance red level */
    0,				/* current white balance blue level */
    0,				/* buffer size in pixels */
    0,				/* size of a frame in octets */
    0,				/* number of video buffer frames */
    0,				/* pixel/picture depth in bits */
    0,				/* type of image palette */
    /* special driver/utilities elements */
    PWC_STATE_MASK,		/* bit mask of state values to set/change */
    0,				/* video channel num, usually 0, set on open */
    /* tmp sanity check into - initialized by default, set by pwc_argv() */
    0,
    0.0,
    0,
    0.0
};


/*
 * LavaRnd values for given models
 */
struct lava_state {
    int model;			/* camera model - see struct camop in camop.c */
    struct pwc_state *state;	/* optimal LavaRnd entropy values */
    double warmup;		/* default seconds to delay during open */
    int def_top_x;		/* default ignore top_x octets as common */
    double def_min_fract;	/* default uncommon octet value fraction */
    int def_half_x;		/* default 1/2 level value */
    double def_diff_fract;	/* default different/same fraction */
};

/*
 * specific pwc camera settings
 *
 * The a 4-sigma margin of the average of 131072 frames as reported by
 * the camsantiy tool:
 *
 *	camsanity pwc730 /dev/video0 /var/tmp/pwc730_sanity 131072 64 \
 *		  -L -T 10 -x 0 -d 0 -v 2
 *
 * with the pwc modules.conf setting of:
 *
 *	options pwc size=vga fps=15 leds=250,750 fbufs=2 mbufs=1 compression=0
 *
 * for a Philips / Logitech 3000 Pro pwc 730 camera:
 *
 *   top_17
 *
 *      The def_top_x of 17 was picked because the average top_17 level was
 *      near 0.6826895 (1-sigma).
 *
 *   top_17 = 0.557056
 *
 * 	For top_17, the min bitdiff_fract was 0.570573 and standard deviation
 *	was 0.002703.  We go 5 sigma below the min for a margin of error.
 *
 *   half_x = 20
 *
 *      The smallest level with a max uncom_fract > 0.5 was 22 and the
 *	standard deviation was 0.262421.  We go 5 sigma below the min
 *	for a margin of error.
 *
 *   diff_fract = 0.300359
 *
 *	The min bitdiff_fract was 0.308092 and standard deviation
 *	was 0.001547.  We go 5 sigma below the min for a margin of error.
 *
 * Therefore a wide margin of error is used in the lava_state below:
 *
 *    def_top_x = 17
 *    def_min_fract = 0.557056
 *    def_half_x = 20
 *    def_diff_fract = 0.300359
 *
 * If your camera produces insane frames, then perhaps:
 *
 *      your camera is broken and behaving poorly
 *      your camera may be someout unusual, tuning with -x, -X, -d, -2
 *          values might be the answer if you do not have to
 *          tune too far
 *      light is leaking into your camera ... cover it up?
 *      your camera is too cold to generate enough thermal noise
 *
 * If you want to disable, by default, sanity checking, inialize
 * the following lava_state structure elements as follows:
 *
 *	def_top_x = 0
 *	def_min_fract = 0.0
 *	def_half_x = 0
 *	def_diff_fract = 0.0
 *
 * The sanity check values should be dynamically determined so that
 * individual camera and camera enviroment differences can be
 * accounted for automatically. - XXX
 */
static struct lava_state lava_state[] = {
    {730, &cam_730740, 3.0, 17, 0.557056, 20, 0.300359},
    {740, &cam_730740, 3.0, 17, 0.520855, 20, 0.318406},
    /*
     * Logitech QuickCam Pro 4000 (USB 046d:08b2, a Philips 740-family
     * camera) on the V4L2 pwc driver, measured 2026-11-11 with the case
     * sealed: 80459 frames of
     *
     *	camsanity pwc740 /dev/video0 DIR 131072 64 -L -T 10 -x 0 -d 0
     *
     * (stopped early).  Limits are 5 standard deviations below the
     * observed minimums, using true standard deviations; upstream's
     * camsanity stddev() understates them.  See PORTING.md.
     *
     *	top_17 uncommon fraction: min 0.676562  sd 0.003092
     *	half level:		  min 27        sd 0.4934
     *	bitdiff fraction:	  min 0.411445  sd 0.001258
     */
    {4000, &cam_730740, 3.0, 17, 0.661101, 24, 0.405155},

    {-1, NULL, 0.0, 0, 0, 0.0, 0.0}	/* must be last */
};

#define STATE_COUNT ((int)sizeof(lava_state) / (int)sizeof(lava_state[0]))
#define STARTUP_FRAMES (5)	/* try to read at least these frames */


/*
 * pwc_model - determine which LavaRnd value model applies
 *
 * given:
 *      model       camera model number
 *
 * returns:
 *      index into lava_state[] to use, or
 *            <0 (LAVACAM_ERR_IDENT) ==> error
 */
static int
pwc_model(int model)
{
    int i;

    /*
     * look for default state
     */
    for (i = 0; i < STATE_COUNT; ++i) {
	if (model == lava_state[i].model) {
	    break;
	}
    }
    if (i >= STATE_COUNT || lava_state[i].model < 0) {
	return LAVACAM_ERR_IDENT;
    }
    return i;
}


/*
 * pwc_opsize - return read/mmap operation size and buffer structure
 *
 * given:
 *      cam_fd      open camera descriptor
 *      u_cam_p     pointer to camera state
 *      siz         pointer to operation size and buffer structure
 *      flag        flags set via lavacam_argv()
 *
 * returns:
 *       0 ==> OK, read/mmap size, <0 ==> error
 *
 * NOTE: This function is only called from within pwc_open().
 *
 * NOTE: This function loads the working sanity check values from
 *       the temp driver check values.
 *
 * NOTE: This function sets the bufsize, framesize and frames.
 *
 * NOTE: With V4L2 each mmapped buffer holds exactly one frame, so the
 *	 mmap LavaRnd offset is relative to the start of a buffer.
 */
static int
pwc_opsize(int cam_fd, union lavacam *u_cam_p, struct opsize *siz,
	   struct lavacam_flag *flag)
{
    struct pwc_state *cam;	/* u_cam_p as pwc union element */
    struct v4l2_format fmt;	/* current capture format */
    int chaos_offset;		/* offset in frame for chaotic data */
    int chaos_len;		/* length of chaotic data in frame */
    int ret;			/* return code */

    /*
     * firewall
     */
    if (cam_fd < 0 || u_cam_p == NULL || siz == NULL || flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * pick out the driver specific union element
     */
    cam = &(u_cam_p->pwc);

    /*
     * initialize opsize structure
     */
    memset(siz, 0, sizeof(*siz));
    siz->prev_frame = NULL;
    siz->image = NULL;
    siz->chaos = NULL;
    siz->cur_buf = -1;

    /*
     * copy over tmp sanity check files into working struct opsize elements
     */
    siz->top_x = cam->tmp_top_x;
    siz->min_fract = cam->tmp_min_fract;
    siz->half_x = cam->tmp_half_x;
    siz->diff_fract = cam->tmp_diff_fract;

    /*
     * determine frame size from the driver's current format
     */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (pwc_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	DBG(LAVACAM_ERR_GETPARAM);
	return LAVACAM_ERR_GETPARAM;
    }
    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    ret = frame_size(PALLETTE_VIDEO4LINUX, cam->palette, 0, 0, NULL,
		     cam->height, cam->width);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }
    if (fmt.fmt.pix.sizeimage > 0 && (int)fmt.fmt.pix.sizeimage != ret) {
	/* driver frame layout is not what the palette code expects */
	DBG(LAVACAM_ERR_NOSIZE);
	return LAVACAM_ERR_NOSIZE;
    }
    cam->framesize = ret;
    cam->frames = PWC_V4L2_BUFS;
    cam->bufsize = cam->frames * cam->framesize;

    /*
     * determine location of chaotic data within a frame
     */
    ret = chaos_zone(PALLETTE_VIDEO4LINUX, cam->palette, cam->framesize,
    		     cam->height, cam->width, &chaos_offset, &chaos_len);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }

    /*
     * determine mmap parameters
     */
    siz->mmapsize = cam->bufsize;
    if (flag->A_flag) {
	/* -A ==> use all the frame */
	siz->mmap_lavaoff = 0;
	siz->mmap_lavalen = cam->framesize;
    } else {
	/* use the chaos part of the frame */
	siz->mmap_lavaoff = chaos_offset;
	siz->mmap_lavalen = chaos_len;
    }

    /*
     * determine read parameters
     */
    siz->readsize = cam->framesize;
    if (flag->A_flag) {
	/* -A ==> use all the frame */
	siz->read_lavaoff = 0;
	siz->read_lavalen = cam->framesize;
    } else {
	/* use the chaos part of the frame */
	siz->read_lavaoff = chaos_offset;
	siz->read_lavalen = chaos_len;
    }

    /*
     * fill in the final opsize information from the camera's state
     */
    siz->frames = cam->frames;
    siz->palette = cam->palette;
    siz->height = cam->height;
    siz->width = cam->width;
    siz->framesize = cam->framesize;

    /*
     * data is complete
     */
    return LAVACAM_ERR_OK;
}


/*
 * pwc_get - get an open camera's state
 *
 * given:
 *      cam_fd      open camera descriptor
 *      u_cam_p     pointer to values of the camera
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 *
 * NOTE: This function will clear out the tmp sanity check values.
 *
 * NOTE: This function only reads camera state.  It may be called while
 *       the camera is streaming.
 */
int
pwc_get(int cam_fd, union lavacam *u_cam_p)
{
    struct v4l2_capability vcap;	/* video capability */
    struct v4l2_format fmt;		/* current capture format */
    struct v4l2_streamparm parm;	/* streaming parameters */
    struct v4l2_frmsizeenum fsize;	/* supported frame size */
    struct v4l2_queryctrl q;		/* control information */
    struct pwc_state *cam;		/* u_cam_p as pwc union element */
    int camtype;			/* camera type */
    int val;				/* control value */
    int ret;				/* return code */
    int i;

    /*
     * firewall
     */
    if (cam_fd < 0 || u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * pick out the driver specific union element
     */
    cam = &(u_cam_p->pwc);

    /*
     * initialize cam state
     */
    memset(cam, 0, sizeof(cam[0]));

    /*
     * verify that we have a Philips camera
     */
    ret = pwc_identify(cam_fd, &vcap, &camtype);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }
    cam->subtype = camtype;

    /*
     * determine the supported frame size range
     */
    for (i = 0; ; ++i) {
	int minw, maxw, minh, maxh;	/* this frame size entry range */

	memset(&fsize, 0, sizeof(fsize));
	fsize.index = i;
	fsize.pixel_format = V4L2_PIX_FMT_YUV420;
	if (pwc_xioctl(cam_fd, VIDIOC_ENUM_FRAMESIZES, &fsize) < 0) {
	    break;
	}
	if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
	    minw = maxw = fsize.discrete.width;
	    minh = maxh = fsize.discrete.height;
	} else {
	    minw = fsize.stepwise.min_width;
	    maxw = fsize.stepwise.max_width;
	    minh = fsize.stepwise.min_height;
	    maxh = fsize.stepwise.max_height;
	}
	if (cam->minwidth == 0 || minw < cam->minwidth) {
	    cam->minwidth = minw;
	}
	if (maxw > cam->maxwidth) {
	    cam->maxwidth = maxw;
	}
	if (cam->minheight == 0 || minh < cam->minheight) {
	    cam->minheight = minh;
	}
	if (maxh > cam->maxheight) {
	    cam->maxheight = maxh;
	}
	if (fsize.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
	    break;
	}
    }

    /*
     * get current picture values
     */
    cam->bright = PWC_UNSET_PICT;
    cam->hue = PWC_UNSET_PICT;
    cam->colour = PWC_UNSET_PICT;
    cam->contrast = PWC_UNSET_PICT;
    cam->gamma = PWC_UNSET_PICT;
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_BRIGHTNESS, PWC_MAX_PICT,
			      &cam->bright);
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_HUE, PWC_MAX_PICT, &cam->hue);
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_SATURATION, PWC_MAX_PICT,
			      &cam->colour);
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_CONTRAST, PWC_MAX_PICT,
			      &cam->contrast);
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_GAMMA, PWC_MAX_PICT,
			      &cam->gamma);

    /*
     * get the current capture format
     *
     * Only planar YUV 4:2:0 is useful to LavaRnd.  Any other format is
     * reported as an unset palette so that pwc_open() will select YUV 4:2:0.
     */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (pwc_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	DBG(LAVACAM_ERR_GETPARAM);
	return LAVACAM_ERR_GETPARAM;
    }
    cam->height = fmt.fmt.pix.height;
    cam->width = fmt.fmt.pix.width;
    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420) {
	cam->palette = VIDEO_PALETTE_YUV420P;
	cam->depth = 12;
    } else {
	cam->palette = 0;
	cam->depth = PWC_UNSET_PICT;
    }

    /*
     * get the frame rate
     */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (pwc_xioctl(cam_fd, VIDIOC_G_PARM, &parm) == 0 &&
	parm.parm.capture.timeperframe.numerator > 0) {
	cam->fps = parm.parm.capture.timeperframe.denominator /
		   parm.parm.capture.timeperframe.numerator;
    } else {
	cam->fps = 0;
    }

    /*
     * the V4L2 pwc driver chooses compression on its own
     */
    cam->compress = PWC_MIN_COMPRESSION - 1;

    /*
     * get white balance
     */
    cam->white_mode = PWC_MIN_WH_BAL_MODE - 1;
    if (pwc_ctrl_query(cam_fd, V4L2_CID_AUTO_WHITE_BALANCE, &q) == 0 &&
	pwc_ctrl_get(cam_fd, V4L2_CID_AUTO_WHITE_BALANCE, &val) == 0) {
	cam->white_mode = val;
    }
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_RED_BALANCE,
			      PWC_MAX_WH_BAL_LEVEL, &cam->white_red);
    (void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_BLUE_BALANCE,
			      PWC_MAX_WH_BAL_LEVEL, &cam->white_blue);
    cam->read_red = cam->white_red;
    cam->read_blue = cam->white_blue;

    /*
     * the V4L2 pwc driver has no LED control
     */
    cam->on = PWC_UNSET_LED_TIME;
    cam->off = PWC_UNSET_LED_TIME;

    /*
     * get electronic sharpness (contour) value
     */
    cam->sharpness = PWC_MIN_SHARPNESS - 1;
    if (pwc_ctrl_query(cam_fd, PWC_CID_AUTO_CONTOUR, &q) == 0 &&
	pwc_ctrl_get(cam_fd, PWC_CID_AUTO_CONTOUR, &val) == 0 && val != 0) {
	cam->sharpness = PWC_AUTO_SHARPNESS;
    } else {
	(void)pwc_ctrl_get_scaled(cam_fd, PWC_CID_CONTOUR, PWC_MAX_SHARPNESS,
				  &cam->sharpness);
    }

    /*
     * get back light compensation mode
     */
    cam->backlight = PWC_UNSET_BACKLIGHT;
    if (pwc_ctrl_query(cam_fd, V4L2_CID_BACKLIGHT_COMPENSATION, &q) == 0 &&
	pwc_ctrl_get(cam_fd, V4L2_CID_BACKLIGHT_COMPENSATION, &val) == 0) {
	cam->backlight = val;
    }

    /*
     * get flicker suppression mode
     */
    cam->flicker = PWC_UNSET_FLICKER;
    if (pwc_ctrl_query(cam_fd, V4L2_CID_BAND_STOP_FILTER, &q) == 0 &&
	pwc_ctrl_get(cam_fd, V4L2_CID_BAND_STOP_FILTER, &val) == 0) {
	cam->flicker = val;
    }

    /*
     * get dynamic noise reduction value
     */
    cam->dyn_noise = PWC_MIN_DYNNOISE - 1;
    if (pwc_ctrl_query(cam_fd, PWC_CID_NOISE_REDUCTION, &q) == 0 &&
	pwc_ctrl_get(cam_fd, PWC_CID_NOISE_REDUCTION, &val) == 0) {
	cam->dyn_noise = val;
    }

    /*
     * get gain control
     *
     * NOTE: Unlike the V4L1 driver, the V4L2 driver can report AGC.
     */
    cam->agc = PWC_MIN_AGC - 1;
    if (pwc_ctrl_query(cam_fd, V4L2_CID_AUTOGAIN, &q) == 0 &&
	pwc_ctrl_get(cam_fd, V4L2_CID_AUTOGAIN, &val) == 0 && val != 0) {
	cam->agc = PWC_AUTO_AGC;
    } else {
	(void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_GAIN, PWC_MAX_AGC,
				  &cam->agc);
    }

    /*
     * get shutter speed (exposure)
     *
     * NOTE: Unlike the V4L1 driver, the V4L2 driver can report exposure.
     *	     Without a separate auto exposure control, exposure is
     *	     automatic whenever gain is automatic.
     */
    cam->speed = PWC_MIN_SPEED - 1;
    if (pwc_ctrl_query(cam_fd, V4L2_CID_EXPOSURE_AUTO, &q) == 0) {
	if (pwc_ctrl_get(cam_fd, V4L2_CID_EXPOSURE_AUTO, &val) == 0 &&
	    val != V4L2_EXPOSURE_MANUAL) {
	    cam->speed = PWC_AUTO_SPEED;
	}
    } else if (cam->agc == PWC_AUTO_AGC) {
	cam->speed = PWC_AUTO_SPEED;
    }
    if (cam->speed != PWC_AUTO_SPEED) {
	(void)pwc_ctrl_get_scaled(cam_fd, V4L2_CID_EXPOSURE, PWC_MAX_SPEED,
				  &cam->speed);
    }

    /*
     * buffer information is set by pwc_opsize() when the camera is opened
     */
    cam->bufsize = 0;
    cam->framesize = 0;
    cam->frames = 0;

    /*
     * all done
     */
    return LAVACAM_ERR_OK;
}


/*
 * pwc_set - set an open camera's state
 *
 * given:
 *      cam_fd      open camera descriptor
 *      u_cam_p     pointer to new camera setting if mask is non-zero
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 *
 * NOTE: The tmp sanity check values are ignored by this function.
 *
 * NOTE: The capture size and frame rate cannot be changed while the
 *	 camera is streaming.  pwc_open() calls this before streaming starts.
 */
int
pwc_set(int cam_fd, union lavacam *u_cam_p)
{
    struct v4l2_capability vcap;	/* video capability */
    struct v4l2_format fmt;		/* capture format */
    struct v4l2_streamparm parm;	/* streaming parameters */
    struct v4l2_queryctrl q;		/* control information */
    struct pwc_state *cam;		/* u_cam_p as pwc union element */
    int camtype;			/* camera type */
    int val;				/* control value */
    int ret;				/* return code */

    /*
     * firewall
     */
    if (cam_fd < 0 || u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * pick out the driver specific union element
     */
    cam = &(u_cam_p->pwc);

    /*
     * quick exit if mask is empty
     */
    if (cam->mask == 0) {
	return LAVACAM_ERR_OK;
    }

    /*
     * verify that we have a Philips camera
     */
    ret = pwc_identify(cam_fd, &vcap, &camtype);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }

    /*
     * set picture values if requested
     *
     * A control the driver does not offer (such as hue) is ignored,
     * as the V4L1 driver silently ignored picture values it did not use.
     */
#define PWC_SET_PICT(field, id) \
    if (PWC_TEST(cam->mask, field)) { \
	if (pwc_ctrl_set_scaled(cam_fd, (id), PWC_MAX_PICT, cam->field) == \
	    PWC_CTRL_FAILED) { \
	    DBG(LAVACAM_ERR_SETPARAM); \
	    return LAVACAM_ERR_SETPARAM; \
	} \
	DBGS(#field, cam->field); \
    }
    PWC_SET_PICT(bright, V4L2_CID_BRIGHTNESS);
    PWC_SET_PICT(hue, V4L2_CID_HUE);
    PWC_SET_PICT(colour, V4L2_CID_SATURATION);
    PWC_SET_PICT(contrast, V4L2_CID_CONTRAST);
    PWC_SET_PICT(gamma, V4L2_CID_GAMMA);
#undef PWC_SET_PICT

    /*
     * change capture size if requested
     *
     * The driver picks the nearest size it supports.
     */
    if (PWC_TEST(cam->mask, height) || PWC_TEST(cam->mask, width)) {
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (pwc_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	    DBG(LAVACAM_ERR_GETPARAM);
	    return LAVACAM_ERR_GETPARAM;
	}
	if (PWC_TEST(cam->mask, height)) {
	    fmt.fmt.pix.height = cam->height;
	    DBGS("height", cam->height);
	}
	if (PWC_TEST(cam->mask, width)) {
	    fmt.fmt.pix.width = cam->width;
	    DBGS("width", cam->width);
	}
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = 0;
	fmt.fmt.pix.sizeimage = 0;
	if (pwc_xioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
    }

    /*
     * change frame rate if requested
     */
    if (PWC_TEST(cam->mask, fps)) {
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = cam->fps;
	if (pwc_xioctl(cam_fd, VIDIOC_S_PARM, &parm) < 0) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
	DBGS("fps", cam->fps);
    }

    /*
     * compression preference cannot be set: the V4L2 driver chooses it
     */
    if (PWC_TEST(cam->mask, compress)) {
	DBGW("compression is chosen by the driver, ignored");
    }

    /*
     * set AGC if requested
     *
     * <0 ==> automatic gain, >=0 ==> manual gain level
     */
    if (PWC_TEST(cam->mask, agc)) {
	if (pwc_ctrl_set(cam_fd, V4L2_CID_AUTOGAIN, cam->agc < 0 ? 1 : 0) ==
	    PWC_CTRL_FAILED) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
	if (cam->agc >= 0 &&
	    pwc_ctrl_set_scaled(cam_fd, V4L2_CID_GAIN, PWC_MAX_AGC,
				cam->agc) != PWC_CTRL_OK) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
	DBGS("agc", cam->agc);
    }

    /*
     * set shutter speed if requested
     *
     * <0 ==> automatic, >=0 ==> manual exposure (higher ==> slower)
     */
    if (PWC_TEST(cam->mask, speed)) {
	if (pwc_ctrl_query(cam_fd, V4L2_CID_EXPOSURE_AUTO, &q) == 0) {
	    val = (cam->speed < 0) ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL;
	    if (pwc_ctrl_set(cam_fd, V4L2_CID_EXPOSURE_AUTO, val) ==
		PWC_CTRL_FAILED) {
		DBG(LAVACAM_ERR_SETPARAM);
		return LAVACAM_ERR_SETPARAM;
	    }
	}
	if (cam->speed >= 0 &&
	    pwc_ctrl_set_scaled(cam_fd, V4L2_CID_EXPOSURE, PWC_MAX_SPEED,
				cam->speed) != PWC_CTRL_OK) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
	DBGS("speed", cam->speed);
    }

    /*
     * change white balance if requested
     */
    if (PWC_TEST(cam->mask, white_mode)) {
	if (pwc_ctrl_set(cam_fd, V4L2_CID_AUTO_WHITE_BALANCE,
			 cam->white_mode) != PWC_CTRL_OK) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
	DBGS("mode", cam->white_mode);
    }
    if (PWC_TEST(cam->mask, white_red) || PWC_TEST(cam->mask, white_blue)) {
	if (pwc_ctrl_get(cam_fd, V4L2_CID_AUTO_WHITE_BALANCE, &val) < 0) {
	    DBG(LAVACAM_ERR_GETPARAM);
	    return LAVACAM_ERR_GETPARAM;
	}
	/* manual levels only matter in manual white balance mode */
	if (val == PWC_WB_MANUAL && PWC_TEST(cam->mask, white_red)) {
	    if (pwc_ctrl_set_scaled(cam_fd, V4L2_CID_RED_BALANCE,
				    PWC_MAX_WH_BAL_LEVEL, cam->white_red) !=
		PWC_CTRL_OK) {
		DBG(LAVACAM_ERR_SETPARAM);
		return LAVACAM_ERR_SETPARAM;
	    }
	    DBGS("manual_red", cam->white_red);
	}
	if (val == PWC_WB_MANUAL && PWC_TEST(cam->mask, white_blue)) {
	    if (pwc_ctrl_set_scaled(cam_fd, V4L2_CID_BLUE_BALANCE,
				    PWC_MAX_WH_BAL_LEVEL, cam->white_blue) !=
		PWC_CTRL_OK) {
		DBG(LAVACAM_ERR_SETPARAM);
		return LAVACAM_ERR_SETPARAM;
	    }
	    DBGS("manual_blue", cam->white_blue);
	}
    }

    /*
     * LED on/off times cannot be set: the V4L2 driver has no LED control
     */
    if (PWC_TEST(cam->mask, on) || PWC_TEST(cam->mask, off)) {
	DBGW("LED control is not supported by the driver, ignored");
    }

    /*
     * set electronic sharpness (contour) value, OK to fail
     */
    if (PWC_TEST(cam->mask, sharpness)) {
	if (cam->sharpness == PWC_AUTO_SHARPNESS) {
	    (void)pwc_ctrl_set(cam_fd, PWC_CID_AUTO_CONTOUR, 1);
	} else {
	    (void)pwc_ctrl_set(cam_fd, PWC_CID_AUTO_CONTOUR, 0);
	    (void)pwc_ctrl_set_scaled(cam_fd, PWC_CID_CONTOUR,
				      PWC_MAX_SHARPNESS, cam->sharpness);
	}
	DBGS("sharpness", cam->sharpness);
    }

    /*
     * set backlight compensation mode, OK to fail
     */
    if (PWC_TEST(cam->mask, backlight)) {
	(void)pwc_ctrl_set(cam_fd, V4L2_CID_BACKLIGHT_COMPENSATION,
			   cam->backlight ? 1 : 0);
	DBGS("backlight", cam->backlight);
    }

    /*
     * set flicker suppression mode, OK to fail
     */
    if (PWC_TEST(cam->mask, flicker)) {
	(void)pwc_ctrl_set(cam_fd, V4L2_CID_BAND_STOP_FILTER,
			   cam->flicker ? 1 : 0);
	DBGS("flicker", cam->flicker);
    }

    /*
     * set dynamic noise reduction value, OK to fail
     */
    if (PWC_TEST(cam->mask, dyn_noise)) {
	(void)pwc_ctrl_set(cam_fd, PWC_CID_NOISE_REDUCTION, cam->dyn_noise);
	DBGS("dyn_noise", cam->dyn_noise);
    }

    /*
     * save newly set settings as current user settings in camera's EEPROM
     */
    if (pwc_ctrl_set(cam_fd, PWC_CID_SAVE_USER, 1) == PWC_CTRL_FAILED) {
	DBG(LAVACAM_ERR_SETPARAM);
	return LAVACAM_ERR_SETPARAM;
    }

    /*
     * all done
     */
    return LAVACAM_ERR_OK;
}


/*
 * pwc_LavaRnd - preset change state with LavaRnd entropy optimal values
 *
 * given:
 *      u_cam_p     pointer to camera state
 *      model       camera model number
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 *
 * NOTE: This function will set the tmp sanity check faules to their
 *       defaults according to the lava_state[] default values.
 */
int
pwc_LavaRnd(union lavacam *u_cam_p, int model)
{
    int model_indx;

    /*
     * firewall
     */
    if (u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * look for default state index
     */
    model_indx = pwc_model(model);
    if (model_indx < 0) {
	return model_indx;
    }

    /*
     * load the LavaRnd entropy optimal values
     */
    memcpy(&(u_cam_p->pwc), lava_state[model_indx].state,
	   sizeof(u_cam_p->pwc));

    /*
     * set the default tmp sanity check values
     */
    u_cam_p->pwc.tmp_top_x = lava_state[model_indx].def_top_x;
    u_cam_p->pwc.tmp_min_fract = lava_state[model_indx].def_min_fract;
    u_cam_p->pwc.tmp_half_x = lava_state[model_indx].def_half_x;
    u_cam_p->pwc.tmp_diff_fract = lava_state[model_indx].def_diff_fract;
    return LAVACAM_ERR_OK;
}


/*
 * pwc_check - determine if the camera state change is valid
 *
 * given:
 *      u_cam_p     pointer to camera values to check if mask is non-zero
 *
 * returns:
 *      0 ==> masked values of union are OK, <0 ==> error in masked values
 */
int
pwc_check(union lavacam *u_cam_p)
{
    struct pwc_state *cam;	/* u_cam_p as pwc union element */

    /*
     * firewall
     */
    if (u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * pick out the driver specific union element
     */
    cam = &(u_cam_p->pwc);

    /*
     * quick exit if mask is empty
     */
    if (cam->mask == 0) {
	return LAVACAM_ERR_OK;
    }

    /*
     * check values if the mask allows
     */
    if (PWC_TEST(cam->mask, bright)) {
	if (cam->bright < PWC_MIN_PICT || cam->bright > PWC_MAX_PICT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, hue)) {
	if (cam->hue < PWC_MIN_PICT || cam->hue > PWC_MAX_PICT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, colour)) {
	if (cam->colour < PWC_MIN_PICT || cam->colour > PWC_MAX_PICT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, contrast)) {
	if (cam->contrast < PWC_MIN_PICT || cam->contrast > PWC_MAX_PICT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, gamma)) {
	if (cam->gamma < PWC_MIN_PICT || cam->gamma > PWC_MAX_PICT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, height)) {
	if (cam->subtype > 0 && cam->minheight > 0 && cam->maxheight > 0) {
	    if (cam->height < cam->minheight || cam->height > cam->maxheight) {
		DBG(LAVACAM_ERR_RANGE);
		return LAVACAM_ERR_RANGE;
	    }
	} else {
	    if (cam->height < PWC_MIN_SIZE || cam->height > PWC_MAX_PICT) {
		DBG(LAVACAM_ERR_RANGE);
		return LAVACAM_ERR_RANGE;
	    }
	}
    }
    if (PWC_TEST(cam->mask, width)) {
	if (cam->subtype > 0 && cam->minwidth > 0 && cam->maxwidth > 0) {
	    if (cam->width < cam->minwidth || cam->width > cam->maxwidth) {
		DBG(LAVACAM_ERR_RANGE);
		return LAVACAM_ERR_RANGE;
	    }
	} else {
	    if (cam->width < PWC_MIN_SIZE || cam->width > PWC_MAX_PICT) {
		DBG(LAVACAM_ERR_RANGE);
		return LAVACAM_ERR_RANGE;
	    }
	}
    }
    if (PWC_TEST(cam->mask, fps)) {
	if (cam->fps < PWC_MIN_FPS || cam->fps > PWC_MAX_PICT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, compress)) {
	if (cam->compress < PWC_MIN_COMPRESSION ||
	    cam->compress > PWC_MAX_COMPRESSION) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, agc)) {
	if (cam->agc < -PWC_MAX_AGC || cam->agc > PWC_MAX_AGC) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, speed)) {
	if (cam->speed < PWC_MIN_AGC || cam->speed > PWC_MAX_AGC) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, white_mode)) {
	if (cam->white_mode < PWC_MIN_WH_BAL_MODE ||
	    cam->white_mode > PWC_MAX_WH_BAL_MODE) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, white_red)) {
	if (cam->white_red < PWC_MIN_WH_BAL_LEVEL ||
	    cam->white_red > PWC_MAX_WH_BAL_LEVEL) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, white_blue)) {
	if (cam->white_blue < PWC_MIN_WH_BAL_LEVEL ||
	    cam->white_blue > PWC_MAX_WH_BAL_LEVEL) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, on)) {
	if (cam->on < PWC_MIN_LED_TIME || cam->on > PWC_MAX_LED_TIME) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, off)) {
	if (cam->off < PWC_MIN_LED_TIME || cam->off > PWC_MAX_LED_TIME) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, sharpness)) {
	if (cam->sharpness < PWC_MIN_SHARPNESS ||
	    cam->sharpness > PWC_MAX_SHARPNESS) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, backlight)) {
	/* any backlight value is valid, but we reserve PWC_UNSET_BACKLIGHT */
	if (cam->backlight == PWC_UNSET_BACKLIGHT) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, flicker)) {
	/* any flicker value is valid, but we reserve PWC_UNSET_FLICKER */
	if (cam->flicker == PWC_UNSET_FLICKER) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }
    if (PWC_TEST(cam->mask, dyn_noise)) {
	if (cam->dyn_noise < PWC_MIN_DYNNOISE ||
	    cam->dyn_noise > PWC_MAX_DYNNOISE) {
	    DBG(LAVACAM_ERR_RANGE);
	    return LAVACAM_ERR_RANGE;
	}
    }

    /*
     * all is OK
     */
    return LAVACAM_ERR_OK;
}


/*
 * pwc_print - print state of an open camera
 *
 * given:
 *      stream      where to print camera info
 *      cam_fd      open camera file descriptor
 *      u_cam_p     pointer to camera state
 *      siz         pointer to operation size and buffer structure
 *
 * returns:
 *      0 ==> unable to obtain camera information
 *      1 ==> camera information obtained and printed
 */
int
pwc_print(FILE * stream, int cam_fd, union lavacam *u_cam_p,
	  struct opsize *siz)
{
    struct pwc_state *cam;		/* u_cam_p as pwc union element */
    struct v4l2_capability vcap;	/* video capability */
    char camname[sizeof(vcap.card)+1];	/* camera name and NUL byte */

    /*
     * firewall
     */
    if (stream == NULL || cam_fd < 0 || u_cam_p == NULL || siz == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * pick out the driver specific union element
     */
    cam = &(u_cam_p->pwc);

    /*
     * obtain camera name
     */
    memset(&vcap, 0, sizeof(vcap));
    if (pwc_xioctl(cam_fd, VIDIOC_QUERYCAP, &vcap) < 0) {
	fprintf(stream, "pwc_print: unable to get video capability\n");
	DBG(LAVACAM_ERR_GETPARAM);
	return LAVACAM_ERR_GETPARAM;
    }
    memcpy(camname, vcap.card, sizeof(vcap.card));
    camname[sizeof(vcap.card)] = '\0';

    /*
     * print the camera name
     */
    if (vcap.card[0] == '\0') {
	fprintf(stream, "\tCamera name: <<__unknown__>>\n");
    } else {
	fprintf(stream, "\tCamera name: %s\n", camname);
    }

    /*
     * print Philips camera type
     */
    fprintf(stream, "\tDriver: %.*s\n", (int)sizeof(vcap.driver),
	    (char *)vcap.driver);
    if (cam->subtype > 0) {
	fprintf(stream, "\tPhilips camera type: %d\n", cam->subtype);
    } else {
	fprintf(stream, "\tPhilips camera type: not reported by driver\n");
    }

    /*
     * print picture information
     */
    if (cam->bright != PWC_UNSET_PICT) {
	fprintf(stream, "\tPicture brightness: %d\n", cam->bright);
    }
    if (cam->hue != PWC_UNSET_PICT) {
	fprintf(stream, "\tPicture hue: %d\n", cam->hue);
    }
    if (cam->colour != PWC_UNSET_PICT) {
	fprintf(stream, "\tPicture colour: %d\n", cam->colour);
    }
    if (cam->contrast != PWC_UNSET_PICT) {
	fprintf(stream, "\tPicture contrast: %d\n", cam->contrast);
    }
    if (cam->gamma != PWC_UNSET_PICT) {
	fprintf(stream, "\tPicture gamma: %d\n", cam->gamma);
    }
    if (cam->depth != PWC_UNSET_PICT) {
	fprintf(stream, "\tPicture depth: %d\n", cam->depth);
    }

    /*
     * print min/max width/height
     */
    if (cam->minwidth >= PWC_MIN_SIZE) {
	fprintf(stream, "\tMin width: %d\n", cam->minwidth);
    } else {
	fprintf(stream, "\tUnknown min width\n");
    }
    if (cam->minheight >= PWC_MIN_SIZE) {
	fprintf(stream, "\tMin height: %d\n", cam->minheight);
    } else {
	fprintf(stream, "\tUnknown min height\n");
    }
    if (cam->maxwidth >= PWC_MIN_SIZE) {
	fprintf(stream, "\tMax width: %d\n", cam->maxwidth);
    } else {
	fprintf(stream, "\tUnknown max width\n");
    }
    if (cam->maxheight >= PWC_MIN_SIZE) {
	fprintf(stream, "\tMax height: %d\n", cam->maxheight);
    } else {
	fprintf(stream, "\tUnknown max height\n");
    }

    /*
     * print the video window information
     */
    if (cam->width >= PWC_MIN_SIZE) {
	fprintf(stream, "\tWidth: %d\n", cam->width);
    } else {
	fprintf(stream, "\tUnknown width\n");
    }
    if (cam->height >= PWC_MIN_SIZE) {
	fprintf(stream, "\tHeight: %d\n", cam->height);
    } else {
	fprintf(stream, "\tUnknown height\n");
    }
    if (cam->fps >= PWC_MIN_FPS) {
	fprintf(stream, "\tFramerate (fps): %d\n", cam->fps);
    } else {
	fprintf(stream, "\tUnknown frame rate\n");
    }

    /*
     * print the compression preference
     */
    if (cam->compress >= PWC_MIN_COMPRESSION &&
	cam->compress <= PWC_MAX_COMPRESSION) {
	fprintf(stream, "\tCompression preference: %d\n", cam->compress);
    } else {
	fprintf(stream, "\tCompression: chosen by the driver\n");
    }

    /*
     * print the while balance
     */
    switch (cam->white_mode) {
    case PWC_WB_AUTO:
	fprintf(stream, "\tWhite balance (%d): automatic\n", cam->white_mode);
	fprintf(stream, "\tRed white balance: %d\n", cam->read_red);
	fprintf(stream, "\tBlue white balance: %d\n", cam->read_blue);
	break;
    case PWC_WB_MANUAL:
	fprintf(stream, "\tWhite balance (%d): manual\n", cam->white_mode);
	fprintf(stream, "\tRed manual white balance: %d\n", cam->white_red);
	fprintf(stream, "\tBlue manual white balance: %d\n", cam->white_blue);
	break;
    case PWC_WB_INDOOR:
	fprintf(stream, "\tWhite balance (%d): indoor\n", cam->white_mode);
	break;
    case PWC_WB_OUTDOOR:
	fprintf(stream, "\tWhite balance (%d): outdoor\n", cam->white_mode);
	break;
    case PWC_WB_FL:
	fprintf(stream, "\tWhite balance (%d): fluorescent\n",
		cam->white_mode);
	break;
    default:
	fprintf(stream, "\tUnknown white balance: %d\n", cam->white_mode);
	break;
    }

    /*
     * print the LED on/off time
     */
    if (cam->on == PWC_UNSET_LED_TIME &&
	cam->off == PWC_UNSET_LED_TIME) {
	fprintf(stream, "\tcamera does not support LED control\n");
    } else {
	if (cam->on >= 0) {
	    fprintf(stream, "\tLED on time: %d\n", cam->on);
	} else {
	    fprintf(stream, "\tUnknown ON time\n");
	}
	if (cam->off >= 0) {
	    fprintf(stream, "\tLED off time: %d\n", cam->off);
	} else {
	    fprintf(stream, "\tUnknown OFF time\n");
	}
    }

    /*
     * print electronic sharpness value
     */
    if (cam->sharpness == PWC_AUTO_SHARPNESS) {
	fprintf(stream, "\tElectronic sharpness (%d): automatic\n",
		cam->sharpness);
    } else if (cam->sharpness >= PWC_MIN_SHARPNESS &&
	       cam->sharpness <= PWC_MAX_SHARPNESS) {
	fprintf(stream, "\tElectronic sharpness: %d\n", cam->sharpness);
    } else {
	fprintf(stream, "\tDriver does not support electronic sharpness\n");
    }

    /*
     * print backlight compression mode
     *
     * While PWC_UNSET_BACKLIGHT is a valid value, it is not likely to be
     * returned so we treat it as if it were invalid.
     */
    if (cam->backlight == PWC_UNSET_BACKLIGHT) {
	fprintf(stream, "\tDriver does not support backlight compression\n");
    } else if (cam->backlight == 0) {
	fprintf(stream, "\tBacklight compression (%d): OFF\n", cam->backlight);
    } else {
	fprintf(stream, "\tBacklight compression (%d): ON\n", cam->backlight);
    }

    /*
     * print flicker suppression mode
     *
     * While PWC_UNSET_FLICKER is a valid value, it is not likely to be
     * returned so we treat it as if it were invalid.
     */
    if (cam->flicker == PWC_UNSET_FLICKER) {
	fprintf(stream, "\tDriver does not support flicker suppression\n");
    } else if (cam->flicker == 0) {
	fprintf(stream, "\tFlicker suppression (%d): OFF\n", cam->flicker);
    } else {
	fprintf(stream, "\tFlicker suppression (%d): ON\n", cam->flicker);
    }

    /*
     * print dynamic noise reduction value
     */
    if (cam->dyn_noise == PWC_MIN_DYNNOISE) {
	fprintf(stream, "\tDynamic noise reduction (%d): OFF\n",
		cam->dyn_noise);
    } else if (cam->dyn_noise > PWC_MIN_DYNNOISE &&
	       cam->dyn_noise <= PWC_MAX_DYNNOISE) {
	fprintf(stream, "\tDynamic noise reduction: %d\n", cam->dyn_noise);
    } else {
	fprintf(stream, "\tDriver does not support dynamic noise reduction\n");
    }

    /*
     * print automatic gain control
     */
    if (cam->agc >= 0 && cam->agc <= PWC_MAX_AGC) {
	fprintf(stream, "\tManual gain control (AGC): %d\n", cam->agc);
    } else if (cam->agc >= -PWC_MAX_AGC && cam->agc < 0) {
	fprintf(stream, "\tAutomatic gain control (AGC): %d\n", cam->agc);
    } else {
	fprintf(stream, "\tUnknown gain control\n");
    }

    /*
     * print shutter speed (exposure)
     */
    if (cam->speed >= 0 && cam->speed <= PWC_MAX_SPEED) {
	fprintf(stream, "\tManual shutter speed: %d\n", cam->speed);
    } else if (cam->speed == PWC_AUTO_SPEED) {
	fprintf(stream, "\tAutomatic shutter speed\n");
    } else {
	fprintf(stream, "\tUnknown shutter speed\n");
    }

    /*
     * print video buffer information
     */
    if (siz->nbufs > 0) {
	fprintf(stream, "\tV4L2 mmap buffers: %d\n", siz->nbufs);
    } else {
	fprintf(stream, "\tNo V4L2 mmap buffers in use\n");
    }

    /*
     * print palette information
     */
    fprintf(stream, "\t%s (%d)\n",
    		    palette_name(PALLETTE_VIDEO4LINUX, cam->palette),
		    cam->palette);

    /*
     * output working sanity check parameters
     */
    fprintf(stream, "\ncamera sanity check parameters:\n");
    fprintf(stream, "\t%d most frequent octet values are considered common\n",
    		    siz->top_x);
    fprintf(stream, "\tmax fraction of frame containing common octet "
    		    "values: %.6f\n",
    		    1.0 - siz->min_fract);
    fprintf(stream, "\t%d most common values must be < 1/2 of frame octets\n",
    		    siz->half_x);
    fprintf(stream, "\tmin fraction of bits same in next frame: %.6f\n",
    		    siz->diff_fract);
    fprintf(stream, "\tmax fraction of bits same in next frame: %.6f\n",
    		    1.0 - siz->diff_fract);

    /*
     * output use fields
     */
    fprintf(stream, "\ncamera use since this process opened the camera:\n");
    /* ctime returns a newline, so the next printf should not end in one */
    fprintf(stream, "\tcamera open time: %s", ctime(&siz->open_time));
    fprintf(stream, "\tframe count: %lld\n", siz->frame_num);
    fprintf(stream, "\tinsane frame count: %lld\n", siz->insane_cnt);

    /*
     * output opsizes
     */
    fprintf(stream, "\ncamera op sizes:\n");
    if (siz->readsize > 0) {
	fprintf(stream, "\tread size: %d\n", siz->readsize);
	fprintf(stream, "\tread LavaRnd offset: %d\n", siz->read_lavaoff);
	fprintf(stream, "\tread LavaRnd length: %d\n", siz->read_lavalen);
    } else {
	fprintf(stream, "\treading not allowed\n");
    }
    if (siz->mmapsize > 0) {
	fprintf(stream, "\n\tmmap size: %d\n", siz->mmapsize);
	fprintf(stream, "\tmmap frame size: %d\n", siz->framesize);
	fprintf(stream, "\tmmap frames: %d\n", siz->frames);
	fprintf(stream, "\tmmap LavaRnd offset: %d\n", siz->mmap_lavaoff);
	fprintf(stream, "\tmmap LavaRnd length: %d\n", siz->mmap_lavalen);
    } else {
	fprintf(stream, "\n\tmmaping not allowed\n");
    }
    if (siz->use_read) {
	fprintf(stream, "\n\twill use read I/O\n");
	fprintf(stream, "\tread image: %d\n", siz->image_len);
    } else {
	fprintf(stream, "\n\twill use mmap I/O\n");
	fprintf(stream, "\tmmap image: %d\n", siz->image_len);
    }
    fprintf(stream, "\tchaos size: %d\n", siz->chaos_len);
    if (siz->image != NULL && siz->chaos != NULL) {
	fprintf(stream, "\tchaos offset: %d\n",
		(int)(siz->chaos - siz->image));
    } else {
	fprintf(stream, "\tchaos offset: UNKNOWN\n");
    }

    /*
     * all done
     */
    return 1;
}


/*
 * pwc_usage - print an argc/argv usage message
 *
 * given:
 *      prog            program name
 *      typename        name of camera type
 */
void
pwc_usage(char *prog, char *typename)
{
    /*
     * firewall
     */
    if (prog == NULL) {
	prog = "<<__NULL__>>";
    }
    if (typename == NULL) {
	typename = "<<__NULL__>>";
    }

    /*
     * print usage message to stderr
     */
    fprintf(stderr,
	    "usage: %s %s devname [-flags ... args ...]\n"
	    "\n"
	    "\t-b bright\tbrightness level [0..%d]\n"
	    "\t-h hue\t\thue level [0..%d] (ignored by V4L2 pwc)\n"
	    "\t-c colour\tcolour level [0..%d]\n"
	    "\t-n contrast\tcontrast level [0..%d]\n"
	    "\t-g gamma\tgamma level [0..%d]\n"
	    "\t-H height\theight in pixels (>=%d)\n"
	    "\t-W width\twidth in pixels (>=%d)\n"
	    "\t-f fps\t\tframes per second (>=%d)\n"
	    "\t-C comp\t\tcompression type [%d..%d] (ignored by V4L2 pwc)\n"
	    "\t-a AGC\t\t(Automatic) gain control [%d..%d], %d==auto\n"
	    "\t-s speed\tshutter (higher==slower) speed [%d..%d], %d==auto\n"
	    "\t-m wh_bal\twhite balance mode [%d..%d], %d==manual, %d==auto\n"
	    "\t-R red\t\tred white balance level [%d..%d]\n"
	    "\t-B blue\t\tblue white balance level [%d..%d]\n"
	    "\t-o on_msec\tLED on time in msec [%d..%d] (ignored by V4L2 pwc)\n"
	    "\t-O off_msec\tLED off time in msec [%d..%d] (ignored by V4L2 pwc)\n"
	    "\t-S sharp\telectronic sharpness [%d..%d], %d==auto\n"
	    "\t-l backlight\tbacklight compression (0 or non-0)\n"
	    "\t-F flicker\timage flicker suppression (0 or non-0)\n"
	    "\t-N noise\tdynamic noise reduction [%d..%d]\n"
	    "\t-P arg\t\tUNUSED\n"
	    "\t-v level\tverbose mode, print settings before/after\n"
	    "\t-T tsec\t\twarm-up/sleep for tsec seconds\n"
	    "\t-D tsec\t\tframe delay tsec seconds between frames\n"
	    "\t-V vid_chan\tvideo channel (>= 0)\n"
	    "\t-M\t\tdisable mmap processing, use reads instead\n"
	    "\t-L\t\tset LavaRnd entropy optimal mode\n"
	    "\t-A\t\tprocess all of the image, not just the high entropy part\n"
	    "\t-E\t\tavoid frame dumping when savefile exists\n"
	    "\t-X top_x\ttop_x common octets are common octets [0, %d]\n"
	    "\t-x min_fract\tmin fraction uncommon octets in frame [0.0, 1.0]\n"
	    "\t-2 half_x\thalf_x common octets occupy <50%% octets [0, %d]\n"
	    "\t-d min_fract\tmin fraction of diff bits between frames [0.0, 0.5]\n",
	    prog, typename,
	    PWC_MAX_PICT,
	    PWC_MAX_PICT,
	    PWC_MAX_PICT,
	    PWC_MAX_PICT,
	    PWC_MAX_PICT,
	    PWC_MIN_SIZE,
	    PWC_MIN_SIZE,
	    PWC_MIN_FPS,
	    PWC_MIN_COMPRESSION, PWC_MAX_COMPRESSION,
	    PWC_MIN_AGC, PWC_MAX_AGC, PWC_AUTO_AGC,
	    PWC_MIN_SPEED, PWC_MAX_SPEED, PWC_AUTO_SPEED,
	    PWC_MIN_WH_BAL_MODE, PWC_MAX_WH_BAL_MODE,
	    PWC_MANUAL_WH_BAL, PWC_WB_AUTO,
	    PWC_MIN_WH_BAL_LEVEL, PWC_MAX_WH_BAL_LEVEL,
	    PWC_MIN_WH_BAL_LEVEL, PWC_MAX_WH_BAL_LEVEL,
	    PWC_MIN_LED_TIME, PWC_MAX_LED_TIME,
	    PWC_MIN_LED_TIME, PWC_MAX_LED_TIME,
	    PWC_MIN_SHARPNESS, PWC_MAX_SHARPNESS, PWC_AUTO_SHARPNESS,
	PWC_MIN_DYNNOISE, PWC_MAX_DYNNOISE,
	OCTET_CNT, OCTET_CNT/2);
    return;
}


/*
 * pwc_argv - argc/argv parse, sets a camera's state change and global flags
 *
 * given:
 *      argc        command line argc count
 *      argv        point to array of command line argument strings
 *      u_cam_p     pointer to values to be set if mask is non-zero
 *      flag        flags to set
 *      model       camera model number
 *
 * returns:
 *      amount of args to skip, <0 ==> error
 *
 * NOTE: This function will initialize the tmp sanity check values to
 *       their defaults according to the lava_state[] default values.
 *       If -L is was given, the tmp sanity check values will be
 *       reloaded according to the lava_state[] default values.
 *       If -x, -X, -d was given, the tmp sanity check values will be
 *       modified according to the flag.  Sometime later during the
 *       pwc_open(), these tmp sanity check values will be loaded
 *       into the working sanity check values in the struct opsize.
 */
int
pwc_argv(int argc, char **argv, union lavacam *u_cam_p,
	 struct lavacam_flag *flag, int model)
{
    int tmp;			/* temporary holder of a parsed argument */
    double dtmp;		/* temporary holder of double/float arg */
    struct pwc_state *cam;	/* u_cam_p as pwc union element */
    char *optarg;		/* option argument */
    int c;			/* option */
    int model_indx;		/* default state model index */
    int i;

    /*
     * firewall
     */
    if (argc <= 0 || argv == NULL || argv[0] == NULL || u_cam_p == NULL ||
	flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    flag->program = argv[0];

    /*
     * pick out the driver specific union element
     */
    cam = &(u_cam_p->pwc);

    /*
     * look for default state index
     */
    model_indx = pwc_model(model);
    if (model_indx < 0) {
	return model_indx;
    }

    /*
     * initialize
     */
    memset(cam, 0, sizeof(cam[0]));
    flag->D_flag = 0.0;
    flag->T_flag = -1.0;
    flag->A_flag = 0;
    flag->M_flag = 1;
    flag->v_flag = 0;
    flag->E_flag = 0;
    flag->savefile = NULL;
    flag->newfile = NULL;
    flag->interval = 0.0;
    /* set the default tmp sanity check values */
    cam->tmp_top_x = lava_state[model_indx].def_top_x;
    cam->tmp_min_fract = lava_state[model_indx].def_min_fract;
    cam->tmp_half_x = lava_state[model_indx].def_half_x;
    cam->tmp_diff_fract = lava_state[model_indx].def_diff_fract;

    /*
     * parse args
     *
     * We would like to use getopt(), but many implementations do not
     * allow one to call getopt() with a different set of args.  Thus
     * if one uses getopt() to parse the command line arguments, one
     * cannot later on use getopt() to parse the driver args.
     *
     * We are forced to roll our own simple arg parser.  *sigh*
     */
    for (i = 1; i < argc; ++i) {

	/*
	 * must be a -<char>
	 */
	if (argv[i] == NULL) {
	    fprintf(stderr, "%s: pwc_argv[%d] is NULL\n", argv[0], i);
	    DBG(LAVACAM_ERR_ARG);
	    return LAVACAM_ERR_ARG;
	}
	if (argv[i][0] != '-') {
	    /* end of -options, stop parsing args */
	    break;
	}

	/*
	 * process the -option
	 */
	c = (int)argv[i][1];
	switch (c) {
	case 'b':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_PICT || tmp > PWC_MAX_PICT) {
		fprintf(stderr, "%s: brightness must be in range [%d..%d]\n",
			flag->program, PWC_MIN_PICT, PWC_MAX_PICT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->bright = tmp;
	    PWC_SET(cam->mask, bright);
	    break;
	case 'h':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_PICT || tmp > PWC_MAX_PICT) {
		fprintf(stderr, "%s: hue must be in range [%d..%d]\n",
			flag->program, PWC_MIN_PICT, PWC_MAX_PICT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->hue = tmp;
	    PWC_SET(cam->mask, hue);
	    break;
	case 'c':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_PICT || tmp > PWC_MAX_PICT) {
		fprintf(stderr, "%s: colour must be in range [%d..%d]\n",
			flag->program, PWC_MIN_PICT, PWC_MAX_PICT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->colour = tmp;
	    PWC_SET(cam->mask, colour);
	    break;
	case 'n':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_PICT || tmp > PWC_MAX_PICT) {
		fprintf(stderr, "%s: contrast must be in range [%d..%d]\n",
			flag->program, PWC_MIN_PICT, PWC_MAX_PICT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->contrast = tmp;
	    PWC_SET(cam->mask, contrast);
	    break;
	case 'g':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_PICT || tmp > PWC_MAX_PICT) {
		fprintf(stderr, "%s: gamma must be in range [%d..%d]\n",
			flag->program, PWC_MIN_PICT, PWC_MAX_PICT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->gamma = tmp;
	    PWC_SET(cam->mask, gamma);
	    break;
	case 'H':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_SIZE) {
		fprintf(stderr, "%s: height must be >= %d\n",
			flag->program, PWC_MIN_SIZE);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->height = tmp;
	    PWC_SET(cam->mask, height);
	    break;
	case 'W':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_SIZE) {
		fprintf(stderr, "%s: width must be >= %d\n",
			flag->program, PWC_MIN_SIZE);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->width = tmp;
	    PWC_SET(cam->mask, width);
	    break;
	case 'f':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_FPS) {
		fprintf(stderr, "%s: frames per second must be >= %d\n",
			flag->program, PWC_MIN_FPS);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->fps = tmp;
	    PWC_SET(cam->mask, fps);
	    break;
	case 'C':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_COMPRESSION || tmp > PWC_MAX_COMPRESSION) {
		fprintf(stderr, "%s: compression must be in the range "
			"[%d..%d]\n",
			flag->program,
			PWC_MIN_COMPRESSION, PWC_MAX_COMPRESSION);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->compress = tmp;
	    PWC_SET(cam->mask, compress);
	    break;
	case 'a':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_AGC || tmp > PWC_MAX_AGC) {
		fprintf(stderr, "%s: AGC must be in the range "
			"[%d..%d]\n", flag->program, PWC_MIN_AGC, PWC_MAX_AGC);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->agc = tmp;
	    PWC_SET(cam->mask, agc);
	    break;
	case 's':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_SPEED || tmp > PWC_MAX_SPEED) {
		fprintf(stderr, "%s: shutter speed must be in the range "
			"[%d..%d]\n",
			flag->program, PWC_MIN_SPEED, PWC_MAX_SPEED);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->speed = tmp;
	    PWC_SET(cam->mask, speed);
	    break;
	case 'm':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_WH_BAL_MODE || tmp > PWC_MAX_WH_BAL_MODE) {
		fprintf(stderr, "%s: white balance mode must be in the range "
			"[%d..%d]\n",
			flag->program,
			PWC_MIN_WH_BAL_MODE, PWC_MAX_WH_BAL_MODE);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->white_mode = tmp;
	    PWC_SET(cam->mask, white_mode);
	    break;
	case 'R':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_WH_BAL_LEVEL || tmp > PWC_MAX_WH_BAL_LEVEL) {
		fprintf(stderr, "%s: red white balance level must be in the "
			"range [%d..%d]\n",
			flag->program,
			PWC_MIN_WH_BAL_LEVEL, PWC_MAX_WH_BAL_LEVEL);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->white_red = tmp;
	    PWC_SET(cam->mask, white_red);
	    break;
	case 'B':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_WH_BAL_LEVEL || tmp > PWC_MAX_WH_BAL_LEVEL) {
		fprintf(stderr, "%s: blue white balance mode must be in the "
			"range [%d..%d]\n",
			flag->program,
			PWC_MIN_WH_BAL_LEVEL, PWC_MAX_WH_BAL_LEVEL);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->white_blue = tmp;
	    PWC_SET(cam->mask, white_blue);
	    break;
	case 'o':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_LED_TIME || tmp > PWC_MAX_LED_TIME) {
		fprintf(stderr, "%s: LED on msec must be in the range "
			"[%d..%d]\n",
			flag->program, PWC_MIN_LED_TIME, PWC_MAX_LED_TIME);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->on = tmp;
	    PWC_SET(cam->mask, on);
	    break;
	case 'O':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_LED_TIME || tmp > PWC_MAX_LED_TIME) {
		fprintf(stderr, "%s: LED off msec must be in the range "
			"[%d..%d]\n",
			flag->program, PWC_MIN_LED_TIME, PWC_MAX_LED_TIME);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->off = tmp;
	    PWC_SET(cam->mask, off);
	    break;
	case 'S':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_SHARPNESS || tmp > PWC_MAX_SHARPNESS) {
		fprintf(stderr,
			"%s: electronic sharpness must be in the range "
			"[%d..%d]\n", flag->program, PWC_MIN_SHARPNESS,
			PWC_MAX_SHARPNESS);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->sharpness = tmp;
	    PWC_SET(cam->mask, sharpness);
	    break;
	case 'l':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp == PWC_UNSET_BACKLIGHT) {
		fprintf(stderr, "%s: please do not use %d for "
			"backlight compression\n",
			flag->program, PWC_UNSET_BACKLIGHT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->backlight = tmp;
	    PWC_SET(cam->mask, backlight);
	    break;
	case 'F':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp == PWC_UNSET_BACKLIGHT) {
		fprintf(stderr, "%s: please do not use %d for "
			"image flicker suppression\n",
			flag->program, PWC_UNSET_FLICKER);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->flicker = tmp;
	    PWC_SET(cam->mask, flicker);
	    break;
	case 'N':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < PWC_MIN_DYNNOISE || tmp > PWC_MAX_DYNNOISE) {
		fprintf(stderr, "%s: dynamic noise reduction must be "
			"in the range [%d..%d]\n",
			flag->program, PWC_MIN_DYNNOISE, PWC_MAX_DYNNOISE);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->dyn_noise = tmp;
	    PWC_SET(cam->mask, dyn_noise);
	    break;
	case 'P':
	    /* type of image palette is not used - silently ignore */
	    if (i >= argc - 1) {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    break;
	case 'v':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < 0) {
		fprintf(stderr, "%s: verbose level must be >= 0\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    flag->v_flag = tmp;
	    break;
	case 'E':
	    flag->E_flag = 1;
	    break;
	case 'T':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    dtmp = (double)atof(optarg);
	    if (dtmp < 0.0) {
		fprintf(stderr, "%s: warm-up time must be >= 0.0\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    flag->T_flag = dtmp;
	    break;
	case 'D':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    dtmp = (double)atof(optarg);
	    if (dtmp < 0.0) {
		fprintf(stderr, "%s: delay time must be >= 0.0\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    flag->D_flag = dtmp;
	    break;
	case 'V':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < 0) {
		fprintf(stderr, "%s: video channel must be >= 0\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->channel = tmp;
	    break;
	case 'M':
	    flag->M_flag = 0;
	    break;
	case 'L':
	    tmp = pwc_LavaRnd(u_cam_p, model);
	    if (tmp < 0) {
		fprintf(stderr, "%s: LavaRnd mode set failed\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    break;
	case 'A':
	    flag->A_flag = 1;
	    break;
	case 'X':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < 0 || tmp > OCTET_CNT) {
		fprintf(stderr, "%s: top_x must be >= 0 and <= %d\n",
			flag->program, OCTET_CNT);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_top_x = tmp;
	    break;
	case 'x':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    dtmp = (double)atof(optarg);
	    if (dtmp < 0.0 || dtmp > 1.0) {
		fprintf(stderr, "%s: min_fract must be >= 0.0 and <= 1.0\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_min_fract = dtmp;
	    break;
	case '2':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    tmp = (int)strtol(optarg, NULL, 0);
	    if (tmp < 0 || tmp > OCTET_CNT / 2) {
		fprintf(stderr, "%s: half_x must be >= 0 and <= %d\n",
			flag->program, OCTET_CNT / 2);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_half_x = tmp;
	    break;
	case 'd':
	    if (i < argc - 1) {
		optarg = argv[++i];
	    } else {
		fprintf(stderr,
			"%s: pwc_argv[%d] -%c missing next argument\n",
			argv[0], i, c);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    dtmp = (double)atof(optarg);
	    if (dtmp < 0.0 || dtmp > 0.5) {
		fprintf(stderr, "%s: diff_fract must be >= 0.0 and <= 0.5\n",
			flag->program);
		DBG(LAVACAM_ERR_ARG);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_diff_fract = dtmp;
	    break;
	default:
	    fprintf(stderr,
		    "%s: pwc_argv[%d] -%c is unknown\n", argv[0], i, c);
	    DBG(LAVACAM_ERR_ARG);
	    return LAVACAM_ERR_ARG;
	}
    }

    /*
     * parse an optimal savefile/interval argument pair
     */
    if (argv[i] != NULL && argv[i + 1] != NULL) {

	/* save the savefile argument */
	flag->savefile = strdup(argv[i]);
	if (flag->savefile == NULL) {
	    DBG(LAVAERR_MALLOC);
	    return LAVAERR_MALLOC;
	}

	/* form the savefile.new name */
	flag->newfile =
	  (char *)malloc(strlen(flag->savefile) + sizeof(".new"));
	if (flag->newfile == NULL) {
	    free(flag->savefile);
	    flag->savefile = NULL;
	    DBG(LAVAERR_MALLOC);
	    return LAVAERR_MALLOC;
	}
	snprintf(flag->newfile, strlen(flag->savefile) + sizeof(".new"),
		 "%s.new", flag->savefile);

	/* note interval */
	flag->interval = (double)atof(argv[i + 1]);
	if (flag->interval <= 0.0) {
	    fprintf(stderr, "%s: interval: %.3f must be >0.0\n",
		    flag->program, flag->interval);
	    free(flag->savefile);
	    flag->savefile = NULL;
	    free(flag->newfile);
	    flag->newfile = NULL;
	    DBG(LAVACAM_ERR_ARG);
	    return LAVACAM_ERR_ARG;
	}

	/* skip over these two args */
	i += 2;
    }

    /*
     * return arg adjustment value
     */
    return i - 1;
}


/*
 * pwc_release - free frame buffers and close the camera descriptor
 *
 * given:
 *      cam_fd   open camera descriptor
 *      siz      pointer to operation size and buffer structure
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 */
static int
pwc_release(int cam_fd, struct opsize *siz)
{
    /*
     * free read buffer if reading
     */
    if (siz->use_read) {
	if (siz->image != NULL) {
	    (void)free(siz->image);
	}

	/*
	 * or stop streaming and unmap buffers if mmapped
	 */
    } else {
	pwc_stream_stop(cam_fd, siz);
    }

    /*
     * free previous chaos frame
     */
    if (siz->prev_frame != NULL) {
	(void)free(siz->prev_frame);
	siz->prev_frame = NULL;
    }

    /*
     * clear read/mmap frame data
     */
    siz->image = NULL;
    siz->image_len = 0;
    siz->chaos = NULL;
    siz->chaos_len = 0;

    /*
     * close device
     */
    if (close(cam_fd) < 0) {
	DBG(LAVACAM_ERR_CLOSE);
	return LAVACAM_ERR_CLOSE;
    }
    return LAVACAM_ERR_OK;
}


/*
 * pwc_open - open a camera, verify its type, return opening camera state
 *
 * given:
 *      devname     camera device file
 *      model       camera model number
 *      o_cam_p     where to place the open camera state
 *      n_cam_p     is non-NULL, set this new state and channel number
 *                      updated with the final open state if non-NULL
 *      siz         pointer to operation size and buffer structure to fill in
 *      def         0 ==> restore saved user settings, 1 ==> keep cur setting
 *      flag        flags set via lavacam_argv()
 *
 * returns:
 *      >=0 ==> camera file descriptor, <0 ==> error
 *
 * NOTE: The video channel number will be placed into cam->channel.
 *       The siz and buffer structure will be setup on successful return.
 *       The n_cam_p state, if it was non-NULL, will be updated as well.
 *
 * NOTE: The function will also transfer tmp sanity check parameters from
 *       the union lavacam to the opsize structure via the pwc_opsize() call.
 *
 * NOTE: When mmapping, V4L2 streaming is started before this returns.
 */
int
pwc_open(char *devname, int model, union lavacam *o_cam_p,
	 union lavacam *n_cam_p, struct opsize *siz, int def,
	 struct lavacam_flag *flag)
{
    struct pwc_state *cam;	/* o_cam_p as pwc union element */
    union lavacam t_cam;	/* temp new camera state */
    struct pwc_state *n_cam;	/* n_cam_p as pwc union element */
    struct v4l2_format fmt;	/* capture format */
    int input;			/* video input (channel) number */
    double open_warmup;		/* if >0.0, seconds to warmup camera */
    int model_indx;		/* default state model index */
    int cam_fd;			/* open device file descriptor */
    int ret;			/* return from fetch of camera state */
    int i;

    /*
     * firewall
     */
    if (devname == NULL || o_cam_p == NULL || siz == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * pick out the driver specific union element
     */
    cam = &(o_cam_p->pwc);
    if (n_cam_p == NULL) {
	n_cam = NULL;
    } else {
	n_cam = &(n_cam_p->pwc);
    }

    /*
     * look for default state index
     */
    model_indx = pwc_model(model);
    if (model_indx < 0) {
	return model_indx;
    }

    /*
     * attempt to open the device
     */
    cam_fd = open(devname, O_RDWR);
    if (cam_fd < 0) {
	/* unable to open camera */
	if (errno == EACCES) {
	    /* permission denied */
	    DBG(LAVAERR_PERMOPEN);
	    return LAVAERR_PERMOPEN;
	} else {
	    /* other open failure */
	    DBG(LAVACAM_ERR_OPEN);
	    return LAVACAM_ERR_OPEN;
	}
    }

    /*
     * set video input (channel)
     */
    input = (n_cam == NULL ? 0 : n_cam->channel);
    if (pwc_xioctl(cam_fd, VIDIOC_S_INPUT, &input) < 0) {
	(void)close(cam_fd);
	DBG(LAVACAM_ERR_CHAN);
	return LAVACAM_ERR_CHAN;
    }

    /*
     * restore saved user settings unless default
     */
    if (def == 0) {
	if (pwc_ctrl_set(cam_fd, PWC_CID_RESTORE_USER, 1) == PWC_CTRL_FAILED) {
	    (void)close(cam_fd);
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
    }

    /*
     * select planar YUV 4:2:0, the only format LavaRnd can use
     *
     * NOTE: The V1 driver did this after loading the new state.  With V4L2
     *	     the format must be chosen before streaming starts, and doing
     *	     it first lets the new state's size override the current one.
     */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (pwc_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	(void)close(cam_fd);
	DBG(LAVACAM_ERR_GETPARAM);
	return LAVACAM_ERR_GETPARAM;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = 0;
	fmt.fmt.pix.sizeimage = 0;
	if (pwc_xioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
	    (void)close(cam_fd);
	    DBG(LAVACAM_ERR_PALUNSET);
	    return LAVACAM_ERR_PALUNSET;
	}
    }

    /*
     * initialize cam state
     */
    memset(cam, 0, sizeof(cam[0]));

    /*
     * fetch the camera state
     */
    ret = pwc_get(cam_fd, o_cam_p);
    if (ret < 0) {
	/* perhaps device is not a Phillis camera */
	(void)close(cam_fd);
	return ret;
    }
    cam->channel = (n_cam == NULL ? 0 : n_cam->channel);
    /* set the default tmp sanity check values */
    if (n_cam == NULL) {
	cam->tmp_top_x = lava_state[model_indx].def_top_x;
	cam->tmp_min_fract = lava_state[model_indx].def_min_fract;
	cam->tmp_half_x = lava_state[model_indx].def_half_x;
	cam->tmp_diff_fract = lava_state[model_indx].def_diff_fract;
    } else {
	cam->tmp_top_x = n_cam->tmp_top_x;
	cam->tmp_min_fract = n_cam->tmp_min_fract;
	cam->tmp_half_x = n_cam->tmp_half_x;
	cam->tmp_diff_fract = n_cam->tmp_diff_fract;
    }

    /*
     * set the new camera state, if one was supplied, then re-fetch it
     */
    if (n_cam != NULL) {

	/* set the new state */
	ret = pwc_set(cam_fd, n_cam_p);
	if (ret < 0) {
	    /* failed to load new camera settings */
	    (void)close(cam_fd);
	    return ret;
	}
	/* re-fetch the camera state */
	ret = pwc_get(cam_fd, n_cam_p);
	if (ret < 0) {
	    /* perhaps device is not a Phillis camera */
	    (void)close(cam_fd);
	    return ret;
	}
	n_cam->channel = cam->channel;
	n_cam->tmp_top_x = cam->tmp_top_x;
	n_cam->tmp_min_fract = cam->tmp_min_fract;
	n_cam->tmp_half_x = cam->tmp_half_x;
	n_cam->tmp_diff_fract = cam->tmp_diff_fract;
	/* copy new camera state into our working state */
	memcpy((void *)&t_cam, (void *)n_cam_p, sizeof(t_cam));

	/*
	 * no new state, just work with the opening state
	 */
    } else {
	memcpy((void *)&t_cam, (void *)o_cam_p, sizeof(t_cam));
    }

    /*
     * determine camera recommended read/mmap size
     */
    i = pwc_opsize(cam_fd, &t_cam, siz, flag);
    if (i < 0) {
	/* opsize failed, return error */
	(void)close(cam_fd);
	return i;
    }

    /*
     * determine if we will read or mmap frames
     */
    if (siz->readsize <= 0 && flag->M_flag) {
	(void)close(cam_fd);
	return LAVACAM_ERR_NOREAD;
    } else if (siz->mmapsize <= 0 && !flag->M_flag) {
	(void)close(cam_fd);
	return LAVACAM_ERR_NOMMAP;
    }
    siz->use_read = !flag->M_flag;

    /*
     * setup to read or mmap
     */
    if (siz->use_read) {

	/*
	 * read setup
	 */
	siz->image_len = siz->readsize;
	if (siz->image_len <= 0) {
	    (void)close(cam_fd);
	    DBG(LAVACAM_ERR_NOREAD);
	    return LAVACAM_ERR_NOREAD;
	}
	siz->image = malloc(siz->image_len);
	if (siz->image == NULL) {
	    (void)close(cam_fd);
	    DBG(LAVACAM_ERR_NOREAD);
	    return LAVACAM_ERR_NOREAD;
	}
	siz->chaos = (u_int8_t *)siz->image + siz->read_lavaoff;
	siz->chaos_len = siz->read_lavalen;

    } else {

	/*
	 * mmap setup - each V4L2 buffer holds one frame
	 */
	siz->image_len = siz->framesize;
	siz->chaos_len = siz->mmap_lavalen;
	ret = pwc_stream_start(cam_fd, siz);
	if (ret < 0) {
	    (void)close(cam_fd);
	    return ret;
	}
    }

    /*
     * allocate and zero previous chaos frame buffer
     */
    siz->prev_frame = calloc(1, siz->chaos_len);
    if (siz->prev_frame == NULL) {
	pwc_release(cam_fd, siz);
	DBG(LAVAERR_MALLOC);
	return LAVAERR_MALLOC;
    }

    /*
     * determine the camera warmup time, if any
     */
    if (flag->T_flag > 0.0) {

	/* use the -T flag instead of model default warmup */
	open_warmup = flag->T_flag;

    } else {

	/* use model default warmup time due to lack of -T flag */
	open_warmup = lava_state[model_indx].warmup;
    }

    /*
     * warm up camera by tossing frames for a period of time if requested
     */
    if (open_warmup > 0.0) {
	double now;	/* current time as a double */
	double end;	/* end of warmup time */

	/*
	 * setup timing loop
	 */
	now = right_now();
	if (now < 0.0) {
	    pwc_release(cam_fd, siz);
	    DBG(LAVAERR_GETTIME);
	    return LAVAERR_GETTIME;
	}
	end = now + open_warmup;

	/*
	 * read until warm up time is over
	 */
	while (now < end) {

	    /*
	     * wait for the next frame
	     */
	    if (pwc_wait_frame(cam_fd, end - now) < 0) {
		pwc_release(cam_fd, siz);
		DBG(LAVACAM_ERR_WARMUP);
		return LAVACAM_ERR_WARMUP;
	    }

	    /*
	     * obtain the next frame and toss it
	     *
	     * NOTE: With V4L2 mmap the frame must be dequeued and requeued,
	     *	     or the driver runs out of buffers and the first frames
	     *	     returned after warm up would be stale.
	     */
	    if (pwc_get_frame(cam_fd, siz) < 0 ||
		pwc_msync(cam_fd, &t_cam, siz) < 0) {
		pwc_release(cam_fd, siz);
		DBG(LAVACAM_ERR_WARMUP);
		return LAVACAM_ERR_WARMUP;
	    }

	    /*
	     * determine the new now
	     */
	    now = right_now();
	    if (now < 0.0) {
		pwc_release(cam_fd, siz);
		DBG(LAVAERR_GETTIME);
		return LAVAERR_GETTIME;
	    }
	}
    }

    /*
     * return open file descriptor
     */
    return cam_fd;
}


/*
 * pwc_close - close an open camera
 *
 * given:
 *      cam_fd   open camera descriptor
 *      siz      pointer to operation size and buffer structure
 *      flag        flags set via lavacam_argv()
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 */
int
pwc_close(int cam_fd, struct opsize *siz, struct lavacam_flag *flag)
{
    /*
     * firewall
     */
    if (cam_fd < 0 || siz == NULL || flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * free savefile/newfile is they are present
     */
    if (flag->savefile != NULL) {
	free(flag->savefile);
	flag->savefile = NULL;
    }
    if (flag->newfile != NULL) {
	free(flag->newfile);
	flag->newfile = NULL;
    }
    flag->interval = 0.0;

    /*
     * free frame buffers and close device
     */
    return pwc_release(cam_fd, siz);
}


/*
 * pwc_get_frame - get the next frame from the camera
 *
 * given:
 *      cam_fd   open camera descriptor
 *      siz      pointer to operation size and buffer structure
 *
 * returns:
 *      >= 0 ==> chaos octets obtained, < 0 ==> error
 *
 * NOTE: One must call pwc_msync() after processing each frame when
 *       mmapping, so that its buffer is returned to the driver.
 *
 * NOTE: To not block, call pwc_wait_frame() (or read select on the
 *       open file descriptor) before calling this function.
 *
 * NOTE: When mmapping, this function dequeues the next filled V4L2 buffer
 *	 and points siz->image and siz->chaos at it.  Unlike the V4L1
 *	 driver, it MUST be called before accessing the frame.
 */
int
pwc_get_frame(int cam_fd, struct opsize *siz)
{
    struct v4l2_buffer buf;	/* dequeued buffer */
    int op_ret = 0;		/* read count */
    int tries;			/* errored buffers skipped */

    /*
     * firewall
     */
    if (cam_fd < 0 || siz == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * read if reading
     */
    if (siz->use_read) {

	/*
	 * read the data
	 */
	if (siz->image == NULL || siz->image_len <= 0) {
	    DBG(LAVACAM_ERR_NOSIZE);
	    return LAVACAM_ERR_NOSIZE;
	}
	do {
	    op_ret = read(cam_fd, siz->image, siz->image_len);
	} while (op_ret < 0 && errno == EINTR);
	if (op_ret < 0) {
	    DBG(LAVACAM_ERR_IOERR);
	    return LAVACAM_ERR_IOERR;
	}
	if (op_ret != siz->image_len) {
	    DBG(LAVACAM_ERR_FRAME);
	    return LAVACAM_ERR_FRAME;
	}

    /*
     * otherwise dequeue the next filled buffer
     */
    } else {

	/* a frame not yet released by pwc_msync() is still current */
	if (siz->cur_buf >= 0) {
	    return siz->chaos_len;
	}

	for (tries = 0; ; ++tries) {
	    memset(&buf, 0, sizeof(buf));
	    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	    buf.memory = V4L2_MEMORY_MMAP;
	    if (pwc_xioctl(cam_fd, VIDIOC_DQBUF, &buf) < 0) {
		DBG(LAVACAM_ERR_IOERR);
		return LAVACAM_ERR_IOERR;
	    }
	    if ((int)buf.index >= siz->nbufs) {
		DBG(LAVACAM_ERR_FRAME);
		return LAVACAM_ERR_FRAME;
	    }
	    if (!(buf.flags & V4L2_BUF_FLAG_ERROR) &&
		(int)buf.bytesused >= siz->framesize) {
		break;
	    }
	    /* damaged or short frame: give it back and try the next one */
	    if (pwc_xioctl(cam_fd, VIDIOC_QBUF, &buf) < 0 ||
		tries >= PWC_DQBUF_TRIES) {
		DBG(LAVACAM_ERR_FRAME);
		return LAVACAM_ERR_FRAME;
	    }
	}
	siz->cur_buf = buf.index;
	siz->image = siz->buf_start[buf.index];
	siz->chaos = (u_int8_t *)siz->image + siz->mmap_lavaoff;
    }

    /*
     * report the amount of chaotic data returned
     */
    return siz->chaos_len;
}


/*
 * pwc_msync - release/sync after processing a mmap captured buffer
 *
 * given:
 *      cam_fd      open camera descriptor
 *      u_cam_p     pointer to camera state (unused with V4L2)
 *      siz         pointer to operation size and buffer structure
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 *
 * NOTE: If we a reading, we immediately return OK (0) after doing nothing.
 *
 * NOTE: This returns the current V4L2 buffer to the driver so that it can
 *	 be filled again.  siz->chaos must not be used after this call.
 */
int
pwc_msync(int cam_fd, union lavacam *u_cam_p, struct opsize *siz)
{
    struct v4l2_buffer buf;	/* buffer to requeue */

    /*
     * firewall
     */
    if (cam_fd < 0 || u_cam_p == NULL || siz == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    if (siz->use_read == TRUE) {
	/* reading, nothing to do */
	return LAVACAM_ERR_OK;
    }

    /*
     * nothing to release if no frame is held
     */
    if (siz->cur_buf < 0) {
	return LAVACAM_ERR_OK;
    }

    /*
     * release
     */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = siz->cur_buf;
    if (pwc_xioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
	DBG(LAVACAM_ERR_SYNC);
	return LAVACAM_ERR_SYNC;
    }
    siz->cur_buf = -1;
    return LAVACAM_ERR_OK;
}


/*
 * pwc_wait_frame - use select to wait for the next camera frame
 *
 * given:
 *      cam_fd      open camera descriptor
 *      max_wait    wait for up to this many seconds, <0.0 ==> infinite
 *
 * returns:
 *      1 ==> frame is ready, 0 ==> timeout, <0 ==> error
 */
int
pwc_wait_frame(int cam_fd, double max_wait)
{
    fd_set rd_set;		/* select mask for reading */
    struct timeval sel_wait = {0, 0};	/* select time, if >= 0 */
    int ret;			/* select return value */
    double start = 0.0;		/* pre-select time */
    double end;			/* post-select time */
    double delay;		/* time spent in select() or pselect() */

    /*
     * firewall
     */
    /* NOTE: timeout may be NULL */
    if (cam_fd < 0) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }

    /*
     * common select setup
     */
    FD_ZERO(&rd_set);
    FD_SET(cam_fd, &rd_set);
    if (max_wait >= 0.0) {
	sel_wait.tv_sec = double_to_tv_sec(max_wait);
	sel_wait.tv_usec = double_to_tv_usec(max_wait);
    }
    /* keep track of time selected in case of signals */
    if (max_wait >= 0.0) {
	start = right_now();
	if (start < 0.0) {
	    DBG(LAVAERR_GETTIME);
	    return LAVAERR_GETTIME;
	}
    }

    /*
     * wait for the next frame or until we wait too long (if max_wait)
     */
    do {
	/* clear system error code */
	errno = 0;

	/* select may clear the descriptor set when interrupted */
	FD_ZERO(&rd_set);
	FD_SET(cam_fd, &rd_set);

	/*
	 * select
	 */
#if defined(HAVE_PSELECT)
	if (max_wait >= 0.0) {
	    struct timespec psel_wait;	/* pselect takes a timespec */

	    psel_wait.tv_sec = sel_wait.tv_sec;
	    psel_wait.tv_nsec = sel_wait.tv_usec * 1000;
	    ret = pselect(cam_fd + 1, &rd_set, NULL, NULL, &psel_wait, NULL);
	} else {
	    ret = pselect(cam_fd + 1, &rd_set, NULL, NULL, NULL, NULL);
	}
#else /* HAVE_PSELECT */
	if (max_wait >= 0.0) {
	    ret = select(cam_fd + 1, &rd_set, NULL, NULL, &sel_wait);
	} else {
	    ret = select(cam_fd + 1, &rd_set, NULL, NULL, NULL);
	}
#endif /* HAVE_PSELECT */

	/*
	 * adjust select time based on how long we just waited
	 */
	if (ret < 0 && errno == EINTR && max_wait >= 0.0) {
	    end = right_now();
	    if (end < 0.0) {
		DBG(LAVAERR_GETTIME);
		return LAVAERR_GETTIME;
	    }
	    delay = end - start;
	    if (delay >= max_wait) {
		break;
	    }
	    sel_wait.tv_sec = double_to_tv_sec(max_wait - delay);
	    sel_wait.tv_usec = double_to_tv_usec(max_wait - delay);
	}
    } while (ret < 0 && errno == EINTR);

    /*
     * return select status
     */
    return ret;
}

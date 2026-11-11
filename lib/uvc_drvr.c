/*
 * uvc_drvr - USB Video Class (uvcvideo) camera driver for LavaRnd
 *
 * Most current webcams are UVC cameras run by the Linux uvcvideo driver.
 * This driver captures uncompressed YUYV frames over V4L2 and sets the
 * standard UVC controls (gain, exposure, sharpness, ...) through V4L2.
 *
 * Unlike pwc_drvr.c, settings are kept in each V4L2 control's own units:
 * there is no legacy V4L1 value scale to stay compatible with.
 *
 * This file is part of the LavaRnd revival fork (AI-assisted).  It is
 * covered by the GNU Lesser General Public License version 2.1, like the
 * rest of LavaRnd.  See the file COPYING.
 */


/*
 * NOTE: This is the raw interface to a specific camera.  You should be
 *       calling the lavacam_*() routines in camop.c instead of directly
 *       calling these functions.
 */


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>

#include "LavaRnd/rawio.h"
#include "LavaRnd/lavacam.h"
#include "LavaRnd/lavaerr.h"

#include "LavaRnd/have/cam_videodev.h"

#include <linux/videodev2.h>
#include "LavaRnd/v4l2cam.h"


/*
 * Special UVC driver debugging - not related to LavaRnd debugging
 */
#if defined(LAVA_UVC_DEBUG)
#  define DBG(c) fprintf(stderr, \
			  "Error: %s:%d: %d\n", __FILE__, __LINE__, (c))
#  define DBGS(s,v) fprintf(stderr, \
			  "Set: %s:%d: %s = %d\n", __FILE__, __LINE__ ,(s) ,(v))
#else
#  define DBG(c)
#  define DBGS(s,v)
#endif


/*
 * LavaRnd entropy optimal values
 *
 * Logitech Webcam C270 (USB 046d:0825), measured 2026-11-11 with the lens
 * covered by sweeping gain, exposure, brightness, contrast, sharpness and
 * saturation and measuring frame-to-frame (temporal) noise.  With these
 * values the luma noise is centred near 123 with no clipping, about
 * 5.7 bits/pixel of temporal noise in luma and 4.9 in chroma.  Without
 * sharpening the temporal noise is only ~2 bits/pixel and frames are 98%
 * correlated with the previous frame (fixed-pattern noise dominates).
 * See PORTING.md.
 */
static struct uvc_state cam_c270 = {
    240,			/* brightness: centres the dark noise */
    128,			/* contrast: most noise without clipping */
    255,			/* saturation: max chroma noise */
    255,			/* sharpness: amplifies temporal noise */
    255,			/* gain: max */
    UVC_FRAME_EXPOSURE,		/* exposure: one frame period */
    0,				/* do not lower the frame rate */
    0,				/* no auto white balance */
    UVC_UNSET,			/* white balance temperature: leave */
    0,				/* no backlight compensation */
    0,				/* no power line frequency filter */
    120,			/* image height in pixels */
    160,			/* image width in pixels */
    30,				/* frame rate */
    0, 0, 0, 0, 0, 0, 0, 0, 0,	/* read only */
    "",				/* card name */
    UVC_STATE_MASK & ~UVC_STATE_wb_temp,	/* what to set */
    0,				/* video input */
    0, 0.0, 0, 0.0		/* tmp sanity check values */
};

/*
 * a starting point for other UVC cameras
 *
 * Maximise gain, sharpness and saturation, expose for a full frame and turn
 * off automatic processing.  Brightness and contrast are left alone because
 * their best values depend on the camera; tune them with -b and -n so the
 * dark noise is not clipped at black, then calibrate with camsanity.
 */
static struct uvc_state cam_generic = {
    UVC_UNSET, UVC_UNSET,	/* brightness, contrast: leave */
    UVC_MAX, UVC_MAX, UVC_MAX,	/* saturation, sharpness, gain: max */
    UVC_FRAME_EXPOSURE,		/* exposure: one frame period */
    0, 0, UVC_UNSET, 0, 0,	/* dyn fps, auto wb, wb temp, backlight, power */
    120, 160, 30,		/* 160x120 at 30 fps (or the nearest) */
    0, 0, 0, 0, 0, 0, 0, 0, 0,	/* read only */
    "",				/* card name */
    UVC_STATE_MASK & ~(UVC_STATE_bright | UVC_STATE_contrast |
		       UVC_STATE_wb_temp),
    0,				/* video input */
    0, 0.0, 0, 0.0		/* tmp sanity check values */
};


/*
 * LavaRnd values for given models
 *
 * The sanity check values are 0 (checks disabled) until a camera has been
 * calibrated with camsanity.
 */
struct lava_state {
    int model;			/* camera model - see struct camop in camop.c */
    struct uvc_state *state;	/* optimal LavaRnd entropy values */
    double warmup;		/* default seconds to delay during open */
    int def_top_x;		/* default ignore top_x octets as common */
    double def_min_fract;	/* default uncommon octet value fraction */
    int def_half_x;		/* default 1/2 level value */
    double def_diff_fract;	/* default different/same fraction */
};
static struct lava_state lava_state[] = {
    /*
     * Logitech C270 (USB 046d:0825), measured
     * 2026-11-11: 131072 frames of
     *
     *	camsanity c270 /dev/video0 DIR 131072 64 -L -T 10 -x 0 -d 0
     *
     * Limits are 5 standard deviations below the observed minimums.
     * top_x is 2 because ignoring the 2 most common octets leaves an
     * average uncommon fraction (0.637) closest to 0.6826895.
     *
     *	top_2 uncommon fraction:  min 0.605443  sd 0.012940
     *	half level:		  min 3         sd 0.3006
     *	bitdiff fraction:	  min 0.238734  sd 0.009072
     */
    {270, &cam_c270, 3.0, 2, 0.540744, 1, 0.193374},
    {0, &cam_generic, 3.0, 0, 0.0, 0, 0.0},

    {-1, NULL, 0.0, 0, 0, 0.0, 0.0}	/* must be last */
};

#define STATE_COUNT ((int)sizeof(lava_state) / (int)sizeof(lava_state[0]))


/*
 * uvc_model - determine which LavaRnd value model applies
 *
 * returns:
 *      index into lava_state[] to use, or <0 (LAVACAM_ERR_IDENT) ==> error
 */
static int
uvc_model(int model)
{
    int i;

    for (i = 0; i < STATE_COUNT; ++i) {
	if (lava_state[i].model < 0) {
	    break;
	}
	if (model == lava_state[i].model) {
	    return i;
	}
    }
    return LAVACAM_ERR_IDENT;
}


/*
 * uvc_identify - verify that the device is a V4L2 capture device run by uvcvideo
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 */
static int
uvc_identify(int cam_fd, struct v4l2_capability *vcap, __u32 *caps)
{
    memset(vcap, 0, sizeof(*vcap));
    if (v4l2cam_xioctl(cam_fd, VIDIOC_QUERYCAP, vcap) < 0) {
	return LAVACAM_ERR_NOCAM;
    }
    if (vcap->capabilities & V4L2_CAP_DEVICE_CAPS) {
	*caps = vcap->device_caps;
    } else {
	*caps = vcap->capabilities;
    }
    if (!(*caps & V4L2_CAP_VIDEO_CAPTURE)) {
	/* such as the uvcvideo metadata node next to the camera */
	return LAVACAM_ERR_NOCAM;
    }
    if (strncmp((char *)vcap->driver, "uvcvideo", sizeof(vcap->driver)) != 0) {
	return LAVACAM_ERR_TYPE;
    }
    return LAVACAM_ERR_OK;
}


/*
 * uvc_get_ctrl - get a control, or UVC_UNSET if the camera lacks it
 */
static int
uvc_get_ctrl(int cam_fd, __u32 id)
{
    struct v4l2_queryctrl q;
    int val;

    if (v4l2cam_ctrl_query(cam_fd, id, &q) < 0 ||
	v4l2cam_ctrl_get(cam_fd, id, &val) < 0) {
	return UVC_UNSET;
    }
    return val;
}


/*
 * uvc_set_ctrl - set a control, clamped to its range
 *
 * UVC_MAX means the control's maximum.  UVC_UNSET and controls the camera
 * does not offer are ignored.
 *
 * returns:
 *      0 ==> OK or ignored, <0 ==> the camera refused the value
 */
static int
uvc_set_ctrl(int cam_fd, __u32 id, int val, const char *name)
{
    struct v4l2_queryctrl q;

    if (val == UVC_UNSET || v4l2cam_ctrl_query(cam_fd, id, &q) < 0) {
	return 0;
    }
    if (val == UVC_MAX || val > q.maximum) {
	val = q.maximum;
    } else if (val < q.minimum) {
	val = q.minimum;
    }
    DBGS(name, val);
    (void)name;
    if (v4l2cam_ctrl_set(cam_fd, id, val) == V4L2CAM_CTRL_FAILED) {
	DBG(LAVACAM_ERR_SETPARAM);
	return LAVACAM_ERR_SETPARAM;
    }
    return 0;
}


/*
 * uvc_opsize - return mmap operation size and buffer structure
 *
 * NOTE: This function is only called from within uvc_open().
 */
static int
uvc_opsize(int cam_fd, union lavacam *u_cam_p, struct opsize *siz,
	   struct lavacam_flag *flag, int can_read)
{
    struct uvc_state *cam;	/* u_cam_p as uvc union element */
    struct v4l2_format fmt;	/* current capture format */
    int chaos_offset;		/* offset in frame for chaotic data */
    int chaos_len;		/* length of chaotic data in frame */
    int ret;

    if (cam_fd < 0 || u_cam_p == NULL || siz == NULL || flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    cam = &(u_cam_p->uvc);

    memset(siz, 0, sizeof(*siz));
    siz->prev_frame = NULL;
    siz->image = NULL;
    siz->chaos = NULL;
    siz->cur_buf = -1;
    siz->top_x = cam->tmp_top_x;
    siz->min_fract = cam->tmp_min_fract;
    siz->half_x = cam->tmp_half_x;
    siz->diff_fract = cam->tmp_diff_fract;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2cam_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
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
	/* padded lines or another layout the palette code does not know */
	DBG(LAVACAM_ERR_NOSIZE);
	return LAVACAM_ERR_NOSIZE;
    }
    cam->framesize = ret;
    cam->frames = V4L2CAM_BUFS;
    cam->bufsize = cam->frames * cam->framesize;

    ret = chaos_zone(PALLETTE_VIDEO4LINUX, cam->palette, cam->framesize,
		     cam->height, cam->width, &chaos_offset, &chaos_len);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }

    siz->mmapsize = cam->bufsize;
    siz->mmap_lavaoff = flag->A_flag ? 0 : chaos_offset;
    siz->mmap_lavalen = flag->A_flag ? cam->framesize : chaos_len;
    if (can_read) {
	siz->readsize = cam->framesize;
	siz->read_lavaoff = siz->mmap_lavaoff;
	siz->read_lavalen = siz->mmap_lavalen;
    }
    siz->frames = cam->frames;
    siz->palette = cam->palette;
    siz->height = cam->height;
    siz->width = cam->width;
    siz->framesize = cam->framesize;
    return LAVACAM_ERR_OK;
}


/*
 * uvc_get - get an open camera's state
 *
 * NOTE: This function only reads camera state; it may be called while
 *	 the camera is streaming.  It clears the tmp sanity check values.
 */
int
uvc_get(int cam_fd, union lavacam *u_cam_p)
{
    struct v4l2_capability vcap;	/* video capability */
    struct v4l2_format fmt;		/* current capture format */
    struct v4l2_streamparm parm;	/* streaming parameters */
    struct v4l2_frmsizeenum fsize;	/* supported frame size */
    struct uvc_state *cam;		/* u_cam_p as uvc union element */
    __u32 caps;				/* device capabilities */
    int val;
    int ret;
    int i;

    if (cam_fd < 0 || u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    cam = &(u_cam_p->uvc);
    memset(cam, 0, sizeof(cam[0]));

    ret = uvc_identify(cam_fd, &vcap, &caps);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }
    snprintf(cam->card, sizeof(cam->card), "%.*s",
	     (int)sizeof(vcap.card), (char *)vcap.card);

    /*
     * supported YUYV frame size range
     */
    for (i = 0; ; ++i) {
	int minw, maxw, minh, maxh;

	memset(&fsize, 0, sizeof(fsize));
	fsize.index = i;
	fsize.pixel_format = V4L2_PIX_FMT_YUYV;
	if (v4l2cam_xioctl(cam_fd, VIDIOC_ENUM_FRAMESIZES, &fsize) < 0) {
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
     * controls
     */
    cam->bright = uvc_get_ctrl(cam_fd, V4L2_CID_BRIGHTNESS);
    cam->contrast = uvc_get_ctrl(cam_fd, V4L2_CID_CONTRAST);
    cam->colour = uvc_get_ctrl(cam_fd, V4L2_CID_SATURATION);
    cam->sharpness = uvc_get_ctrl(cam_fd, V4L2_CID_SHARPNESS);
    cam->gain = uvc_get_ctrl(cam_fd, V4L2_CID_GAIN);
    cam->dyn_fps = uvc_get_ctrl(cam_fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY);
    cam->wb_auto = uvc_get_ctrl(cam_fd, V4L2_CID_AUTO_WHITE_BALANCE);
    cam->wb_temp = uvc_get_ctrl(cam_fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE);
    cam->backlight = uvc_get_ctrl(cam_fd, V4L2_CID_BACKLIGHT_COMPENSATION);
    cam->power_line = uvc_get_ctrl(cam_fd, V4L2_CID_POWER_LINE_FREQUENCY);
    val = uvc_get_ctrl(cam_fd, V4L2_CID_EXPOSURE_AUTO);
    if (val != UVC_UNSET && val != V4L2_EXPOSURE_MANUAL) {
	cam->exposure = UVC_AUTO;
    } else {
	cam->exposure = uvc_get_ctrl(cam_fd, V4L2_CID_EXPOSURE_ABSOLUTE);
    }

    /*
     * format: only YUYV is useful, anything else is reported as unset
     */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2cam_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	DBG(LAVACAM_ERR_GETPARAM);
	return LAVACAM_ERR_GETPARAM;
    }
    cam->height = fmt.fmt.pix.height;
    cam->width = fmt.fmt.pix.width;
    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
	cam->palette = VIDEO_PALETTE_YUYV;
	cam->depth = 16;
    } else {
	cam->palette = 0;
	cam->depth = 0;
    }

    /*
     * frame rate
     */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2cam_xioctl(cam_fd, VIDIOC_G_PARM, &parm) == 0 &&
	parm.parm.capture.timeperframe.numerator > 0) {
	cam->fps = parm.parm.capture.timeperframe.denominator /
		   parm.parm.capture.timeperframe.numerator;
    }
    return LAVACAM_ERR_OK;
}


/*
 * uvc_set - set an open camera's state
 *
 * Controls the camera does not offer are ignored, and values are clamped to
 * each control's range.  The size and frame rate cannot be changed while
 * streaming; uvc_open() calls this before streaming starts.
 */
int
uvc_set(int cam_fd, union lavacam *u_cam_p)
{
    struct v4l2_capability vcap;	/* video capability */
    struct v4l2_format fmt;		/* capture format */
    struct v4l2_streamparm parm;	/* streaming parameters */
    struct uvc_state *cam;		/* u_cam_p as uvc union element */
    __u32 caps;				/* device capabilities */
    int fps;				/* current frame rate */
    int ret;

    if (cam_fd < 0 || u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    cam = &(u_cam_p->uvc);
    if (cam->mask == 0) {
	return LAVACAM_ERR_OK;
    }
    ret = uvc_identify(cam_fd, &vcap, &caps);
    if (ret < 0) {
	DBG(ret);
	return ret;
    }

    /*
     * size, as YUYV; the camera picks its nearest supported size
     */
    if (UVC_TEST(cam->mask, height) || UVC_TEST(cam->mask, width)) {
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (v4l2cam_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	    DBG(LAVACAM_ERR_GETPARAM);
	    return LAVACAM_ERR_GETPARAM;
	}
	if (UVC_TEST(cam->mask, height)) {
	    fmt.fmt.pix.height = cam->height;
	}
	if (UVC_TEST(cam->mask, width)) {
	    fmt.fmt.pix.width = cam->width;
	}
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = 0;
	fmt.fmt.pix.sizeimage = 0;
	if (v4l2cam_xioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
    }

    /*
     * frame rate
     */
    if (UVC_TEST(cam->mask, fps) && cam->fps > 0) {
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = cam->fps;
	if (v4l2cam_xioctl(cam_fd, VIDIOC_S_PARM, &parm) < 0) {
	    DBG(LAVACAM_ERR_SETPARAM);
	    return LAVACAM_ERR_SETPARAM;
	}
    }

    /*
     * picture controls
     */
#define UVC_SET_CTRL(field, id) \
    if (UVC_TEST(cam->mask, field) && \
	uvc_set_ctrl(cam_fd, (id), cam->field, #field) < 0) { \
	return LAVACAM_ERR_SETPARAM; \
    }
    UVC_SET_CTRL(bright, V4L2_CID_BRIGHTNESS);
    UVC_SET_CTRL(contrast, V4L2_CID_CONTRAST);
    UVC_SET_CTRL(colour, V4L2_CID_SATURATION);
    UVC_SET_CTRL(sharpness, V4L2_CID_SHARPNESS);
    UVC_SET_CTRL(gain, V4L2_CID_GAIN);
    UVC_SET_CTRL(backlight, V4L2_CID_BACKLIGHT_COMPENSATION);
    UVC_SET_CTRL(power_line, V4L2_CID_POWER_LINE_FREQUENCY);
    UVC_SET_CTRL(dyn_fps, V4L2_CID_EXPOSURE_AUTO_PRIORITY);
    /* white balance mode before its temperature */
    UVC_SET_CTRL(wb_auto, V4L2_CID_AUTO_WHITE_BALANCE);
    UVC_SET_CTRL(wb_temp, V4L2_CID_WHITE_BALANCE_TEMPERATURE);

    /*
     * exposure: switch to manual first, then set the time
     */
    if (UVC_TEST(cam->mask, exposure) && cam->exposure != UVC_UNSET) {
	if (cam->exposure == UVC_AUTO) {
	    /* UVC cameras usually offer aperture priority as "auto" */
	    if (uvc_set_ctrl(cam_fd, V4L2_CID_EXPOSURE_AUTO,
			     V4L2_EXPOSURE_APERTURE_PRIORITY, "exposure_auto") < 0 &&
		uvc_set_ctrl(cam_fd, V4L2_CID_EXPOSURE_AUTO,
			     V4L2_EXPOSURE_AUTO, "exposure_auto") < 0) {
		return LAVACAM_ERR_SETPARAM;
	    }
	} else {
	    int exposure = cam->exposure;	/* in 100 usec units */

	    if (exposure == UVC_FRAME_EXPOSURE) {
		memset(&parm, 0, sizeof(parm));
		parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fps = 30;
		if (v4l2cam_xioctl(cam_fd, VIDIOC_G_PARM, &parm) == 0 &&
		    parm.parm.capture.timeperframe.numerator > 0 &&
		    parm.parm.capture.timeperframe.denominator > 0) {
		    fps = parm.parm.capture.timeperframe.denominator /
			  parm.parm.capture.timeperframe.numerator;
		}
		exposure = 10000 / (fps > 0 ? fps : 30);
	    }
	    if (uvc_set_ctrl(cam_fd, V4L2_CID_EXPOSURE_AUTO,
			     V4L2_EXPOSURE_MANUAL, "exposure_auto") < 0 ||
		uvc_set_ctrl(cam_fd, V4L2_CID_EXPOSURE_ABSOLUTE,
			     exposure, "exposure") < 0) {
		return LAVACAM_ERR_SETPARAM;
	    }
	}
    }
#undef UVC_SET_CTRL
    return LAVACAM_ERR_OK;
}


/*
 * uvc_LavaRnd - preset change state with LavaRnd entropy optimal values
 */
int
uvc_LavaRnd(union lavacam *u_cam_p, int model)
{
    int model_indx;

    if (u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    model_indx = uvc_model(model);
    if (model_indx < 0) {
	return model_indx;
    }
    memcpy(&(u_cam_p->uvc), lava_state[model_indx].state,
	   sizeof(u_cam_p->uvc));
    u_cam_p->uvc.tmp_top_x = lava_state[model_indx].def_top_x;
    u_cam_p->uvc.tmp_min_fract = lava_state[model_indx].def_min_fract;
    u_cam_p->uvc.tmp_half_x = lava_state[model_indx].def_half_x;
    u_cam_p->uvc.tmp_diff_fract = lava_state[model_indx].def_diff_fract;
    return LAVACAM_ERR_OK;
}


/*
 * uvc_check - determine if the camera state change is valid
 *
 * Control ranges differ between cameras and are clamped by uvc_set(), so
 * only values with a fixed meaning are checked here.
 */
int
uvc_check(union lavacam *u_cam_p)
{
    struct uvc_state *cam;

    if (u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    cam = &(u_cam_p->uvc);
    if ((UVC_TEST(cam->mask, height) && cam->height < 1) ||
	(UVC_TEST(cam->mask, width) && cam->width < 1) ||
	(UVC_TEST(cam->mask, fps) && cam->fps < 1) ||
	(UVC_TEST(cam->mask, exposure) && cam->exposure < UVC_FRAME_EXPOSURE &&
	 cam->exposure != UVC_UNSET) ||
	(UVC_TEST(cam->mask, dyn_fps) && cam->dyn_fps != 0 && cam->dyn_fps != 1) ||
	(UVC_TEST(cam->mask, wb_auto) && cam->wb_auto != 0 && cam->wb_auto != 1)) {
	DBG(LAVACAM_ERR_RANGE);
	return LAVACAM_ERR_RANGE;
    }
    return LAVACAM_ERR_OK;
}


/*
 * uvc_print_ctrl - print one control value
 */
static void
uvc_print_ctrl(FILE * stream, const char *name, int val)
{
    if (val == UVC_UNSET) {
	fprintf(stream, "\t%s: not offered by camera\n", name);
    } else {
	fprintf(stream, "\t%s: %d\n", name, val);
    }
}


/*
 * uvc_print - print state of an open camera
 *
 * returns:
 *      <0 ==> error, 1 ==> camera information printed
 */
int
uvc_print(FILE * stream, int cam_fd, union lavacam *u_cam_p,
	  struct opsize *siz)
{
    struct uvc_state *cam;

    if (stream == NULL || cam_fd < 0 || u_cam_p == NULL || siz == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    cam = &(u_cam_p->uvc);

    fprintf(stream, "\tCamera name: %s\n",
	    cam->card[0] ? cam->card : "<<__unknown__>>");
    fprintf(stream, "\tDriver: uvcvideo\n");
    uvc_print_ctrl(stream, "Brightness", cam->bright);
    uvc_print_ctrl(stream, "Contrast", cam->contrast);
    uvc_print_ctrl(stream, "Saturation", cam->colour);
    uvc_print_ctrl(stream, "Sharpness", cam->sharpness);
    uvc_print_ctrl(stream, "Gain", cam->gain);
    if (cam->exposure == UVC_AUTO) {
	fprintf(stream, "\tExposure: automatic\n");
    } else if (cam->exposure == UVC_UNSET) {
	fprintf(stream, "\tExposure: not offered by camera\n");
    } else {
	fprintf(stream, "\tExposure: %d (x 100 usec)\n", cam->exposure);
    }
    uvc_print_ctrl(stream, "Exposure may lower frame rate", cam->dyn_fps);
    uvc_print_ctrl(stream, "Auto white balance", cam->wb_auto);
    uvc_print_ctrl(stream, "White balance temperature", cam->wb_temp);
    uvc_print_ctrl(stream, "Backlight compensation", cam->backlight);
    uvc_print_ctrl(stream, "Power line frequency filter", cam->power_line);
    fprintf(stream, "\tMin width: %d\n\tMin height: %d\n"
		    "\tMax width: %d\n\tMax height: %d\n",
	    cam->minwidth, cam->minheight, cam->maxwidth, cam->maxheight);
    fprintf(stream, "\tWidth: %d\n\tHeight: %d\n\tFramerate (fps): %d\n",
	    cam->width, cam->height, cam->fps);
    if (siz->nbufs > 0) {
	fprintf(stream, "\tV4L2 mmap buffers: %d\n", siz->nbufs);
    } else {
	fprintf(stream, "\tNo V4L2 mmap buffers in use\n");
    }
    fprintf(stream, "\t%s (%d)\n",
	    palette_name(PALLETTE_VIDEO4LINUX, cam->palette), cam->palette);

    fprintf(stream, "\ncamera sanity check parameters:\n");
    if (siz->top_x == 0 && siz->min_fract == 0.0 && siz->half_x == 0 &&
	siz->diff_fract == 0.0) {
	fprintf(stream, "\tsanity checks disabled (camera not calibrated)\n");
    }
    fprintf(stream, "\t%d most frequent octet values are considered common\n",
	    siz->top_x);
    fprintf(stream, "\tmax fraction of frame containing common octet "
		    "values: %.6f\n", 1.0 - siz->min_fract);
    fprintf(stream, "\t%d most common values must be < 1/2 of frame octets\n",
	    siz->half_x);
    fprintf(stream, "\tmin fraction of bits same in next frame: %.6f\n",
	    siz->diff_fract);
    fprintf(stream, "\tmax fraction of bits same in next frame: %.6f\n",
	    1.0 - siz->diff_fract);

    fprintf(stream, "\ncamera use since this process opened the camera:\n");
    fprintf(stream, "\tcamera open time: %s", ctime(&siz->open_time));
    fprintf(stream, "\tframe count: %llu\n",
	    (unsigned long long)siz->frame_num);
    fprintf(stream, "\tinsane frame count: %llu\n",
	    (unsigned long long)siz->insane_cnt);

    fprintf(stream, "\ncamera op sizes:\n");
    if (siz->readsize > 0) {
	fprintf(stream, "\tread size: %d\n", siz->readsize);
    } else {
	fprintf(stream, "\treading not supported by camera\n");
    }
    fprintf(stream, "\tmmap frame size: %d\n", siz->framesize);
    fprintf(stream, "\tmmap frames: %d\n", siz->frames);
    fprintf(stream, "\tLavaRnd offset within frame: %d\n", siz->mmap_lavaoff);
    fprintf(stream, "\tLavaRnd length: %d\n", siz->mmap_lavalen);
    fprintf(stream, "\twill use %s I/O\n", siz->use_read ? "read" : "mmap");
    fprintf(stream, "\tchaos size: %d\n", siz->chaos_len);
    return 1;
}


/*
 * uvc_usage - print an argc/argv usage message
 */
void
uvc_usage(char *prog, char *typename)
{
    if (prog == NULL) {
	prog = "<<__NULL__>>";
    }
    if (typename == NULL) {
	typename = "<<__NULL__>>";
    }
    fprintf(stderr,
	"usage: %s %s devname [-flags ... args ...]\n"
	"\n"
	"Control values are in the camera's own units and are clamped to\n"
	"its range; see: v4l2-ctl -d devname --list-ctrls\n"
	"\n"
	"\t-b bright\tbrightness\n"
	"\t-n contrast\tcontrast\n"
	"\t-c colour\tsaturation\n"
	"\t-S sharp\tsharpness\n"
	"\t-a gain\t\tgain\n"
	"\t-s exposure\texposure in 100 usec units, -1==auto, -2==one frame\n"
	"\t-y 0|1\t\tlet auto exposure lower the frame rate\n"
	"\t-w 0|1\t\tautomatic white balance\n"
	"\t-K temp\t\twhite balance temperature\n"
	"\t-l backlight\tbacklight compensation\n"
	"\t-F freq\t\tpower line frequency filter (0==off)\n"
	"\t-H height\theight in pixels (>=1)\n"
	"\t-W width\twidth in pixels (>=1)\n"
	"\t-f fps\t\tframes per second (>=1)\n"
	"\t-v level\tverbose mode, print settings before/after\n"
	"\t-T tsec\t\twarm-up/sleep for tsec seconds\n"
	"\t-D tsec\t\tframe delay tsec seconds between frames\n"
	"\t-V vid_chan\tvideo input (>= 0)\n"
	"\t-M\t\tuse read() instead of mmap (most UVC cameras cannot)\n"
	"\t-L\t\tset LavaRnd entropy optimal mode\n"
	"\t-A\t\tprocess all of the image, not just the high entropy part\n"
	"\t-E\t\tavoid frame dumping when savefile exists\n"
	"\t-X top_x\ttop_x common octets are common octets [0, %d]\n"
	"\t-x min_fract\tmin fraction uncommon octets in frame [0.0, 1.0]\n"
	"\t-2 half_x\thalf_x common octets occupy <50%% octets [0, %d]\n"
	"\t-d min_fract\tmin fraction of diff bits between frames [0.0, 0.5]\n",
	prog, typename, OCTET_CNT, OCTET_CNT / 2);
}


/*
 * uvc_argv - argc/argv parse, sets a camera's state change and global flags
 *
 * returns:
 *      amount of args to skip, <0 ==> error
 *
 * NOTE: This initializes the tmp sanity check values to the model defaults.
 *	 -L reloads them; -X, -x, -2, -d change them.
 */
int
uvc_argv(int argc, char **argv, union lavacam *u_cam_p,
	 struct lavacam_flag *flag, int model)
{
    struct uvc_state *cam;	/* u_cam_p as uvc union element */
    char *optarg;		/* option argument */
    char *end;			/* end of parsed number */
    long lval;			/* parsed integer argument */
    double dval;		/* parsed floating argument */
    int model_indx;		/* default state model index */
    int c;			/* option */
    int i;

    if (argc <= 0 || argv == NULL || argv[0] == NULL || u_cam_p == NULL ||
	flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    flag->program = argv[0];
    cam = &(u_cam_p->uvc);
    model_indx = uvc_model(model);
    if (model_indx < 0) {
	return model_indx;
    }

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
    cam->tmp_top_x = lava_state[model_indx].def_top_x;
    cam->tmp_min_fract = lava_state[model_indx].def_min_fract;
    cam->tmp_half_x = lava_state[model_indx].def_half_x;
    cam->tmp_diff_fract = lava_state[model_indx].def_diff_fract;

    for (i = 1; i < argc; ++i) {
	if (argv[i] == NULL) {
	    fprintf(stderr, "%s: uvc_argv[%d] is NULL\n", argv[0], i);
	    return LAVACAM_ERR_ARG;
	}
	if (argv[i][0] != '-') {
	    break;
	}
	c = (int)argv[i][1];

	/*
	 * flags without an argument
	 */
	switch (c) {
	case 'M':
	    flag->M_flag = 0;
	    continue;
	case 'A':
	    flag->A_flag = 1;
	    continue;
	case 'E':
	    flag->E_flag = 1;
	    continue;
	case 'L':
	    if (uvc_LavaRnd(u_cam_p, model) < 0) {
		fprintf(stderr, "%s: LavaRnd mode set failed\n", flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    continue;
	}

	/*
	 * all other flags take one argument
	 */
	if (i >= argc - 1) {
	    fprintf(stderr, "%s: uvc_argv[%d] -%c missing next argument\n",
		    argv[0], i, c);
	    return LAVACAM_ERR_ARG;
	}
	optarg = argv[++i];
	errno = 0;
	lval = strtol(optarg, &end, 0);
	dval = atof(optarg);

#define INT_ARG(field, lo, what) \
	if (errno != 0 || *end != '\0' || lval < (lo) || lval > INT_MAX) { \
	    fprintf(stderr, "%s: %s must be an integer >= %d\n", \
		    flag->program, (what), (lo)); \
	    return LAVACAM_ERR_ARG; \
	} \
	cam->field = (int)lval; \
	UVC_SET(cam->mask, field); \
	break

	switch (c) {
	case 'b': INT_ARG(bright, INT_MIN + 1, "brightness");
	case 'n': INT_ARG(contrast, INT_MIN + 1, "contrast");
	case 'c': INT_ARG(colour, INT_MIN + 1, "saturation");
	case 'S': INT_ARG(sharpness, INT_MIN + 1, "sharpness");
	case 'a': INT_ARG(gain, INT_MIN + 1, "gain");
	case 's': INT_ARG(exposure, UVC_FRAME_EXPOSURE, "exposure");
	case 'y': INT_ARG(dyn_fps, 0, "dynamic frame rate (0 or 1)");
	case 'w': INT_ARG(wb_auto, 0, "auto white balance (0 or 1)");
	case 'K': INT_ARG(wb_temp, 0, "white balance temperature");
	case 'l': INT_ARG(backlight, 0, "backlight compensation");
	case 'F': INT_ARG(power_line, 0, "power line frequency");
	case 'H': INT_ARG(height, 1, "height");
	case 'W': INT_ARG(width, 1, "width");
	case 'f': INT_ARG(fps, 1, "frames per second");
	case 'P':
	    /* palette is not used - silently ignore, as pwc does */
	    break;
	case 'v':
	    if (lval < 0) {
		fprintf(stderr, "%s: verbose level must be >= 0\n",
			flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    flag->v_flag = (int)lval;
	    break;
	case 'T':
	    if (dval < 0.0) {
		fprintf(stderr, "%s: warm-up time must be >= 0.0\n",
			flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    flag->T_flag = dval;
	    break;
	case 'D':
	    if (dval < 0.0) {
		fprintf(stderr, "%s: delay time must be >= 0.0\n",
			flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    flag->D_flag = dval;
	    break;
	case 'V':
	    if (lval < 0) {
		fprintf(stderr, "%s: video input must be >= 0\n",
			flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    cam->channel = (int)lval;
	    break;
	case 'X':
	    if (lval < 0 || lval > OCTET_CNT) {
		fprintf(stderr, "%s: top_x must be >= 0 and <= %d\n",
			flag->program, OCTET_CNT);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_top_x = (int)lval;
	    break;
	case 'x':
	    if (dval < 0.0 || dval > 1.0) {
		fprintf(stderr, "%s: min_fract must be >= 0.0 and <= 1.0\n",
			flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_min_fract = dval;
	    break;
	case '2':
	    if (lval < 0 || lval > OCTET_CNT / 2) {
		fprintf(stderr, "%s: half_x must be >= 0 and <= %d\n",
			flag->program, OCTET_CNT / 2);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_half_x = (int)lval;
	    break;
	case 'd':
	    if (dval < 0.0 || dval > 0.5) {
		fprintf(stderr, "%s: diff_fract must be >= 0.0 and <= 0.5\n",
			flag->program);
		return LAVACAM_ERR_ARG;
	    }
	    cam->tmp_diff_fract = dval;
	    break;
	default:
	    fprintf(stderr, "%s: uvc_argv[%d] -%c is unknown\n",
		    argv[0], i - 1, c);
	    return LAVACAM_ERR_ARG;
	}
#undef INT_ARG
    }

    /*
     * parse an optional savefile/interval argument pair
     */
    if (i < argc && argv[i] != NULL && i + 1 < argc && argv[i + 1] != NULL) {
	flag->savefile = strdup(argv[i]);
	if (flag->savefile == NULL) {
	    return LAVAERR_MALLOC;
	}
	flag->newfile = (char *)malloc(strlen(flag->savefile) + sizeof(".new"));
	if (flag->newfile == NULL) {
	    free(flag->savefile);
	    flag->savefile = NULL;
	    return LAVAERR_MALLOC;
	}
	snprintf(flag->newfile, strlen(flag->savefile) + sizeof(".new"),
		 "%s.new", flag->savefile);
	flag->interval = atof(argv[i + 1]);
	if (flag->interval <= 0.0) {
	    fprintf(stderr, "%s: interval: %.3f must be >0.0\n",
		    flag->program, flag->interval);
	    free(flag->savefile);
	    flag->savefile = NULL;
	    free(flag->newfile);
	    flag->newfile = NULL;
	    return LAVACAM_ERR_ARG;
	}
	i += 2;
    }
    return i - 1;
}


/*
 * uvc_open - open a camera, verify its type, return opening camera state
 *
 * given:
 *      devname     camera device file
 *      model       camera model number
 *      o_cam_p     where to place the open camera state
 *      n_cam_p     is non-NULL, set this new state and channel number
 *      siz         pointer to operation size and buffer structure to fill in
 *      def         unused: UVC cameras have no saved user settings
 *      flag        flags set via lavacam_argv()
 *
 * returns:
 *      >=0 ==> camera file descriptor, <0 ==> error
 */
int
uvc_open(char *devname, int model, union lavacam *o_cam_p,
	 union lavacam *n_cam_p, struct opsize *siz, int def,
	 struct lavacam_flag *flag)
{
    struct uvc_state *cam;	/* o_cam_p as uvc union element */
    struct uvc_state *n_cam;	/* n_cam_p as uvc union element */
    union lavacam t_cam;	/* working camera state */
    struct v4l2_capability vcap;	/* video capability */
    struct v4l2_format fmt;	/* capture format */
    __u32 caps;			/* device capabilities */
    int input;			/* video input number */
    double open_warmup;		/* seconds to warm up the camera */
    int model_indx;		/* default state model index */
    int cam_fd;			/* open device file descriptor */
    int ret;

    (void)def;
    if (devname == NULL || o_cam_p == NULL || siz == NULL || flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    cam = &(o_cam_p->uvc);
    n_cam = (n_cam_p == NULL) ? NULL : &(n_cam_p->uvc);
    model_indx = uvc_model(model);
    if (model_indx < 0) {
	return model_indx;
    }

    cam_fd = open(devname, O_RDWR);
    if (cam_fd < 0) {
	if (errno == EACCES) {
	    DBG(LAVAERR_PERMOPEN);
	    return LAVAERR_PERMOPEN;
	}
	DBG(LAVACAM_ERR_OPEN);
	return LAVACAM_ERR_OPEN;
    }
    ret = uvc_identify(cam_fd, &vcap, &caps);
    if (ret < 0) {
	(void)close(cam_fd);
	return ret;
    }

    input = (n_cam == NULL ? 0 : n_cam->channel);
    if (v4l2cam_xioctl(cam_fd, VIDIOC_S_INPUT, &input) < 0) {
	(void)close(cam_fd);
	DBG(LAVACAM_ERR_CHAN);
	return LAVACAM_ERR_CHAN;
    }

    /*
     * select uncompressed YUYV, the only format LavaRnd can use here
     */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (v4l2cam_xioctl(cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
	(void)close(cam_fd);
	DBG(LAVACAM_ERR_GETPARAM);
	return LAVACAM_ERR_GETPARAM;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = 0;
	fmt.fmt.pix.sizeimage = 0;
	if (v4l2cam_xioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
	    (void)close(cam_fd);
	    DBG(LAVACAM_ERR_PALUNSET);
	    return LAVACAM_ERR_PALUNSET;
	}
    }

    /*
     * opening state, then the new state if one was given
     */
    ret = uvc_get(cam_fd, o_cam_p);
    if (ret < 0) {
	(void)close(cam_fd);
	return ret;
    }
    cam->channel = input;
    if (n_cam == NULL) {
	cam->tmp_top_x = lava_state[model_indx].def_top_x;
	cam->tmp_min_fract = lava_state[model_indx].def_min_fract;
	cam->tmp_half_x = lava_state[model_indx].def_half_x;
	cam->tmp_diff_fract = lava_state[model_indx].def_diff_fract;
	memcpy(&t_cam, o_cam_p, sizeof(t_cam));
    } else {
	cam->tmp_top_x = n_cam->tmp_top_x;
	cam->tmp_min_fract = n_cam->tmp_min_fract;
	cam->tmp_half_x = n_cam->tmp_half_x;
	cam->tmp_diff_fract = n_cam->tmp_diff_fract;
	ret = uvc_check(n_cam_p);
	if (ret == 0) {
	    ret = uvc_set(cam_fd, n_cam_p);
	}
	if (ret == 0) {
	    ret = uvc_get(cam_fd, n_cam_p);
	}
	if (ret < 0) {
	    (void)close(cam_fd);
	    return ret;
	}
	n_cam->channel = input;
	n_cam->tmp_top_x = cam->tmp_top_x;
	n_cam->tmp_min_fract = cam->tmp_min_fract;
	n_cam->tmp_half_x = cam->tmp_half_x;
	n_cam->tmp_diff_fract = cam->tmp_diff_fract;
	memcpy(&t_cam, n_cam_p, sizeof(t_cam));
    }

    /*
     * frame layout, then read or mmap setup
     */
    ret = uvc_opsize(cam_fd, &t_cam, siz, flag,
		     (caps & V4L2_CAP_READWRITE) != 0);
    if (ret < 0) {
	(void)close(cam_fd);
	return ret;
    }
    siz->use_read = !flag->M_flag;
    if (siz->use_read) {
	if (siz->readsize <= 0) {
	    (void)close(cam_fd);
	    DBG(LAVACAM_ERR_NOREAD);
	    return LAVACAM_ERR_NOREAD;
	}
	siz->image_len = siz->readsize;
	siz->image = malloc(siz->image_len);
	if (siz->image == NULL) {
	    (void)close(cam_fd);
	    return LAVACAM_ERR_NOREAD;
	}
	siz->chaos = (u_int8_t *)siz->image + siz->read_lavaoff;
	siz->chaos_len = siz->read_lavalen;
    } else {
	siz->image_len = siz->framesize;
	siz->chaos_len = siz->mmap_lavalen;
	ret = v4l2cam_stream_start(cam_fd, siz);
	if (ret < 0) {
	    (void)close(cam_fd);
	    return ret;
	}
    }
    siz->prev_frame = calloc(1, siz->chaos_len);
    if (siz->prev_frame == NULL) {
	(void)v4l2cam_release(cam_fd, siz);
	return LAVAERR_MALLOC;
    }

    /*
     * warm up: toss frames for a while (UVC cameras often send black
     * frames for the first second after streaming starts)
     */
    open_warmup = (flag->T_flag > 0.0) ? flag->T_flag :
		  lava_state[model_indx].warmup;
    if (open_warmup > 0.0) {
	double now = right_now();
	double end = now + open_warmup;

	while (now >= 0.0 && now < end) {
	    if (v4l2cam_wait_frame(cam_fd, end - now) < 0 ||
		v4l2cam_get_frame(cam_fd, siz) < 0 ||
		v4l2cam_msync(cam_fd, siz) < 0) {
		(void)v4l2cam_release(cam_fd, siz);
		DBG(LAVACAM_ERR_WARMUP);
		return LAVACAM_ERR_WARMUP;
	    }
	    now = right_now();
	}
	if (now < 0.0) {
	    (void)v4l2cam_release(cam_fd, siz);
	    return LAVAERR_GETTIME;
	}
    }
    return cam_fd;
}


/*
 * uvc_close - close an open camera
 */
int
uvc_close(int cam_fd, struct opsize *siz, struct lavacam_flag *flag)
{
    if (cam_fd < 0 || siz == NULL || flag == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    if (flag->savefile != NULL) {
	free(flag->savefile);
	flag->savefile = NULL;
    }
    if (flag->newfile != NULL) {
	free(flag->newfile);
	flag->newfile = NULL;
    }
    flag->interval = 0.0;
    return v4l2cam_release(cam_fd, siz);
}


/*
 * uvc_get_frame, uvc_msync, uvc_wait_frame - see v4l2cam.c
 */
int
uvc_get_frame(int cam_fd, struct opsize *siz)
{
    return v4l2cam_get_frame(cam_fd, siz);
}

int
uvc_msync(int cam_fd, union lavacam *u_cam_p, struct opsize *siz)
{
    if (u_cam_p == NULL) {
	DBG(LAVACAM_ERR_ARG);
	return LAVACAM_ERR_ARG;
    }
    return v4l2cam_msync(cam_fd, siz);
}

int
uvc_wait_frame(int cam_fd, double max_wait)
{
    return v4l2cam_wait_frame(cam_fd, max_wait);
}

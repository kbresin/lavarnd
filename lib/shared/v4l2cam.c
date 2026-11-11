/*
 * v4l2cam - Video4Linux2 capture helpers shared by the camera drivers
 *
 * Originally part of the V4L2 port of pwc_drvr.c; split out so the pwc and
 * uvc drivers share one implementation of control access, mmap streaming,
 * frame get/release and frame waiting.
 *
 * This file is part of the LavaRnd revival fork (AI-assisted).  It is
 * covered by the GNU Lesser General Public License version 2.1, like the
 * rest of LavaRnd.  See the file COPYING.
 */


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>

#include "LavaRnd/rawio.h"
#include "LavaRnd/lavacam.h"
#include "LavaRnd/lavaerr.h"

#include "LavaRnd/have/cam_videodev.h"
#include "LavaRnd/have/have_pselect.h"

#include <linux/videodev2.h>
#include "LavaRnd/v4l2cam.h"


/*
 * debugging - see pwc_drvr.c
 */
#if defined(LAVA_V4L2CAM_DEBUG)
#  define DBG(c) fprintf(stderr, "Error: %s:%d: %d\n", __FILE__, __LINE__, (c))
#else
#  define DBG(c)
#endif


/*
 * v4l2cam_xioctl - ioctl that retries when interrupted by a signal
 */
int
v4l2cam_xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
	ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}


/*
 * v4l2cam_ctrl_query - look up a V4L2 control
 *
 * returns:
 *      0 ==> control exists and is enabled, <0 ==> not available
 */
int
v4l2cam_ctrl_query(int fd, __u32 id, struct v4l2_queryctrl *q)
{
    memset(q, 0, sizeof(*q));
    q->id = id;
    if (v4l2cam_xioctl(fd, VIDIOC_QUERYCTRL, q) < 0) {
	return -1;
    }
    if (q->flags & V4L2_CTRL_FLAG_DISABLED) {
	return -1;
    }
    return 0;
}


/*
 * v4l2cam_ctrl_get - get the raw value of a V4L2 control
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 */
int
v4l2cam_ctrl_get(int fd, __u32 id, int *val)
{
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    if (v4l2cam_xioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
	return -1;
    }
    *val = ctrl.value;
    return 0;
}


/*
 * v4l2cam_ctrl_set - set the raw value of a V4L2 control
 *
 * returns:
 *      V4L2CAM_CTRL_OK, V4L2CAM_CTRL_MISSING or V4L2CAM_CTRL_FAILED
 */
int
v4l2cam_ctrl_set(int fd, __u32 id, int val)
{
    struct v4l2_queryctrl q;
    struct v4l2_control ctrl;

    if (v4l2cam_ctrl_query(fd, id, &q) < 0) {
	return V4L2CAM_CTRL_MISSING;
    }
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = val;
    if (v4l2cam_xioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
	return V4L2CAM_CTRL_FAILED;
    }
    return V4L2CAM_CTRL_OK;
}


/*
 * v4l2cam_scale_to_ctrl - map a value in [0, full] onto a control's range
 */
int
v4l2cam_scale_to_ctrl(int val, int full, const struct v4l2_queryctrl *q)
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
 * v4l2cam_scale_from_ctrl - map a control value onto [0, full]
 */
int
v4l2cam_scale_from_ctrl(int val, int full, const struct v4l2_queryctrl *q)
{
    long long span = (long long)q->maximum - (long long)q->minimum;

    if (span <= 0) {
	return 0;
    }
    return (int)((((long long)val - q->minimum) * full + span / 2) / span);
}


/*
 * v4l2cam_ctrl_get_scaled - get a control value scaled onto [0, full]
 *
 * returns:
 *      0 ==> OK, <0 ==> control not available
 */
int
v4l2cam_ctrl_get_scaled(int fd, __u32 id, int full, int *out)
{
    struct v4l2_queryctrl q;
    int val;

    if (v4l2cam_ctrl_query(fd, id, &q) < 0 || v4l2cam_ctrl_get(fd, id, &val) < 0) {
	return -1;
    }
    *out = v4l2cam_scale_from_ctrl(val, full, &q);
    return 0;
}


/*
 * v4l2cam_ctrl_set_scaled - set a control from a value in [0, full]
 *
 * returns:
 *      V4L2CAM_CTRL_OK, V4L2CAM_CTRL_MISSING or V4L2CAM_CTRL_FAILED
 */
int
v4l2cam_ctrl_set_scaled(int fd, __u32 id, int full, int val)
{
    struct v4l2_queryctrl q;

    if (v4l2cam_ctrl_query(fd, id, &q) < 0) {
	return V4L2CAM_CTRL_MISSING;
    }
    return v4l2cam_ctrl_set(fd, id, v4l2cam_scale_to_ctrl(val, full, &q));
}


/*
 * v4l2cam_stream_stop - stop V4L2 streaming and release mmapped buffers
 */
void
v4l2cam_stream_stop(int cam_fd, struct opsize *siz)
{
    struct v4l2_requestbuffers req;	/* buffer release request */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i;

    (void)v4l2cam_xioctl(cam_fd, VIDIOC_STREAMOFF, &type);
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
    (void)v4l2cam_xioctl(cam_fd, VIDIOC_REQBUFS, &req);
}


/*
 * v4l2cam_stream_start - mmap V4L2 capture buffers, queue them, start streaming
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
int
v4l2cam_stream_start(int cam_fd, struct opsize *siz)
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
    req.count = V4L2CAM_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (v4l2cam_xioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
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
	if (v4l2cam_xioctl(cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
	    v4l2cam_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOMMAP);
	    return LAVACAM_ERR_NOMMAP;
	}
	if ((int)buf.length < siz->framesize) {
	    v4l2cam_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOSIZE);
	    return LAVACAM_ERR_NOSIZE;
	}
	siz->buf_len[i] = buf.length;
	siz->buf_start[i] = mmap(NULL, buf.length, PROT_READ, MAP_SHARED,
				 cam_fd, buf.m.offset);
	siz->nbufs = i + 1;
	if (siz->buf_start[i] == MAP_FAILED) {
	    siz->buf_start[i] = NULL;
	    v4l2cam_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOMMAP);
	    return LAVACAM_ERR_NOMMAP;
	}
	if (v4l2cam_xioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
	    v4l2cam_stream_stop(cam_fd, siz);
	    DBG(LAVACAM_ERR_NOMMAP);
	    return LAVACAM_ERR_NOMMAP;
	}
    }

    /*
     * start capturing
     */
    if (v4l2cam_xioctl(cam_fd, VIDIOC_STREAMON, &type) < 0) {
	v4l2cam_stream_stop(cam_fd, siz);
	DBG(LAVACAM_ERR_NOMMAP);
	return LAVACAM_ERR_NOMMAP;
    }
    siz->image = siz->buf_start[0];
    siz->chaos = (u_int8_t *)siz->image + siz->mmap_lavaoff;
    return LAVACAM_ERR_OK;
}


/*
 * v4l2cam_release - free frame buffers and close the camera descriptor
 *
 * given:
 *      cam_fd   open camera descriptor
 *      siz      pointer to operation size and buffer structure
 *
 * returns:
 *      0 ==> OK, <0 ==> error
 */
int
v4l2cam_release(int cam_fd, struct opsize *siz)
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
	v4l2cam_stream_stop(cam_fd, siz);
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
 * v4l2cam_get_frame - get the next frame from the camera
 *
 * given:
 *      cam_fd   open camera descriptor
 *      siz      pointer to operation size and buffer structure
 *
 * returns:
 *      >= 0 ==> chaos octets obtained, < 0 ==> error
 *
 * NOTE: One must call v4l2cam_msync() after processing each frame when
 *       mmapping, so that its buffer is returned to the driver.
 *
 * NOTE: To not block, call v4l2cam_wait_frame() (or read select on the
 *       open file descriptor) before calling this function.
 *
 * NOTE: When mmapping, this function dequeues the next filled V4L2 buffer
 *	 and points siz->image and siz->chaos at it.  Unlike the V4L1
 *	 driver, it MUST be called before accessing the frame.
 */
int
v4l2cam_get_frame(int cam_fd, struct opsize *siz)
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

	/* a frame not yet released by v4l2cam_msync() is still current */
	if (siz->cur_buf >= 0) {
	    return siz->chaos_len;
	}

	for (tries = 0; ; ++tries) {
	    memset(&buf, 0, sizeof(buf));
	    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	    buf.memory = V4L2_MEMORY_MMAP;
	    if (v4l2cam_xioctl(cam_fd, VIDIOC_DQBUF, &buf) < 0) {
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
	    if (v4l2cam_xioctl(cam_fd, VIDIOC_QBUF, &buf) < 0 ||
		tries >= V4L2CAM_DQBUF_TRIES) {
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
 * v4l2cam_msync - release/sync after processing a mmap captured buffer
 *
 * given:
 *      cam_fd      open camera descriptor
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
v4l2cam_msync(int cam_fd, struct opsize *siz)
{
    struct v4l2_buffer buf;	/* buffer to requeue */

    /*
     * firewall
     */
    if (cam_fd < 0 || siz == NULL) {
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
    if (v4l2cam_xioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
	DBG(LAVACAM_ERR_SYNC);
	return LAVACAM_ERR_SYNC;
    }
    siz->cur_buf = -1;
    return LAVACAM_ERR_OK;
}


/*
 * v4l2cam_wait_frame - use select to wait for the next camera frame
 *
 * given:
 *      cam_fd      open camera descriptor
 *      max_wait    wait for up to this many seconds, <0.0 ==> infinite
 *
 * returns:
 *      1 ==> frame is ready, 0 ==> timeout, <0 ==> error
 */
int
v4l2cam_wait_frame(int cam_fd, double max_wait)
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

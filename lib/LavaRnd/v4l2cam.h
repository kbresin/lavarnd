/*
 * v4l2cam - Video4Linux2 capture helpers shared by the camera drivers
 *
 * This file is part of the LavaRnd revival fork (AI-assisted).  It is
 * covered by the GNU Lesser General Public License version 2.1, like the
 * rest of LavaRnd.  See the file COPYING.
 */


#if !defined(__LAVARND_V4L2CAM_H__)
#  define __LAVARND_V4L2CAM_H__


#  include <sys/types.h>
#  include <linux/videodev2.h>


/*
 * the struct opsize and lavacam definitions
 */
#  include "LavaRnd/lavacam.h"


#  define V4L2CAM_BUFS (4)	/* number of V4L2 mmap buffers to request */
#  define V4L2CAM_DQBUF_TRIES (8)	/* max errored buffers to skip per frame */

/* v4l2cam_ctrl_set() and v4l2cam_ctrl_set_scaled() return values */
#  define V4L2CAM_CTRL_OK (0)		/* control was set */
#  define V4L2CAM_CTRL_MISSING (-1)	/* driver does not offer this control */
#  define V4L2CAM_CTRL_FAILED (-2)	/* control exists but could not be set */


/*
 * external functions
 */
extern int v4l2cam_xioctl(int fd, unsigned long request, void *arg);
extern int v4l2cam_ctrl_query(int fd, __u32 id, struct v4l2_queryctrl *q);
extern int v4l2cam_ctrl_get(int fd, __u32 id, int *val);
extern int v4l2cam_ctrl_set(int fd, __u32 id, int val);
extern int v4l2cam_scale_to_ctrl(int val, int full,
				 const struct v4l2_queryctrl *q);
extern int v4l2cam_scale_from_ctrl(int val, int full,
				   const struct v4l2_queryctrl *q);
extern int v4l2cam_ctrl_get_scaled(int fd, __u32 id, int full, int *out);
extern int v4l2cam_ctrl_set_scaled(int fd, __u32 id, int full, int val);
extern void v4l2cam_stream_stop(int cam_fd, struct opsize *siz);
extern int v4l2cam_stream_start(int cam_fd, struct opsize *siz);
extern int v4l2cam_release(int cam_fd, struct opsize *siz);
extern int v4l2cam_get_frame(int cam_fd, struct opsize *siz);
extern int v4l2cam_msync(int cam_fd, struct opsize *siz);
extern int v4l2cam_wait_frame(int cam_fd, double max_wait);


#endif /* __LAVARND_V4L2CAM_H__ */

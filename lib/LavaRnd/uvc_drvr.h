/*
 * uvc_drvr - USB Video Class (uvcvideo) camera driver for LavaRnd
 *
 * This file is part of the LavaRnd revival fork (AI-assisted).  It is
 * covered by the GNU Lesser General Public License version 2.1, like the
 * rest of LavaRnd.  See the file COPYING.
 */


#if !defined(__LAVARND_UVC_DRVR_H__)
#  define __LAVARND_UVC_DRVR_H__


#  include <stdio.h>
#  include <sys/types.h>


/*
 * external functions
 */
extern int uvc_get(int cam_fd, union lavacam *u_cam_p);
extern int uvc_set(int cam_fd, union lavacam *u_cam_p);
extern int uvc_LavaRnd(union lavacam *u_cam_p, int model);
extern int uvc_check(union lavacam *u_cam_p);
extern int uvc_print(FILE * stream, int cam_fd,
		     union lavacam *u_cam_p, struct opsize *siz);
extern void uvc_usage(char *prog, char *typename);
extern int uvc_argv(int argc, char **argv, union lavacam *u_cam_p,
		    struct lavacam_flag *flag, int model);
extern int uvc_open(char *devname, int model, union lavacam *o_cam,
		    union lavacam *n_cam, struct opsize *siz, int def,
		    struct lavacam_flag *flag);
extern int uvc_close(int cam_fd, struct opsize *siz,
		     struct lavacam_flag *flag);
extern int uvc_get_frame(int cam_fd, struct opsize *siz);
extern int uvc_msync(int cam_fd, union lavacam *u_cam_p, struct opsize *siz);
extern int uvc_wait_frame(int cam_fd, double max_wait);


#endif /* __LAVARND_UVC_DRVR_H__ */

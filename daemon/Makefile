#!/usr/bin/make
#
# daemons - LavaRnd daemons
#
# @(#) $Revision: 10.2 $
# @(#) $Id: Makefile,v 10.2 2003/08/25 11:14:07 lavarnd Exp $
#
# Copyright (c) 2000-2003 by Landon Curt Noll and Simon Cooper.
# All Rights Reserved.
#
# This is open software; you can redistribute it and/or modify it under
# the terms of the version 2.1 of the GNU Lesser General Public License
# as published by the Free Software Foundation.
#
# This software is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General
# Public License for more details.
#
# The file COPYING contains important info about Licenses and Copyrights.
# Please read the COPYING file for details about this open software.
#
# A copy of version 2.1 of the GNU Lesser General Public License is
# distributed with calc under the filename COPYING-LGPL.  You should have
# received a copy with calc; if not, write to Free Software Foundation, Inc.
# 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
#
# For more information on LavaRnd: http://www.LavaRnd.org
#
# Share and enjoy! :-)

# utilities
#
SHELL= /bin/sh
AWK= awk
BASENAME= basename
CAT= cat
CC= cc
CHMOD= chmod
CHOWN= chown
CMP= cmp
CP= cp
CTAGS= ctags
DATE= date
DIRNAME= dirname
EGREP= egrep
FGREP= fgrep
FIND= find
HEAD= head
INSTALL= install
LDD= ldd
LN= ln
MAKE= make
MAKEDEPEND= makedepend
MKDIR= mkdir
MV= mv
RM= rm
RSYNC= rsync
SED= sed
SORT= sort
TAIL= tail
TAR= tar
XARGS= xargs

# How to compile C programs
#
#LAVA_DEBUG=
LAVA_DEBUG= -DLAVA_DEBUG

DMALLOC_CC=
DMALLOC_LIB=
#DMALLOC_CC= -DDMALLOC
#DMALLOC_LIB= -ldmalloc

#CC_WARN=
CC_WARN= -Wall -Werror

#CC_OPT=
CC_OPT= -O3

#CC_DBG= ${CC_OPT}
#CC_DBG= -g ${CC_OPT}
CC_DBG= -g3 ${CC_OPT}

# DESTLIB	run-time library search path (rpath) built into programs
#
DESTLIB= /usr/lib

# user and host information
#
# LAVARND_VIDEODEV	special device file for the LavaRnd webcam
# LAVARND_CAMTYPE	camera type (use a name given by: tool/camset list all)
# LAVAPOOL_CHROOT	lavapool chroots under this directory at startup
#
LAVARND_VIDEODEV=/dev/video0
LAVARND_CAMTYPE= pwc730
LAVAPOOL_CHROOT=/

# distribution
#
# DIST		sub-directory under . where external distributions are formed
# BASESUBDIST	basename sub-directory under ${DIST} to form a tarball
# SUBDIST	sub-directory under ${DIST} used to form a tarball
# DISTDIR	source release path
# TARBALL	make of source tarball installed under DISTDIR
#
DIST= ./dist
BASESUBDIST= daemon
SUBDIST= LavaRnd/${BASESUBDIST}
DISTDIR= /var/tmp/LavaRnd-src
TARBALL= daemon.tar.gz

#########################################
#=-=-= end of common Makefile vars =-=-=#
#########################################

# src and .o files
#
HSRC= cfg_lavapool.h chan.h dbg.h pool.h simple_url.h lava_retry.h
CSRC= listener.c client.c chaos.c chan.c cfg_lavapool.c pool.c dbg.c \
	lavapool.c simple_url.c lavaurl.c
LAVAPOOL_OBJS= listener.o client.o chaos.o chan.o cfg_lavapool.o pool.o \
	dbg.o lavapool.o
SHSRC=
OBJS= ${LAVAPOOL_OBJS} simple_url.o lavaurl.o
#
LIB_BUILD_HSRC= ${LDIR}/have_getppid.h ${LDIR}/have_getprid.h \
	${LDIR}/have_gettime.h ${LDIR}/have_rusage.h \
	${LDIR}/have_sys_resource.h ${LDIR}/have_sys_time.h \
	${LDIR}/have_sys_times.h ${LDIR}/have_time.h \
	${LDIR}/have_uid_t.h ${LDIR}/have_urandom.h ${LDIR}/have_ustat.h \
	${LDIR}/have_ustat_h.h
#
SRC= ${HSRC} ${CSRC} ${SHSRC}

# what to build or have
#
DESTSBIN_TARGETS= lavaurl lavapool ${SHSRC}
CFG_TARGETS= cfg.lavapool
TARGETS= ${DESTSBIN_TARGETS} ${SHBIN_TARGETS} ${CFG_TARGETS}

# optional usb module parameter control
#

# C compiler control
#
CC_PIC=
#CC_PIC= -fPIC

#SHARED= -shared
#SHARED= -static
SHARED=

CC_INC= -I../lib

#LSUF= .a
LSUF= .so

#LDIR= ../lib
LDIR= ../lib/shared

#LD_LIB= -L${LDIR}
LD_LIB= -Wl,-rpath=${DESTLIB} -L${LDIR}

CFLAGS= ${LAVA_DEBUG} ${DMALLOC_CC} ${CC_WARN} ${CC_DBG} ${CC_PIC} ${SHARED} ${CC_INC}
CLINK= ${CC_DBG} ${LD_LIB} ${DMALLOC_LIB} ${SHARED}
LDFLAGS= ${CC_OPT} ${DMALLOC_LIB} ${SHARED}

# Force the modes on directories by setting FORCE to a non-empty string
#
FORCE=

#####################################
#=-=-= start of Makefile rules =-=-=#
#####################################

all: ${TARGETS} dist.sed COPYING COPYING-LGPL

lavapool: ${LAVAPOOL_OBJS} ${LDIR}/libLavaRnd_util${LSUF} \
	${LDIR}/libLavaRnd_cam${LSUF} ${LDIR}/libLavaRnd_raw${LSUF}
	${CC} ${CLINK} ${LAVAPOOL_OBJS} -lLavaRnd_util \
	      -lLavaRnd_cam -lLavaRnd_raw -lm -o lavapool

${LDIR}/libLavaRnd_util${LSUF}:
	cd ${LDIR}; $(MAKE) libLavaRnd_util${LSUF}

${LDIR}/libLavaRnd_cam${LSUF}:
	cd ${LDIR}; $(MAKE) libLavaRnd_cam${LSUF}

${LDIR}/libLavaRnd_raw${LSUF}:
	cd ${LDIR}; $(MAKE) libLavaRnd_raw${LSUF}

${LDIR}/liblava_return${LSUF}:
	cd ${LDIR}; $(MAKE) liblava_return${LSUF}

../lib/LavaRnd/have/pwc_cam.h:
	cd ../lib; $(MAKE) have

lavaurl: lavaurl.o dbg.o simple_url.o ${LDIR}/libLavaRnd_util${LSUF} \
	 ${LDIR}/liblava_return${LSUF}
	${CC} ${CLINK} lavaurl.o dbg.o simple_url.o \
		-lLavaRnd_util -llava_return -lm -o lavaurl

${LIB_BUILD_HSRC}:
	cd ${LDIR}; $(MAKE) hsrc

# untility rules
#
clean:
	${RM} -rf dist skel tags
	${RM} -f ${OBJS}
	${RM} -f dist.sed
	${RM} -f *.tmp

clobber: clean
	${RM} -f lavaurl lavapool

tags: ${CSRC} ${HSRC}
	${RM} -f tags
	${CTAGS} ${CSRC} ${HSRC}

# ccflags - output C compiler flag values
#
ccflags:
	@echo "${BASESUBDIST} CC= ${CC}"
	@echo "${BASESUBDIST} LD= ${LD}"
	@echo "${BASESUBDIST} LAVA_DEBUG= ${LAVA_DEBUG}"
	@echo "${BASESUBDIST} DMALLOC_CC= ${DMALLOC_CC}"
	@echo "${BASESUBDIST} DMALLOC_LIB= ${DMALLOC_LIB}"
	@echo "${BASESUBDIST} CC_WARN= ${CC_WARN}"
	@echo "${BASESUBDIST} CC_OPT= ${CC_OPT}"
	@echo "${BASESUBDIST} CC_DBG= ${CC_DBG}"
	@echo "${BASESUBDIST} CC_PIC= ${CC_PIC}"
	@echo "${BASESUBDIST} SHARED= ${SHARED}"
	@echo "${BASESUBDIST} CC_INC= ${CC_INC}"
	@echo "${BASESUBDIST} LD_LIB= ${LD_LIB}"
	@echo "${BASESUBDIST} CFLAGS= ${CFLAGS}"
	@echo "${BASESUBDIST} CLINK= ${CLINK}"
	@echo "${BASESUBDIST} LDFLAGS= ${LDFLAGS}"
	@echo "${BASESUBDIST} LSUF= ${LSUF}"
	@echo "${BASESUBDIST} LDIR= ${LDIR}"

# How to modify things as they are distributed for source code release
#
# Modify scripts and source, prior to forming the source to be distributed,
# to use a canonical set of paths and values.
#
# NOTE: We do not substitute INSERT_VIDEODEV_HERE and INSERT_CAMTYPE_HERE
#	when forming a distribution to permit the user's install rule
#	to do the job.
#
dist.sed: Makefile
	@echo building $@
	@${RM} -f $@
	@(echo 's:^LAVARND_VIDEODEV=.*:LAVARND_VIDEODEV=/dev/video0:'; \
	  echo 's:^LAVARND_USER=.*:LAVARND_USER=lava:'; \
	  echo 's:^LAVARND_GROUP=.*:LAVARND_GROUP=lava:'; \
	  echo 's:^LAVAPOOL_CHROOT=.*:LAVAPOOL_CHROOT=/:'; \
	  echo 's:^LAVAPOOL=.*:LAVAPOOL=/usr/sbin/lavapool:'; \
	  echo 's:^RANDOM_CFG=.*:RANDOM_CFG=/etc/LavaRnd/cfg.random:'; \
	  echo 's:^LAVAPOOL_CFG=.*:LAVAPOOL_CFG=/etc/LavaRnd/cfg.lavapool:'; \
	  echo 's:^DESTBIN=.*:DESTBIN=/usr/bin:'; \
	  echo 's:^DESTSBIN=.*:DESTSBIN=/usr/sbin:'; \
	) > $@
	@echo $@ built

# what and how we make source available via the web
#
dist: clobber dist.sed COPYING COPYING-LGPL
	@echo "=+= starting $@ for ${BASESUBDIST} =+=+="
	@echo form ${DIST} tree with dist.sed modified files for ${BASESUBDIST}
	${RM} -rf ${DIST}
	${MKDIR} -p ${DIST}/${SUBDIST}
	${CHMOD} 0755 ${DIST}/${SUBDIST}
	${CP} -p COPYING COPYING-LGPL ${DIST}/${SUBDIST}
	-@for i in ${SRC} ${CFG_TARGETS} Makefile; do \
	    echo "${SED} -f dist.sed < $$i > ${DIST}/${SUBDIST}/$$i"; \
	    ${SED} -f dist.sed < "$$i" > "${DIST}/${SUBDIST}/$$i"; \
	    echo "${CHMOD} 0644 ${DIST}/${SUBDIST}/$$i"; \
	    ${CHMOD} 0644 ${DIST}/${SUBDIST}/$$i; \
	done
	-@for i in ${SHSRC}; do \
	    echo "${SED} -f dist.sed < $$i > ${DIST}/${SUBDIST}/$$i"; \
	    ${SED} -f dist.sed < "$$i" > "${DIST}/${SUBDIST}/$$i"; \
	    echo "${CHMOD} 0755 ${DIST}/${SUBDIST}/$$i"; \
	    ${CHMOD} 0755 ${DIST}/${SUBDIST}/$$i; \
	done
	@echo set distribution permissions under ${DIST}
	${FIND} ${DIST} -type d | ${XARGS} ${CHMOD} 0775
	${FIND} ${DIST} ! -type d | ${XARGS} ${CHMOD} ugo-w,ugo+r
	${FIND} ${DIST} -name Makefile -o -name '*.cfg' -o -name 'cfg.*' | \
	    ${XARGS} ${CHMOD} 0666
	@echo list files being distributed under ${DISTDIR}/${BASESUBDIST}.list
	${RM} -f ${DISTDIR}/${BASESUBDIST}.list
	cd ${DIST}; ${FIND} . ! -type d | ${SED} -e 's:^\./LavaRnd/::' | \
	   ${SORT} > ${DISTDIR}/${BASESUBDIST}.list
	${CHMOD} 0444 ${DISTDIR}/${BASESUBDIST}.list
	@echo create the tarball ${TARBALL}
	cd ${DIST}; ${RM} -f ${TARBALL}
	cd ${DIST}; ${TAR} -zcvf ${TARBALL} ${SUBDIST}
	-@if [ ! -d "${DISTDIR}" -o -n "${FORCE}" ]; then \
	    echo "${MKDIR} -p ${DISTDIR}"; \
	    ${MKDIR} -p "${DISTDIR}"; \
	    echo "${CHMOD} 0775 ${DISTDIR}"; \
	    ${CHMOD} 0775 "${DISTDIR}"; \
	fi
	-@for i in ${TARBALL}; do \
	    ${RM} -f ${DISTDIR}/$$i.new; \
	    ${CP} -p ${DIST}/$$i ${DISTDIR}/$$i.new; \
	    ${CHMOD} 0444 "${DISTDIR}/$$i.new"; \
	    ${MV} -f ${DISTDIR}/$$i.new ${DISTDIR}/$$i; \
	    echo "distributed ${DISTDIR}/$$i"; \
	done
	${RM} -rf ${DIST}
	@echo "=+= ending $@ for ${BASESUBDIST} =+=+="

# dependency setup
#
depend: ${CSRC} ${HSRC}
	if [ -f Makefile.bak ]; then \
	    echo "Makefile.bak exists, remove or move it out of the way"; \
	    exit 1; \
	else \
	    true; \
	fi
	cd ../lib; $(MAKE) all
	echo forming skel
	-${RM} -rf skel
	${MKDIR} skel skel/${BASESUBDIST}
	${CP} ${CSRC} ${HSRC} skel/${BASESUBDIST}
	${LN} -s ../${LDIR} skel/lib
	-${RM} -f skel/${BASESUBDIST}/makedep.out
	echo skel formed
	echo forming dependency list
	echo "# DO NOT DELETE THIS LINE -- make depend depends on it." > \
	    skel/${BASESUBDIST}/makedep.out
	cd skel/${BASESUBDIST}; \
		${MAKEDEPEND} -w 1 -f makedep.out ${CC_INC} ${CSRC}
	-for i in ${CSRC}; do \
		echo "$$i" | ${SED} 's/^\(.*\)\.c/\1.o: \1.c/'; \
	done >> skel/${BASESUBDIST}/makedep.out
	echo dependency list formed
	echo forming new Makefile
	-${RM} -f Makefile.bak
	${MV} Makefile Makefile.bak
	${SED} -n '1,/^# DO NOT DELETE THIS LINE/p' Makefile.bak | \
		${SED} '$d' > Makefile
	${TAIL} +2 skel/${BASESUBDIST}/makedep.out | \
	    ${SED} -e 's;: /usr/include/LavaRnd/;: ../lib/LavaRnd/;' | \
	    ${EGREP} -v '/usr/include|/usr/lib/gcc-lib|/usr/src/|/boot/' | \
	    LANG=C ${SORT} -u >> Makefile
	-${RM} -rf skel
	-if cmp -s Makefile.bak Makefile; then \
		echo 'Makefile was already up to date'; \
		${MV} -f Makefile.bak Makefile; \
	else \
		echo 'new Makefile formed'; \
	fi

# DO NOT DELETE THIS LINE - make depend needs it

cfg_lavapool.o: ../lib/LavaRnd/cfg.h
cfg_lavapool.o: ../lib/LavaRnd/sha1.h
cfg_lavapool.o: cfg_lavapool.c
cfg_lavapool.o: cfg_lavapool.h
cfg_lavapool.o: dbg.h
chan.o: ../lib/LavaRnd/fnv1.h
chan.o: ../lib/LavaRnd/have/cam_videodev.h
chan.o: ../lib/LavaRnd/have/pwc_cam.h
chan.o: ../lib/LavaRnd/lavacam.h
chan.o: ../lib/LavaRnd/lavaerr.h
chan.o: ../lib/LavaRnd/pwc_drvr.h
chan.o: ../lib/LavaRnd/pwc_state.h
chan.o: ../lib/LavaRnd/rawio.h
chan.o: cfg_lavapool.h
chan.o: chan.c
chan.o: chan.h
chan.o: dbg.h
chan.o: pool.h
chaos.o: ../lib/LavaRnd/cfg.h
chaos.o: ../lib/LavaRnd/have/cam_videodev.h
chaos.o: ../lib/LavaRnd/have/pwc_cam.h
chaos.o: ../lib/LavaRnd/lava_debug.h
chaos.o: ../lib/LavaRnd/lavacam.h
chaos.o: ../lib/LavaRnd/lavaerr.h
chaos.o: ../lib/LavaRnd/lavaquality.h
chaos.o: ../lib/LavaRnd/pwc_drvr.h
chaos.o: ../lib/LavaRnd/pwc_state.h
chaos.o: ../lib/LavaRnd/rawio.h
chaos.o: cfg_lavapool.h
chaos.o: chan.h
chaos.o: chaos.c
chaos.o: dbg.h
chaos.o: pool.h
client.o: ../lib/LavaRnd/cfg.h
client.o: ../lib/LavaRnd/have/cam_videodev.h
client.o: ../lib/LavaRnd/have/pwc_cam.h
client.o: ../lib/LavaRnd/lavacam.h
client.o: ../lib/LavaRnd/lavaerr.h
client.o: ../lib/LavaRnd/pwc_drvr.h
client.o: ../lib/LavaRnd/pwc_state.h
client.o: ../lib/LavaRnd/rawio.h
client.o: cfg_lavapool.h
client.o: chan.h
client.o: client.c
client.o: dbg.h
client.o: pool.h
dbg.o: ../lib/LavaRnd/rawio.h
dbg.o: dbg.c
dbg.o: dbg.h
lavapool.o: ../lib/LavaRnd/cfg.h
lavapool.o: ../lib/LavaRnd/cleanup.h
lavapool.o: ../lib/LavaRnd/fetchlava.h
lavapool.o: ../lib/LavaRnd/have/cam_videodev.h
lavapool.o: ../lib/LavaRnd/have/pwc_cam.h
lavapool.o: ../lib/LavaRnd/lava_callback.h
lavapool.o: ../lib/LavaRnd/lavacam.h
lavapool.o: ../lib/LavaRnd/lavaerr.h
lavapool.o: ../lib/LavaRnd/lavaquality.h
lavapool.o: ../lib/LavaRnd/lavarnd.h
lavapool.o: ../lib/LavaRnd/pwc_drvr.h
lavapool.o: ../lib/LavaRnd/pwc_state.h
lavapool.o: ../lib/LavaRnd/rawio.h
lavapool.o: ../lib/LavaRnd/sha1.h
lavapool.o: cfg_lavapool.h
lavapool.o: chan.h
lavapool.o: dbg.h
lavapool.o: lavapool.c
lavapool.o: pool.h
lavaurl.o: ../lib/LavaRnd/cleanup.h
lavaurl.o: ../lib/LavaRnd/lavaerr.h
lavaurl.o: ../lib/LavaRnd/lavarnd.h
lavaurl.o: ../lib/LavaRnd/rawio.h
lavaurl.o: ../lib/LavaRnd/sha1.h
lavaurl.o: dbg.h
lavaurl.o: lava_retry.h
lavaurl.o: lavaurl.c
lavaurl.o: pool.h
lavaurl.o: simple_url.h
listener.o: ../lib/LavaRnd/cfg.h
listener.o: ../lib/LavaRnd/have/cam_videodev.h
listener.o: ../lib/LavaRnd/have/pwc_cam.h
listener.o: ../lib/LavaRnd/lavacam.h
listener.o: ../lib/LavaRnd/lavaerr.h
listener.o: ../lib/LavaRnd/pwc_drvr.h
listener.o: ../lib/LavaRnd/pwc_state.h
listener.o: ../lib/LavaRnd/rawio.h
listener.o: cfg_lavapool.h
listener.o: chan.h
listener.o: dbg.h
listener.o: listener.c
pool.o: ../lib/LavaRnd/lavaerr.h
pool.o: ../lib/LavaRnd/lavarnd.h
pool.o: ../lib/LavaRnd/rawio.h
pool.o: ../lib/LavaRnd/sha1.h
pool.o: cfg_lavapool.h
pool.o: dbg.h
pool.o: pool.c
pool.o: pool.h
simple_url.o: ../lib/LavaRnd/lavaerr.h
simple_url.o: ../lib/LavaRnd/rawio.h
simple_url.o: dbg.h
simple_url.o: lava_retry.h
simple_url.o: simple_url.c
simple_url.o: simple_url.h

/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __LXCFS_CGROUP_FUSE_H
#define __LXCFS_CGROUP_FUSE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
#endif

#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "macro.h"

extern int cg_getattr(const char *path, struct stat *sb);
extern int cg_mkdir(const char *path, mode_t mode);
extern int cg_rmdir(const char *path);
extern int cg_chmod(const char *path, mode_t mode);
extern int cg_chown(const char *path, uid_t uid, gid_t gid);
extern int cg_open(const char *path, struct fuse_file_info *fi);
extern int cg_read(const char *path, char *buf, size_t size, off_t offset,
		   struct fuse_file_info *fi);
extern int cg_opendir(const char *path, struct fuse_file_info *fi);
extern int cg_release(const char *path, struct fuse_file_info *fi);
extern int cg_releasedir(const char *path, struct fuse_file_info *fi);
extern int cg_write(const char *path, const char *buf, size_t size,
		    off_t offset, struct fuse_file_info *fi);
extern int cg_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		      off_t offset, struct fuse_file_info *fi);
extern int cg_access(const char *path, int mode);

#endif /* __LXCFS_CGROUP_FUSE_H */

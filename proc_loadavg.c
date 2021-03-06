/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
#endif

#define _FILE_OFFSET_BITS 64

#define __STDC_FORMAT_MACROS
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wait.h>
#include <linux/magic.h>
#include <linux/sched.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>

#include "bindings.h"
#include "config.h"
#include "cgroup_fuse.h"
#include "cgroups/cgroup.h"
#include "cgroups/cgroup_utils.h"
#include "memory_utils.h"
#include "utils.h"

/*
 * This parameter is used for proc_loadavg_read().
 * 1 means use loadavg, 0 means not use.
 */
static int loadavg = 0;

/* The function of hash table.*/
#define LOAD_SIZE 100 /*the size of hash_table */
#define FLUSH_TIME 5  /*the flush rate */
#define DEPTH_DIR 3   /*the depth of per cgroup */
/* The function of calculate loadavg .*/
#define FSHIFT		11		/* nr of bits of precision */
#define FIXED_1		(1<<FSHIFT)	/* 1.0 as fixed-point */
#define EXP_1		1884		/* 1/exp(5sec/1min) as fixed-point */
#define EXP_5		2014		/* 1/exp(5sec/5min) */
#define EXP_15		2037		/* 1/exp(5sec/15min) */
#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)
static volatile sig_atomic_t loadavg_stop = 0;

struct load_node {
	char *cg;  /*cg */
	unsigned long avenrun[3];		/* Load averages */
	unsigned int run_pid;
	unsigned int total_pid;
	unsigned int last_pid;
	int cfd; /* The file descriptor of the mounted cgroup */
	struct  load_node *next;
	struct  load_node **pre;
};

struct load_head {
	/*
	 * The lock is about insert load_node and refresh load_node.To the first
	 * load_node of each hash bucket, insert and refresh in this hash bucket is
	 * mutually exclusive.
	 */
	pthread_mutex_t lock;
	/*
	 * The rdlock is about read loadavg and delete load_node.To each hash
	 * bucket, read and delete is mutually exclusive. But at the same time, we
	 * allow paratactic read operation. This rdlock is at list level.
	 */
	pthread_rwlock_t rdlock;
	/*
	 * The rilock is about read loadavg and insert load_node.To the first
	 * load_node of each hash bucket, read and insert is mutually exclusive.
	 * But at the same time, we allow paratactic read operation.
	 */
	pthread_rwlock_t rilock;
	struct load_node *next;
};

static struct load_head load_hash[LOAD_SIZE]; /* hash table */

/*
 * locate_node() finds special node. Not return NULL means success.
 * It should be noted that rdlock isn't unlocked at the end of code
 * because this function is used to read special node. Delete is not
 * allowed before read has ended.
 * unlock rdlock only in proc_loadavg_read().
 */
static struct load_node *locate_node(char *cg, int locate)
{
	struct load_node *f = NULL;
	int i = 0;

	pthread_rwlock_rdlock(&load_hash[locate].rilock);
	pthread_rwlock_rdlock(&load_hash[locate].rdlock);
	if (load_hash[locate].next == NULL) {
		pthread_rwlock_unlock(&load_hash[locate].rilock);
		return f;
	}
	f = load_hash[locate].next;
	pthread_rwlock_unlock(&load_hash[locate].rilock);
	while (f && ((i = strcmp(f->cg, cg)) != 0))
		f = f->next;
	return f;
}

static void insert_node(struct load_node **n, int locate)
{
	struct load_node *f;

	pthread_mutex_lock(&load_hash[locate].lock);
	pthread_rwlock_wrlock(&load_hash[locate].rilock);
	f = load_hash[locate].next;
	load_hash[locate].next = *n;

	(*n)->pre = &(load_hash[locate].next);
	if (f)
		f->pre = &((*n)->next);
	(*n)->next = f;
	pthread_mutex_unlock(&load_hash[locate].lock);
	pthread_rwlock_unlock(&load_hash[locate].rilock);
}

int calc_hash(const char *name)
{
	unsigned int hash = 0;
	unsigned int x = 0;
	/* ELFHash algorithm. */
	while (*name) {
		hash = (hash << 4) + *name++;
		x = hash & 0xf0000000;
		if (x != 0)
			hash ^= (x >> 24);
		hash &= ~x;
	}
	return (hash & 0x7fffffff);
}

int proc_loadavg_read(char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	struct fuse_context *fc = fuse_get_context();
	struct file_info *d = INTTYPE_TO_PTR(fi->fh);
	pid_t initpid;
	char *cg;
	size_t total_len = 0;
	char *cache = d->buf;
	struct load_node *n;
	int hash;
	int cfd, rv = 0;
	unsigned long a, b, c;

	if (offset) {
		int left;

		if (offset > d->size)
			return -EINVAL;

		if (!d->cached)
			return 0;

		left = d->size - offset;
		total_len = left > size ? size : left;
		memcpy(buf, cache + offset, total_len);

		return total_len;
	}
	if (!loadavg)
		return read_file_fuse("/proc/loadavg", buf, size, d);

	initpid = lookup_initpid_in_store(fc->pid);
	if (initpid <= 1 || is_shared_pidns(initpid))
		initpid = fc->pid;

	cg = get_pid_cgroup(initpid, "cpu");
	if (!cg)
		return read_file_fuse("/proc/loadavg", buf, size, d);

	prune_init_slice(cg);
	hash = calc_hash(cg) % LOAD_SIZE;
	n = locate_node(cg, hash);

	/* First time */
	if (n == NULL) {
		cfd = get_cgroup_fd("cpu");
		if (cfd >= 0) {
			/*
			 * In locate_node() above, pthread_rwlock_unlock() isn't used
			 * because delete is not allowed before read has ended.
			 */
			pthread_rwlock_unlock(&load_hash[hash].rdlock);
			rv = 0;
			goto err;
		}
		do {
			n = malloc(sizeof(struct load_node));
		} while (!n);

		do {
			n->cg = malloc(strlen(cg)+1);
		} while (!n->cg);
		strcpy(n->cg, cg);
		n->avenrun[0] = 0;
		n->avenrun[1] = 0;
		n->avenrun[2] = 0;
		n->run_pid = 0;
		n->total_pid = 1;
		n->last_pid = initpid;
		n->cfd = cfd;
		insert_node(&n, hash);
	}
	a = n->avenrun[0] + (FIXED_1/200);
	b = n->avenrun[1] + (FIXED_1/200);
	c = n->avenrun[2] + (FIXED_1/200);
	total_len = snprintf(d->buf, d->buflen, "%lu.%02lu %lu.%02lu %lu.%02lu %d/%d %d\n",
		LOAD_INT(a), LOAD_FRAC(a),
		LOAD_INT(b), LOAD_FRAC(b),
		LOAD_INT(c), LOAD_FRAC(c),
		n->run_pid, n->total_pid, n->last_pid);
	pthread_rwlock_unlock(&load_hash[hash].rdlock);
	if (total_len < 0 || total_len >=  d->buflen) {
		lxcfs_error("%s\n", "Failed to write to cache");
		rv = 0;
		goto err;
	}
	d->size = (int)total_len;
	d->cached = 1;

	if (total_len > size)
		total_len = size;
	memcpy(buf, d->buf, total_len);
	rv = total_len;

err:
	free(cg);
	return rv;
}

/*
 * Find the process pid from cgroup path.
 * eg:from /sys/fs/cgroup/cpu/docker/containerid/cgroup.procs to find the process pid.
 * @pid_buf : put pid to pid_buf.
 * @dpath : the path of cgroup. eg: /docker/containerid or /docker/containerid/child-cgroup ...
 * @depth : the depth of cgroup in container.
 * @sum : return the number of pid.
 * @cfd : the file descriptor of the mounted cgroup. eg: /sys/fs/cgroup/cpu
 */
static int calc_pid(char ***pid_buf, char *dpath, int depth, int sum, int cfd)
{
	__do_free char *path = NULL;
	__do_close_prot_errno int fd = -EBADF;
	__do_fclose FILE *f = NULL;
	__do_closedir DIR *dir = NULL;
	struct dirent *file;
	size_t linelen = 0;
	char *line = NULL;
	int pd;
	char **pid;

	/* path = dpath + "/cgroup.procs" + /0 */
	path = malloc(strlen(dpath) + 20);
	if (!path)
		return sum;

	strcpy(path, dpath);
	fd = openat(cfd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return sum;

	dir = fdopendir(move_fd(fd));
	if (!dir)
		return sum;

	while (((file = readdir(dir)) != NULL) && depth > 0) {
		if (strcmp(file->d_name, ".") == 0)
			continue;

		if (strcmp(file->d_name, "..") == 0)
			continue;

		if (file->d_type == DT_DIR) {
			__do_free char *path_dir = NULL;

			/* path + '/' + d_name +/0 */
			path_dir = malloc(strlen(path) + 2 + sizeof(file->d_name));
			if (!path_dir)
				return sum;

			strcpy(path_dir, path);
			strcat(path_dir, "/");
			strcat(path_dir, file->d_name);
			pd = depth - 1;
			sum = calc_pid(pid_buf, path_dir, pd, sum, cfd);
		}
	}

	strcat(path, "/cgroup.procs");
	fd = openat(cfd, path, O_RDONLY);
	if (fd < 0)
		return sum;

	f = fdopen(move_fd(fd), "r");
	if (!f)
		return sum;

	while (getline(&line, &linelen, f) != -1) {
		pid = realloc(*pid_buf, sizeof(char *) * (sum + 1));
		if (!pid)
			return sum;
		*pid_buf = pid;

		*(*pid_buf + sum) = malloc(strlen(line) + 1);
		if (!*(*pid_buf + sum))
			return sum;

		strcpy(*(*pid_buf + sum), line);
		sum++;
	}

	return sum;
}

/*
 * calc_load calculates the load according to the following formula:
 * load1 = load0 * exp + active * (1 - exp)
 *
 * @load1: the new loadavg.
 * @load0: the former loadavg.
 * @active: the total number of running pid at this moment.
 * @exp: the fixed-point defined in the beginning.
 */
static unsigned long calc_load(unsigned long load, unsigned long exp,
			       unsigned long active)
{
	unsigned long newload;

	active = active > 0 ? active * FIXED_1 : 0;
	newload = load * exp + active * (FIXED_1 - exp);
	if (active >= load)
		newload += FIXED_1 - 1;

	return newload / FIXED_1;
}

/*
 * Return 0 means that container p->cg is closed.
 * Return -1 means that error occurred in refresh.
 * Positive num equals the total number of pid.
 */
static int refresh_load(struct load_node *p, char *path)
{
	__do_free char *line = NULL;
	char **idbuf;
	char proc_path[256];
	int i, ret, run_pid = 0, total_pid = 0, last_pid = 0;
	size_t linelen = 0;
	int sum, length;
	struct dirent *file;

	idbuf = malloc(sizeof(char *));
	if (!idbuf)
		return -1;

	sum = calc_pid(&idbuf, path, DEPTH_DIR, 0, p->cfd);
	/*  normal exit  */
	if (sum == 0)
		goto out;

	for (i = 0; i < sum; i++) {
		__do_closedir DIR *dp = NULL;

		/*clean up '\n' */
		length = strlen(idbuf[i]) - 1;
		idbuf[i][length] = '\0';
		ret = snprintf(proc_path, 256, "/proc/%s/task", idbuf[i]);
		if (ret < 0 || ret > 255) {
			lxcfs_error("%s\n",
				    "snprintf() failed in refresh_load.");
			i = sum;
			sum = -1;
			goto err_out;
		}

		dp = opendir(proc_path);
		if (!dp) {
			lxcfs_error("%s\n",
				    "Open proc_path failed in refresh_load.");
			continue;
		}
		while ((file = readdir(dp)) != NULL) {
			__do_fclose FILE *f = NULL;

			if (strncmp(file->d_name, ".", 1) == 0)
				continue;
			if (strncmp(file->d_name, "..", 1) == 0)
				continue;
			total_pid++;
			/* We make the biggest pid become last_pid.*/
			ret = atof(file->d_name);
			last_pid = (ret > last_pid) ? ret : last_pid;

			ret = snprintf(proc_path, 256, "/proc/%s/task/%s/status",
				       idbuf[i], file->d_name);
			if (ret < 0 || ret > 255) {
				lxcfs_error("%s\n", "snprintf() failed in refresh_load.");
				i = sum;
				sum = -1;
				goto err_out;
			}

			f = fopen(proc_path, "r");
			if (f != NULL) {
				while (getline(&line, &linelen, f) != -1) {
					/* Find State */
					if ((strncmp(line, "State", 5) == 0) &&
					    (strncmp(line, "State R", 7) == 0 ||
					     strncmp(line, "State D", 7) == 0))
						run_pid++;
					break;
				}
			}
		}
	}
	/*Calculate the loadavg.*/
	p->avenrun[0] = calc_load(p->avenrun[0], EXP_1, run_pid);
	p->avenrun[1] = calc_load(p->avenrun[1], EXP_5, run_pid);
	p->avenrun[2] = calc_load(p->avenrun[2], EXP_15, run_pid);
	p->run_pid = run_pid;
	p->total_pid = total_pid;
	p->last_pid = last_pid;

err_out:
	for (; i > 0; i--)
		free(idbuf[i-1]);
out:
	free(idbuf);
	return sum;
}

/* Delete the load_node n and return the next node of it. */
static struct load_node *del_node(struct load_node *n, int locate)
{
	struct load_node *g;

	pthread_rwlock_wrlock(&load_hash[locate].rdlock);
	if (n->next == NULL) {
		*(n->pre) = NULL;
	} else {
		*(n->pre) = n->next;
		n->next->pre = n->pre;
	}
	g = n->next;
	free_disarm(n->cg);
	free_disarm(n);
	pthread_rwlock_unlock(&load_hash[locate].rdlock);
	return g;
}

/*
 * Traverse the hash table and update it.
 */
static void *load_begin(void *arg)
{

	int i, sum, length, ret;
	struct load_node *f;
	int first_node;
	clock_t time1, time2;

	while (1) {
		if (loadavg_stop == 1)
			return NULL;

		time1 = clock();
		for (i = 0; i < LOAD_SIZE; i++) {
			pthread_mutex_lock(&load_hash[i].lock);
			if (load_hash[i].next == NULL) {
				pthread_mutex_unlock(&load_hash[i].lock);
				continue;
			}
			f = load_hash[i].next;
			first_node = 1;
			while (f) {
				__do_free char *path = NULL;

				length = strlen(f->cg) + 2;
					/* strlen(f->cg) + '.' or '' + \0 */
				path = malloc(length);
				if  (!path)
					goto out;

				ret = snprintf(path, length, "%s%s", dot_or_empty(f->cg), f->cg);
				if (ret < 0 || ret > length - 1) {
					/* snprintf failed, ignore the node.*/
					lxcfs_error("Refresh node %s failed for snprintf().\n", f->cg);
					goto out;
				}

				sum = refresh_load(f, path);
				if (sum == 0)
					f = del_node(f, i);
				else
out:					f = f->next;
				/* load_hash[i].lock locks only on the first node.*/
				if (first_node == 1) {
					first_node = 0;
					pthread_mutex_unlock(&load_hash[i].lock);
				}
			}
		}

		if (loadavg_stop == 1)
			return NULL;

		time2 = clock();
		usleep(FLUSH_TIME * 1000000 - (int)((time2 - time1) * 1000000 / CLOCKS_PER_SEC));
	}
}

/*
 * init_load initialize the hash table.
 * Return 0 on success, return -1 on failure.
 */
static int init_load(void)
{
	int i;
	int ret;

	for (i = 0; i < LOAD_SIZE; i++) {
		load_hash[i].next = NULL;
		ret = pthread_mutex_init(&load_hash[i].lock, NULL);
		if (ret != 0) {
			lxcfs_error("%s\n", "Failed to initialize lock");
			goto out3;
		}
		ret = pthread_rwlock_init(&load_hash[i].rdlock, NULL);
		if (ret != 0) {
			lxcfs_error("%s\n", "Failed to initialize rdlock");
			goto out2;
		}
		ret = pthread_rwlock_init(&load_hash[i].rilock, NULL);
		if (ret != 0) {
			lxcfs_error("%s\n", "Failed to initialize rilock");
			goto out1;
		}
	}
	return 0;
out1:
	pthread_rwlock_destroy(&load_hash[i].rdlock);
out2:
	pthread_mutex_destroy(&load_hash[i].lock);
out3:
	while (i > 0) {
		i--;
		pthread_mutex_destroy(&load_hash[i].lock);
		pthread_rwlock_destroy(&load_hash[i].rdlock);
		pthread_rwlock_destroy(&load_hash[i].rilock);
	}
	return -1;
}

static void load_free(void)
{
	struct load_node *f, *p;

	for (int i = 0; i < LOAD_SIZE; i++) {
		pthread_mutex_lock(&load_hash[i].lock);
		pthread_rwlock_wrlock(&load_hash[i].rilock);
		pthread_rwlock_wrlock(&load_hash[i].rdlock);
		if (load_hash[i].next == NULL) {
			pthread_mutex_unlock(&load_hash[i].lock);
			pthread_mutex_destroy(&load_hash[i].lock);
			pthread_rwlock_unlock(&load_hash[i].rilock);
			pthread_rwlock_destroy(&load_hash[i].rilock);
			pthread_rwlock_unlock(&load_hash[i].rdlock);
			pthread_rwlock_destroy(&load_hash[i].rdlock);
			continue;
		}

		for (f = load_hash[i].next; f;) {
			free_disarm(f->cg);
			p = f->next;
			free_disarm(f);
			f = p;
		}

		pthread_mutex_unlock(&load_hash[i].lock);
		pthread_mutex_destroy(&load_hash[i].lock);
		pthread_rwlock_unlock(&load_hash[i].rilock);
		pthread_rwlock_destroy(&load_hash[i].rilock);
		pthread_rwlock_unlock(&load_hash[i].rdlock);
		pthread_rwlock_destroy(&load_hash[i].rdlock);
	}
}

/* Return a positive number on success, return 0 on failure.*/
pthread_t load_daemon(int load_use)
{
	int ret;
	pthread_t pid;

	ret = init_load();
	if (ret == -1) {
		lxcfs_error("%s\n", "Initialize hash_table fails in load_daemon!");
		return 0;
	}
	ret = pthread_create(&pid, NULL, load_begin, NULL);
	if (ret != 0) {
		lxcfs_error("%s\n", "Create pthread fails in load_daemon!");
		load_free();
		return 0;
	}
	/* use loadavg, here loadavg = 1*/
	loadavg = load_use;
	return pid;
}

/* Returns 0 on success. */
int stop_load_daemon(pthread_t pid)
{
	int s;

	/* Signal the thread to gracefully stop */
	loadavg_stop = 1;

	s = pthread_join(pid, NULL); /* Make sure sub thread has been canceled. */
	if (s != 0) {
		lxcfs_error("%s\n", "stop_load_daemon error: failed to join");
		return -1;
	}

	load_free();
	loadavg_stop = 0;

	return 0;
}

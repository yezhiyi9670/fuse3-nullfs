/*
 * nullfs - a "black hole" filesystem for Linux, built on FUSE 3.
 *
 * A real, writable directory tree in which no file payload is ever stored:
 *
 *   - files and directories can be created at any path, at any depth;
 *   - write(2) succeeds and the bytes are discarded;
 *   - read(2) succeeds and returns zeroes (like /dev/zero);
 *   - the *logical* file size is remembered, so ls(1), stat(1) and friends
 *     see plausible sizes;
 *   - the volume always reports ~1 PiB free;
 *   - the namespace lives only in memory and disappears when the process
 *     exits.
 *
 * This is the Linux/FUSE counterpart of the WinFsp nullfs sample.  It exists
 * for programs that insist on writing large files which are never read back.
 *
 * Two working modes:
 *
 *   default          metadata mode: the whole tree is remembered, with an
 *                    optional eviction threshold (-n) and idle expiry (-T)
 *                    so that an unbounded stream of files cannot exhaust RAM.
 *   -B               stateless mode: file metadata is remembered only while a
 *                    handle is open, so memory is O(open handles).  Only
 *                    directories are kept, because FUSE's VFS needs them to
 *                    resolve paths at all (see README).
 *
 * Copyright (C) 2026, GPL-2.0-or-later.  Derived in spirit from
 * libfuse's example/null.c (Miklos Szeredi) and from the WinFsp memfs sample.
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <fuse_lowlevel.h> /* fuse_cmdline_help() */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* fuse.h refuses to compile without a 64-bit off_t. */
#if !defined(_FILE_OFFSET_BITS) || _FILE_OFFSET_BITS != 64
#error "compile with -D_FILE_OFFSET_BITS=64"
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u << 1)
#endif

#define NF_NAME_MAX    255
#define NF_PATH_MAX    4096
#define NF_BLOCK_SIZE  4096
#define NF_VOLUME_SIZE (1ULL << 50) /* 1 PiB: advertised, never allocated */

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

static struct {
	unsigned long max_nodes; /* 0 = never evict */
	unsigned long expire;    /* idle seconds before a file is forgotten */
	int stateless;           /* -B */
} cfg = {
	.max_nodes = 65536,
	.expire = 0,
	.stateless = 0,
};

static uid_t g_uid;
static gid_t g_gid;

/* ------------------------------------------------------------------ */
/* Data structures                                                    */
/* ------------------------------------------------------------------ */

struct node;

/* One name in one directory.  The name is stored inline to save an
 * allocation and a level of indirection. */
struct dentry {
	struct dentry *next;
	uint32_t hash;
	struct node *node;
	char name[];
};

struct node {
	char *name;        /* basename; NULL for the root */
	struct node *parent;

	/* Directory payload (type == DT_DIR only). */
	struct dentry **buckets;
	uint32_t nbuckets; /* power of two, 0 while empty */
	uint32_t nentries;

	/* Attributes. */
	uint8_t type; /* DT_DIR, DT_REG or DT_LNK */
	mode_t mode;
	uid_t uid;
	gid_t gid;
	off_t size; /* logical size; nothing backs it */
	char *target; /* symlink target */
	struct timespec atime, mtime, ctime;
	time_t last_use; /* for -T */

	unsigned open_count;

	/* Creation-ordered list used by eviction and by -T. */
	struct node *lru_prev, *lru_next;
	bool in_lru;
};

static struct node g_root;
static struct node *g_lru_head, *g_lru_tail;
static unsigned long g_nodes = 1; /* the root */
static time_t g_last_sweep;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void lock(void)
{
	pthread_mutex_lock(&g_lock);
}

static void unlock(void)
{
	pthread_mutex_unlock(&g_lock);
}

static struct timespec ts_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ts;
}

/* ------------------------------------------------------------------ */
/* Name hash and per-directory hash table                             */
/* ------------------------------------------------------------------ */

static uint32_t name_hash(const char *s)
{
	uint32_t h = 2166136261u; /* FNV-1a */

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static struct node *dir_lookup(const struct node *d, const char *name)
{
	uint32_t h;
	struct dentry *e;

	if (d->nbuckets == 0)
		return NULL;

	h = name_hash(name);
	for (e = d->buckets[h & (d->nbuckets - 1)]; e; e = e->next)
		if (e->hash == h && strcmp(e->name, name) == 0)
			return e->node;
	return NULL;
}

static int dir_rehash(struct node *d, uint32_t nb)
{
	struct dentry **nbk;
	uint32_t i;

	nbk = calloc(nb, sizeof(*nbk));
	if (nbk == NULL)
		return -1;

	for (i = 0; i < d->nbuckets; i++) {
		struct dentry *e = d->buckets[i];

		while (e) {
			struct dentry *next = e->next;
			uint32_t b = e->hash & (nb - 1);

			e->next = nbk[b];
			nbk[b] = e;
			e = next;
		}
	}
	free(d->buckets);
	d->buckets = nbk;
	d->nbuckets = nb;
	return 0;
}

static int dir_insert(struct node *d, struct node *child)
{
	struct dentry *e;
	uint32_t b;
	size_t len;

	if (d->nbuckets == 0 ||
	    (d->nentries + 1) * 4 > (size_t)d->nbuckets * 3) {
		uint32_t nb = d->nbuckets ? d->nbuckets * 2 : 16;

		if (dir_rehash(d, nb) < 0)
			return -1;
	}

	len = strlen(child->name);
	e = malloc(sizeof(*e) + len + 1);
	if (e == NULL)
		return -1;

	memcpy(e->name, child->name, len + 1);
	e->hash = name_hash(child->name);
	e->node = child;

	b = e->hash & (d->nbuckets - 1);
	e->next = d->buckets[b];
	d->buckets[b] = e;
	d->nentries++;
	return 0;
}

/* Remove the entry pointing at `child` (child->name is still valid). */
static void dir_remove(struct node *d, const struct node *child)
{
	uint32_t h;
	struct dentry **pp;

	if (d->nbuckets == 0)
		return;

	h = name_hash(child->name);
	pp = &d->buckets[h & (d->nbuckets - 1)];
	while (*pp) {
		if ((*pp)->node == child) {
			struct dentry *e = *pp;

			*pp = e->next;
			free(e);
			d->nentries--;
			return;
		}
		pp = &(*pp)->next;
	}
}

/* ------------------------------------------------------------------ */
/* Node lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void lru_append(struct node *n)
{
	n->lru_prev = g_lru_tail;
	n->lru_next = NULL;
	if (g_lru_tail)
		g_lru_tail->lru_next = n;
	else
		g_lru_head = n;
	g_lru_tail = n;
	n->in_lru = true;
}

static void lru_remove(struct node *n)
{
	if (!n->in_lru)
		return;

	if (n->lru_prev)
		n->lru_prev->lru_next = n->lru_next;
	else
		g_lru_head = n->lru_next;

	if (n->lru_next)
		n->lru_next->lru_prev = n->lru_prev;
	else
		g_lru_tail = n->lru_prev;

	n->lru_prev = n->lru_next = NULL;
	n->in_lru = false;
}

static struct node *node_new(uint8_t type, const char *name)
{
	struct node *n = calloc(1, sizeof(*n));

	if (n == NULL)
		return NULL;

	if (name) {
		n->name = strdup(name);
		if (n->name == NULL) {
			free(n);
			return NULL;
		}
	}

	n->type = type;
	n->mode = (type == DT_DIR) ? 0755 : 0666;
	n->uid = g_uid;
	n->gid = g_gid;
	n->atime = n->mtime = n->ctime = ts_now();
	n->last_use = time(NULL);
	lru_append(n);
	g_nodes++;
	return n;
}

/* Free `n` and everything below it.  Must not be called on the root. */
static void node_destroy(struct node *n)
{
	uint32_t i;

	if (n == &g_root) {
		/* Only ever used to clear the root's children; see root_clear. */
		return;
	}

	lru_remove(n);

	if (n->type == DT_DIR) {
		for (i = 0; i < n->nbuckets; i++) {
			struct dentry *e = n->buckets[i];

			while (e) {
				struct dentry *next = e->next;
				struct node *child = e->node;

				child->parent = NULL;
				free(e);
				node_destroy(child);
				e = next;
			}
		}
		free(n->buckets);
	}

	free(n->name);
	free(n->target);
	free(n);
	g_nodes--;
}

/*
 * Remove the name from its directory.  If handles are still open (unlink of
 * a file that is being written, which gcc, sqlite and friends do all the
 * time) the node survives as an orphan until the last one is closed.
 */
static void node_unlink(struct node *n)
{
	if (n->parent)
		dir_remove(n->parent, n);
	n->parent = NULL;

	if (n->type == DT_DIR || n->open_count == 0) {
		node_destroy(n);
		return;
	}
	lru_remove(n); /* nothing may evict it from under its handles */
}

static int node_attach(struct node *parent, struct node *child)
{
	if (dir_insert(parent, child) < 0)
		return -1;

	child->parent = parent;
	return 0;
}

static void root_clear(void)
{
	uint32_t i;

	for (i = 0; i < g_root.nbuckets; i++) {
		struct dentry *e = g_root.buckets[i];

		while (e) {
			struct dentry *next = e->next;
			struct node *child = e->node;

			child->parent = NULL;
			free(e);
			node_destroy(child);
			e = next;
		}
	}
	free(g_root.buckets);
	g_root.buckets = NULL;
	g_root.nbuckets = 0;
	g_root.nentries = 0;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                    */
/* ------------------------------------------------------------------ */

/* Walk a FUSE path.  Lock must be held. */
static struct node *node_lookup(const char *path)
{
	struct node *n = &g_root;
	const char *p = path;

	while (*p) {
		const char *slash;
		size_t len;
		char name[NF_NAME_MAX + 1];

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		slash = strchr(p, '/');
		len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 0 || len > NF_NAME_MAX)
			return NULL;
		if (n->type != DT_DIR)
			return NULL;

		memcpy(name, p, len);
		name[len] = '\0';

		n = dir_lookup(n, name);
		if (n == NULL)
			return NULL;

		p += len;
	}
	return n;
}

/*
 * Split `path` into the parent directory and the last component.
 * Returns the parent node (NULL if it does not exist) and fills `name`.
 * Sets *err to 0 on success, or -ENOENT / -ENOTDIR / -ENAMETOOLONG.
 */
static struct node *parent_of(const char *path, char *name, size_t namesz,
			      int *err)
{
	const char *slash = strrchr(path, '/');
	const char *base;
	size_t len;
	char dirpath[NF_PATH_MAX];
	struct node *parent;

	*err = 0;

	if (slash == NULL) { /* not reached through the high-level API */
		*err = -EINVAL;
		return NULL;
	}

	base = slash + 1;
	len = strlen(base);
	if (len == 0) { /* the mount point itself */
		*err = -EINVAL;
		return NULL;
	}
	if (len > NF_NAME_MAX) {
		*err = -ENAMETOOLONG;
		return NULL;
	}
	if (len + 1 > namesz) {
		*err = -ENAMETOOLONG;
		return NULL;
	}
	memcpy(name, base, len + 1);

	if (slash == path) {
		dirpath[0] = '/';
		dirpath[1] = '\0';
	} else {
		size_t dl = (size_t)(slash - path);

		if (dl >= sizeof(dirpath)) {
			*err = -ENAMETOOLONG;
			return NULL;
		}
		memcpy(dirpath, path, dl);
		dirpath[dl] = '\0';
	}

	parent = node_lookup(dirpath);
	if (parent == NULL) {
		*err = -ENOENT;
		return NULL;
	}
	if (parent->type != DT_DIR) {
		*err = -ENOTDIR;
		return NULL;
	}
	return parent;
}

/* ------------------------------------------------------------------ */
/* Attributes                                                         */
/* ------------------------------------------------------------------ */

static void fill_stat(const struct node *n, struct stat *st)
{
	memset(st, 0, sizeof(*st));

	st->st_uid = n->uid;
	st->st_gid = n->gid;
	st->st_blksize = NF_BLOCK_SIZE;
	st->st_blocks = 0; /* nothing is allocated, ever */
	st->st_atim = n->atime;
	st->st_mtim = n->mtime;
	st->st_ctim = n->ctime;

	switch (n->type) {
	case DT_DIR:
		st->st_mode = S_IFDIR | n->mode;
		st->st_nlink = 2;
		break;
	case DT_LNK:
		st->st_mode = S_IFLNK | n->mode;
		st->st_nlink = 1;
		st->st_size = (off_t)(n->target ? strlen(n->target) : 0);
		break;
	default:
		st->st_mode = S_IFREG | n->mode;
		st->st_nlink = 1;
		st->st_size = n->size;
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Eviction and idle expiry                                           */
/* ------------------------------------------------------------------ */

static bool on_path(const struct node *anc, const struct node *n)
{
	for (; n; n = n->parent)
		if (n == anc)
			return true;
	return false;
}

static bool subtree_has_open(const struct node *n)
{
	uint32_t i;

	if (n->open_count)
		return true;

	if (n->type != DT_DIR)
		return false;

	for (i = 0; i < n->nbuckets; i++) {
		const struct dentry *e;

		for (e = n->buckets[i]; e; e = e->next)
			if (subtree_has_open(e->node))
				return true;
	}
	return false;
}

/*
 * Forget one node, oldest first:
 *   1. the oldest closed regular file (or symlink),
 *   2. else the oldest closed empty directory,
 *   3. else the oldest closed directory together with its subtree.
 *
 * `protect` is the parent directory of the entry being created: neither it
 * nor anything above it may be evicted, and `keep` (the new entry itself)
 * is never evicted either.
 */
static bool evict_one(const struct node *protect, const struct node *keep)
{
	struct node *n;

	for (n = g_lru_head; n; n = n->lru_next) {
		if (n->type == DT_DIR || n->open_count || n == keep)
			continue;
		node_unlink(n);
		return true;
	}

	for (n = g_lru_head; n; n = n->lru_next) {
		if (n->type != DT_DIR || n == &g_root || n->open_count)
			continue;
		if (n->nentries != 0)
			continue;
		if (on_path(n, protect))
			continue;
		node_unlink(n);
		return true;
	}

	for (n = g_lru_head; n; n = n->lru_next) {
		if (n->type != DT_DIR || n == &g_root)
			continue;
		if (subtree_has_open(n))
			continue;
		if (on_path(n, protect))
			continue;
		node_unlink(n);
		return true;
	}

	return false;
}

static void maybe_evict(const struct node *protect, const struct node *keep)
{
	if (cfg.stateless || cfg.max_nodes == 0)
		return;

	while (g_nodes > cfg.max_nodes) {
		if (!evict_one(protect, keep))
			break; /* everything left is held open */
	}
}

/* Forget file metadata that has not been touched for cfg.expire seconds. */
static void maybe_expire(const struct node *protect)
{
	time_t now, interval;
	struct node *n;

	if (cfg.stateless || cfg.expire == 0)
		return;

	now = time(NULL);
	interval = (time_t)cfg.expire / 4;
	if (interval < 1)
		interval = 1;
	if (interval > 60)
		interval = 60;
	if (g_last_sweep != 0 && now - g_last_sweep < interval)
		return;
	g_last_sweep = now;

	for (n = g_lru_head; n;) {
		struct node *next = n->lru_next;

		if (n->type != DT_DIR && n->open_count == 0 && n != protect &&
		    now - n->last_use >= (time_t)cfg.expire)
			node_unlink(n);
		n = next;
	}
}

/* ------------------------------------------------------------------ */
/* FUSE operations                                                    */
/* ------------------------------------------------------------------ */

static struct node *fh_node(struct fuse_file_info *fi)
{
	if (fi && fi->fh)
		return (struct node *)(uintptr_t)fi->fh;
	return NULL;
}

static int nf_getattr(const char *path, struct stat *st,
		      struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);
	if (n == NULL)
		rc = -ENOENT;
	else
		fill_stat(n, st);
	unlock();
	return rc;
}

static int nf_readlink(const char *path, char *buf, size_t size)
{
	struct node *n;
	int rc = 0;

	lock();
	n = node_lookup(path);
	if (n == NULL)
		rc = -ENOENT;
	else if (n->type != DT_LNK)
		rc = -EINVAL;
	else
		snprintf(buf, size, "%s", n->target ? n->target : "");
	unlock();
	return rc;
}

static int nf_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void)dev;

	if (!S_ISREG(mode))
		return -EPERM; /* devices, fifos and sockets are not emulated */

	if (cfg.stateless)
		return 0;

	{
		char name[NF_NAME_MAX + 1];
		struct node *parent, *n;
		int err, rc = 0;

		lock();
		parent = parent_of(path, name, sizeof(name), &err);
		if (parent == NULL) {
			rc = err;
		} else if ((n = dir_lookup(parent, name)) != NULL) {
			rc = -EEXIST;
		} else {
			n = node_new(DT_REG, name);
			if (n == NULL) {
				rc = -ENOMEM;
			} else {
				n->mode = (mode & 07777) ? (mode & 07777) : 0666;
				if (node_attach(parent, n) < 0) {
					node_destroy(n);
					rc = -ENOMEM;
				} else {
					maybe_evict(parent, n);
				}
			}
		}
		unlock();
		return rc;
	}
}

static int nf_mkdir(const char *path, mode_t mode)
{
	char name[NF_NAME_MAX + 1];
	struct node *parent, *n;
	int err, rc = 0;

	lock();
	maybe_expire(&g_root);
	parent = parent_of(path, name, sizeof(name), &err);
	if (parent == NULL) {
		rc = err;
	} else if ((n = dir_lookup(parent, name)) != NULL) {
		rc = (n->type == DT_DIR) ? -EEXIST : -ENOTDIR;
	} else {
		n = node_new(DT_DIR, name);
		if (n == NULL) {
			rc = -ENOMEM;
		} else {
			n->mode = (mode & 07777) ? (mode & 07777) : 0755;
			if (node_attach(parent, n) < 0) {
				node_destroy(n);
				rc = -ENOMEM;
			} else {
				maybe_evict(parent, n);
			}
		}
	}
	unlock();
	return rc;
}

static int nf_unlink(const char *path)
{
	char name[NF_NAME_MAX + 1];
	struct node *parent, *n;
	int err, rc = 0;

	if (cfg.stateless)
		return 0; /* -B: report success, do nothing */

	lock();
	parent = parent_of(path, name, sizeof(name), &err);
	if (parent == NULL) {
		rc = err;
	} else if ((n = dir_lookup(parent, name)) == NULL) {
		rc = -ENOENT;
	} else if (n->type == DT_DIR) {
		rc = -EISDIR;
	} else {
		node_unlink(n);
	}
	unlock();
	return rc;
}

static int nf_rmdir(const char *path)
{
	char name[NF_NAME_MAX + 1];
	struct node *parent, *n;
	int err, rc = 0;

	if (cfg.stateless)
		return 0;

	lock();
	parent = parent_of(path, name, sizeof(name), &err);
	if (parent == NULL) {
		rc = err;
	} else if ((n = dir_lookup(parent, name)) == NULL) {
		rc = -ENOENT;
	} else if (n->type != DT_DIR) {
		rc = -ENOTDIR;
	} else if (n->nentries != 0) {
		rc = -ENOTEMPTY;
	} else {
		node_unlink(n);
	}
	unlock();
	return rc;
}

static int nf_symlink(const char *target, const char *linkpath)
{
	char name[NF_NAME_MAX + 1];
	struct node *parent, *n;
	int err, rc = 0;

	if (cfg.stateless)
		return 0;

	lock();
	parent = parent_of(linkpath, name, sizeof(name), &err);
	if (parent == NULL) {
		rc = err;
	} else if (dir_lookup(parent, name) != NULL) {
		rc = -EEXIST;
	} else {
		n = node_new(DT_LNK, name);
		if (n == NULL) {
			rc = -ENOMEM;
		} else {
			n->target = strdup(target);
			n->mode = 0777;
			if (n->target == NULL || node_attach(parent, n) < 0) {
				node_destroy(n);
				rc = -ENOMEM;
			} else {
				maybe_evict(parent, n);
			}
		}
	}
	unlock();
	return rc;
}

static int nf_link(const char *from, const char *to)
{
	(void)from;
	(void)to;
	return -EPERM; /* hard links are not emulated */
}

static int nf_rename(const char *from, const char *to, unsigned int flags)
{
	char name[NF_NAME_MAX + 1];
	struct node *src, *newparent, *dst;
	int err, rc = 0;

	if (cfg.stateless)
		return 0; /* -B: report success, do nothing */

	if (flags & RENAME_EXCHANGE)
		return -EINVAL;

	lock();

	src = node_lookup(from);
	if (src == NULL || src == &g_root) {
		rc = -ENOENT;
		goto out;
	}

	newparent = parent_of(to, name, sizeof(name), &err);
	if (newparent == NULL) {
		rc = err;
		goto out;
	}

	if (on_path(src, newparent)) {
		rc = -EINVAL; /* cannot move a directory into itself */
		goto out;
	}

	dst = dir_lookup(newparent, name);
	if (dst == src) {
		rc = 0; /* rename("a", "a") */
		goto out;
	}
	if (dst != NULL) {
		if (flags & RENAME_NOREPLACE) {
			rc = -EEXIST;
			goto out;
		}
		if (dst->type == DT_DIR && src->type != DT_DIR) {
			rc = -EISDIR;
			goto out;
		}
		if (dst->type != DT_DIR && src->type == DT_DIR) {
			rc = -ENOTDIR;
			goto out;
		}
		if (dst->type == DT_DIR && dst->nentries != 0) {
			rc = -ENOTEMPTY;
			goto out;
		}
		node_unlink(dst);
	}

	/* Re-key the source. */
	if (src->parent)
		dir_remove(src->parent, src);
	free(src->name);
	src->name = strdup(name);
	if (src->name == NULL || node_attach(newparent, src) < 0) {
		/* Out of memory: the entry is lost, but so is its data. */
		rc = -ENOMEM;
		goto out;
	}

out:
	unlock();
	return rc;
}

static int nf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);
	if (n == NULL) {
		rc = cfg.stateless ? 0 : -ENOENT;
	} else {
		n->mode = mode & 07777;
		n->ctime = ts_now();
	}
	unlock();
	return rc;
}

static int nf_chown(const char *path, uid_t uid, gid_t gid,
		    struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);
	if (n == NULL) {
		rc = cfg.stateless ? 0 : -ENOENT;
	} else {
		if (uid != (uid_t)-1)
			n->uid = uid;
		if (gid != (gid_t)-1)
			n->gid = gid;
		n->ctime = ts_now();
	}
	unlock();
	return rc;
}

static int nf_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	if (size < 0)
		return -EINVAL;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);
	if (n == NULL) {
		rc = cfg.stateless ? 0 : -ENOENT;
	} else if (n->type == DT_DIR) {
		rc = -EISDIR;
	} else {
		n->size = size; /* the "growth" is a lie, and it is free */
		n->mtime = n->ctime = ts_now();
		n->last_use = time(NULL);
	}
	unlock();
	return rc;
}

static int nf_utimens(const char *path, const struct timespec tv[2],
		      struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);
	if (n == NULL) {
		rc = cfg.stateless ? 0 : -ENOENT;
	} else {
		int i;

		for (i = 0; i < 2; i++) {
			struct timespec ts;

			if (tv[i].tv_nsec == UTIME_OMIT)
				continue;
			if (tv[i].tv_nsec == UTIME_NOW)
				ts = ts_now();
			else
				ts = tv[i];
			if (i == 0)
				n->atime = ts;
			else
				n->mtime = ts;
		}
		n->ctime = ts_now();
	}
	unlock();
	return rc;
}

static int nf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char name[NF_NAME_MAX + 1];
	struct node *parent, *n = NULL;
	int err, rc = 0;

	lock();
	maybe_expire(&g_root);
	parent = parent_of(path, name, sizeof(name), &err);
	if (parent == NULL) {
		rc = err;
	} else {
		n = dir_lookup(parent, name);
		if (n != NULL && n->type == DT_DIR) {
			rc = -EISDIR;
			n = NULL;
		} else if (n == NULL) {
			n = node_new(DT_REG, name);
			if (n == NULL) {
				rc = -ENOMEM;
			} else {
				n->mode = (mode & 07777) ? (mode & 07777) : 0666;
				if (node_attach(parent, n) < 0) {
					node_destroy(n);
					n = NULL;
					rc = -ENOMEM;
				} else {
					maybe_evict(parent, n);
				}
			}
		} else if (fi->flags & O_TRUNC) {
			n->size = 0;
		}
	}

	if (n != NULL) {
		n->open_count++;
		n->last_use = time(NULL);
		fi->fh = (uint64_t)(uintptr_t)n;
	}
	unlock();
	return rc;
}

static int nf_open(const char *path, struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	lock();
	n = node_lookup(path);

	if (n != NULL && n->type == DT_DIR) {
		rc = -EISDIR;
		n = NULL;
	} else if (n == NULL && (cfg.stateless || (fi->flags & O_CREAT))) {
		/*
		 * Either -B, or the kernel still has a dentry for a path we
		 * have already forgotten: materialise it again, so that
		 * O_CREAT never fails just because of eviction.
		 */
		char name[NF_NAME_MAX + 1];
		struct node *parent;
		int err;

		parent = parent_of(path, name, sizeof(name), &err);
		if (parent == NULL) {
			rc = err;
		} else {
			n = dir_lookup(parent, name);
			if (n != NULL && n->type == DT_DIR) {
				rc = -EISDIR;
				n = NULL;
			} else if (n == NULL) {
				n = node_new(DT_REG, name);
				if (n == NULL) {
					rc = -ENOMEM;
				} else if (node_attach(parent, n) < 0) {
					node_destroy(n);
					n = NULL;
					rc = -ENOMEM;
				} else {
					maybe_evict(parent, n);
				}
			}
		}
	} else if (n == NULL) {
		rc = -ENOENT;
	}

	if (n != NULL) {
		if (n->type != DT_DIR && (fi->flags & O_TRUNC))
			n->size = 0;
		n->open_count++;
		n->last_use = time(NULL);
		fi->fh = (uint64_t)(uintptr_t)n;
	}
	unlock();
	return rc;
}

static int nf_read(const char *path, char *buf, size_t size, off_t off,
		   struct fuse_file_info *fi)
{
	struct node *n;
	int rc;

	if (off < 0)
		return -EINVAL;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);

	if (n == NULL) {
		rc = -ENOENT;
	} else if (n->type == DT_DIR) {
		rc = -EISDIR;
	} else if (off >= n->size) {
		rc = 0; /* beyond the logical end: EOF */
	} else {
		uint64_t avail = (uint64_t)(n->size - off);
		size_t len = avail < (uint64_t)size ? (size_t)avail : size;

		memset(buf, 0, len); /* /dev/zero */
		rc = (int)len;
	}
	unlock();
	return rc;
}

static int nf_write(const char *path, const char *buf, size_t size, off_t off,
		    struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	(void)buf;

	if (off < 0 || (uint64_t)off + (uint64_t)size > (uint64_t)INT64_MAX)
		return -EFBIG;

	lock();
	n = fh_node(fi);
	if (n == NULL)
		n = node_lookup(path);

	if (n == NULL) {
		rc = -ENOENT;
	} else if (n->type == DT_DIR) {
		rc = -EISDIR;
	} else {
		off_t end = off + (off_t)size;

		if (end > n->size)
			n->size = end; /* remember the size, discard the data */
		n->mtime = n->ctime = ts_now();
		n->last_use = time(NULL);
		rc = (int)size;
	}
	unlock();
	return rc;
}

static int nf_statfs(const char *path, struct statvfs *st)
{
	(void)path;

	memset(st, 0, sizeof(*st));
	st->f_bsize = NF_BLOCK_SIZE;
	st->f_frsize = NF_BLOCK_SIZE;
	st->f_blocks = NF_VOLUME_SIZE / NF_BLOCK_SIZE;
	st->f_bfree = st->f_blocks;
	st->f_bavail = st->f_blocks;
	st->f_namemax = NF_NAME_MAX;

	lock();
	st->f_files = cfg.max_nodes ? cfg.max_nodes : (1UL << 20);
	st->f_ffree = st->f_files > g_nodes ? st->f_files - g_nodes : 0;
	st->f_favail = st->f_ffree;
	unlock();
	return 0;
}

static int nf_flush(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	(void)fi;
	return 0;
}

static int nf_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	(void)datasync;
	(void)fi;
	return 0;
}

static int nf_release(const char *path, struct fuse_file_info *fi)
{
	struct node *n;

	(void)path;

	lock();
	n = fh_node(fi);
	if (n != NULL) {
		if (n->open_count)
			n->open_count--;
		n->last_use = time(NULL);

		if (n->open_count == 0 && n->type != DT_DIR) {
			if (cfg.stateless)
				node_unlink(n); /* -B: nothing outlives a handle */
			else if (n->parent == NULL)
				node_destroy(n); /* unlinked while open */
		}
	}
	unlock();
	return 0;
}

static int nf_setxattr(const char *path, const char *name, const char *value,
		       size_t size, int flags)
{
	(void)path;
	(void)name;
	(void)value;
	(void)size;
	(void)flags;
	return 0; /* accepted and forgotten, so that archivers keep going */
}

static int nf_getxattr(const char *path, const char *name, char *value,
		       size_t size)
{
	(void)path;
	(void)name;
	(void)value;
	(void)size;
	return -ENODATA;
}

static int nf_listxattr(const char *path, char *list, size_t size)
{
	(void)path;
	(void)list;
	(void)size;
	return 0;
}

static int nf_removexattr(const char *path, const char *name)
{
	(void)path;
	(void)name;
	return 0;
}

static int nf_opendir(const char *path, struct fuse_file_info *fi)
{
	struct node *n;
	int rc = 0;

	(void)fi;

	lock();
	n = node_lookup(path);
	if (n == NULL)
		rc = -ENOENT;
	else if (n->type != DT_DIR)
		rc = -ENOTDIR;
	unlock();
	return rc;
}

static int nf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		      off_t off, struct fuse_file_info *fi,
		      enum fuse_readdir_flags flags)
{
	struct node *d;
	struct stat st;
	off_t idx = 0;
	int rc = 0;

	(void)fi;
	(void)flags;

	lock();
	d = node_lookup(path);
	if (d == NULL || d->type != DT_DIR) {
		rc = -ENOENT;
		goto out;
	}

	fill_stat(d, &st);

	if (idx >= off && filler(buf, ".", &st, idx + 1, 0) != 0)
		goto out;
	idx++;
	if (idx >= off && filler(buf, "..", &st, idx + 1, 0) != 0)
		goto out;
	idx++;

	{
		uint32_t i;

		for (i = 0; i < d->nbuckets; i++) {
			struct dentry *e;

			for (e = d->buckets[i]; e; e = e->next) {
				if (idx >= off) {
					struct stat cst;

					fill_stat(e->node, &cst);
					if (filler(buf, e->node->name, &cst,
						   idx + 1, 0) != 0)
						goto out;
				}
				idx++;
			}
		}
	}

out:
	unlock();
	return rc;
}

static int nf_releasedir(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	(void)fi;
	return 0;
}

static void nf_destroy(void *private_data)
{
	(void)private_data;

	lock();
	root_clear();
	unlock();
}

static const struct fuse_operations nf_oper = {
	.getattr = nf_getattr,
	.readlink = nf_readlink,
	.mknod = nf_mknod,
	.mkdir = nf_mkdir,
	.unlink = nf_unlink,
	.rmdir = nf_rmdir,
	.symlink = nf_symlink,
	.rename = nf_rename,
	.link = nf_link,
	.chmod = nf_chmod,
	.chown = nf_chown,
	.truncate = nf_truncate,
	.open = nf_open,
	.read = nf_read,
	.write = nf_write,
	.statfs = nf_statfs,
	.flush = nf_flush,
	.release = nf_release,
	.fsync = nf_fsync,
	.setxattr = nf_setxattr,
	.getxattr = nf_getxattr,
	.listxattr = nf_listxattr,
	.removexattr = nf_removexattr,
	.opendir = nf_opendir,
	.readdir = nf_readdir,
	.releasedir = nf_releasedir,
	.create = nf_create,
	.utimens = nf_utimens,
	.destroy = nf_destroy,
};

/* ------------------------------------------------------------------ */
/* Command line                                                       */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	printf("Usage: %s [options] <mountpoint>\n"
	       "\n"
	       "A \"black hole\" filesystem: files can be created anywhere, writes\n"
	       "are discarded, reads return zeroes, and the namespace lives only in\n"
	       "memory.\n"
	       "\n"
	       "nullfs options:\n"
	       "    -B              stateless mode: only directories are remembered;\n"
	       "                    file metadata exists only while a handle is open\n"
	       "    -n <nodes|0>    evict metadata above this many nodes\n"
	       "                    (default 65536, 0 = never evict)\n"
	       "    -T <seconds|0>  forget file metadata idle for this long\n"
	       "                    (default 0 = never)\n"
	       "    -h, --help      show this help\n"
	       "\n"
	       "FUSE options (passed through):\n",
	       prog);
	fuse_cmdline_help();
	printf("\nExamples:\n"
	       "    %s -f /mnt/null          # metadata mode, foreground\n"
	       "    %s -B -o allow_other /mnt/null\n",
	       prog, prog);
}

static unsigned long parse_ulong(const char *s, int *ok)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0') {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	return v;
}

int main(int argc, char *argv[])
{
	char **fargv;
	int fargc = 0;
	int i;

	fargv = calloc((size_t)argc + 1, sizeof(*fargv));
	if (fargv == NULL) {
		perror("nullfs");
		return 1;
	}
	fargv[fargc++] = argv[0];

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "-B") == 0) {
			cfg.stateless = 1;
		} else if (strcmp(a, "-h") == 0 ||
			   strcmp(a, "--help") == 0) {
			usage(argv[0]);
			free(fargv);
			return 0;
		} else if (strcmp(a, "-n") == 0 || strcmp(a, "-T") == 0) {
			int ok;
			unsigned long v;

			if (i + 1 >= argc) {
				fprintf(stderr, "nullfs: %s needs a value\n", a);
				free(fargv);
				return 1;
			}
			v = parse_ulong(argv[++i], &ok);
			if (!ok) {
				fprintf(stderr, "nullfs: bad value for %s\n", a);
				free(fargv);
				return 1;
			}
			if (a[1] == 'n')
				cfg.max_nodes = v;
			else
				cfg.expire = v;
		} else if (strncmp(a, "-n", 2) == 0 && a[2] != '\0') {
			int ok;
			unsigned long v = parse_ulong(a + 2, &ok);

			if (!ok) {
				fprintf(stderr, "nullfs: bad value for -n\n");
				free(fargv);
				return 1;
			}
			cfg.max_nodes = v;
		} else if (strncmp(a, "-T", 2) == 0 && a[2] != '\0') {
			int ok;
			unsigned long v = parse_ulong(a + 2, &ok);

			if (!ok) {
				fprintf(stderr, "nullfs: bad value for -T\n");
				free(fargv);
				return 1;
			}
			cfg.expire = v;
		} else {
			fargv[fargc++] = argv[i];
		}
	}

	if (cfg.max_nodes == 1) {
		fprintf(stderr,
			"nullfs: -n must be 0 (unlimited) or at least 2 "
			"(the root directory always occupies one slot)\n");
		free(fargv);
		return 1;
	}

	g_uid = getuid();
	g_gid = getgid();

	memset(&g_root, 0, sizeof(g_root));
	g_root.type = DT_DIR;
	g_root.mode = 0755;
	g_root.uid = g_uid;
	g_root.gid = g_gid;
	g_root.atime = g_root.mtime = g_root.ctime = ts_now();
	g_root.last_use = time(NULL);

	return fuse_main(fargc, fargv, &nf_oper, NULL);
}

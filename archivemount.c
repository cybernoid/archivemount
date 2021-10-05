/*

   Copyright (c) 2005-2018 Andre Landwehr <andrel@cybernoia.de>

   This program can be distributed under the terms of the GNU LGPL.
   See the file COPYING.

   Based on: fusexmp.c and sshfs.c by Miklos Szeredi <miklos@szeredi.hu>

   Contributions by: Niels de Vos <niels@nixpanic.net>
                     Thomas J. Duck
                     Andrew Brampton <me at bramp dot net>
                     Tomáš Čech <sleep_walker at suse dot cz>
                     Timothy Hobbs <timothyhobbs at seznam dot cz>
                     Lee Leahu
                     Alain Parmentier <pa at infodata.lu>
*/

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 500
#endif

#define MAXBUF 4096

#define BLOCK_SIZE 10240

#include "config.h"

#ifdef HAVE_FUSE2
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#elif defined(HAVE_FUSE3)
#define FUSE_USE_VERSION 30
#include <fuse.h>
#else
#error No fuse version defined
#endif
#include <fuse_opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <grp.h>
#include <pwd.h>
#include <utime.h>
#include <string.h>
#include <wchar.h>
#include <archive.h>
#include <archive_entry.h>
#include <pthread.h>
#include <regex.h>
#include <termios.h>

#include "archivecomp.h"

#include "uthash.h"

  /**********/
 /* macros */
/**********/
#ifdef NDEBUG
#   define log(format, ...)
#else
#   define log(format, ...) \
{ \
	FILE *FH = fopen("/tmp/archivemount.log", "a"); \
	if (FH) { \
		fprintf(FH, "l. %4d: " format "\n", __LINE__, ##__VA_ARGS__); \
		fclose(FH); \
	} \
}
#endif


  /*******************/
 /* data structures */
/*******************/

typedef struct node {
	struct node *parent;
	struct node *child; /* first child for directories */
	char *name; /* fully qualified with prepended '/' */
	char *basename; /* every after the last '/' */
	char *location; /* location on disk for new/modified files, else NULL */
	int namechanged; /* true when file was renamed */
	struct archive_entry *entry; /* libarchive header data */
	int modified; /* true when node was modified */
	UT_hash_handle hh;
} NODE;

struct options {
	int readonly;
	int password;
	int nobackup;
	int nosave;
	char *subtree_filter;
	int formatraw;
};

typedef struct formatraw_cache {
	struct stat st;
	struct archive *archive;
	int opened;
	off_t offset_uncompressed;
} FORMATRAW_CACHE;

enum {
	KEY_VERSION,
	KEY_HELP,
};

#define AR_OPT(t, p, v) { t, offsetof(struct options, p), v }

static struct fuse_opt ar_opts[] =
{
	AR_OPT("readonly", readonly, 1),
	AR_OPT("password", password, 1),
	AR_OPT("nobackup", nobackup, 1),
	AR_OPT("nosave"  , nosave  , 1),
	AR_OPT("subtree=%s", subtree_filter, 1),
	AR_OPT("formatraw", formatraw, 1),

	FUSE_OPT_KEY("-V",	       KEY_VERSION),
	FUSE_OPT_KEY("--version",      KEY_VERSION),
	FUSE_OPT_KEY("-h",	       KEY_HELP),
	FUSE_OPT_KEY("--help",	       KEY_HELP),
	FUSE_OPT_END
};


  /***********/
 /* globals */
/***********/

static int archiveFd; /* file descriptor of archive file, just to keep the
			 beast alive in case somebody deletes the file while
			 it is mounted */
static int archiveModified = 0;
static int archiveWriteable = 0;
static NODE *root;
static FORMATRAW_CACHE *rawcache;
struct options options;
char *mtpt = NULL;
char *archiveFile = NULL;
char *user_passphrase = NULL;
size_t user_passphrase_size = 0;
pthread_mutex_t lock; /* global node tree lock */

/* Taken from the GNU under the GPL */
char *
strchrnul (const char *s, int c_in)
{
	char c = c_in;
	while (*s && (*s != c))
		s++;

	return (char *) s;
}

  /**********************/
 /* internal functions */
/**********************/

static void
usage(const char *progname)
{
	fprintf(stderr,
		"usage: %s archivepath mountpoint [options]\n"
		"\n"
		"general options:\n"
		"    -o opt,[opt...]	    mount options\n"
		"    -h   --help	    print help\n"
		"    -V   --version	    print version\n"
		"\n"
		"archivemount options:\n"
		"    -o readonly	    disable write support\n"
		"    -o password	    prompt for a password.\n"
		"    -o nobackup	    remove archive file backups\n"
		"    -o nosave		    do not save changes upon unmount.\n"
		"			    Good if you want to change something\n"
		"			    and save it as a diff,\n"
		"			    or use a format for saving which is\n"
		"			    not supported by archivemount.\n"
		"\n"
		"    -o subtree=<regexp>    use only subtree matching ^\\.\\?<regexp> from archive\n"
		"			    it implies readonly\n"
		"\n"
		"    -o formatraw	    treat input as a single element archive\n"
		"			    it implies readonly\n"
		"\n",progname);
}

static struct fuse_operations ar_oper;

static int
ar_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	(void) data;

	switch(key) {
		case FUSE_OPT_KEY_OPT:
			return 1;

		case FUSE_OPT_KEY_NONOPT:
			if (!archiveFile) {
				archiveFile = strdup(arg);
				return 0;
			} else if (!mtpt) {
				mtpt = strdup(arg);
			}
			return 1;

		case KEY_HELP:
			usage(outargs->argv[0]);
			fuse_opt_add_arg(outargs, "-ho");
			fuse_main(outargs->argc, outargs->argv, &ar_oper, NULL);
			exit(1);

		case KEY_VERSION:
			fprintf(stderr, "archivemount version %s\n", VERSION);
			fuse_opt_add_arg(outargs, "--version");
			fuse_main(outargs->argc, outargs->argv, &ar_oper, NULL);
			exit(0);

		default:
			fprintf(stderr, "internal error\n");
			abort();
	}
}

static NODE *
init_node()
{
	NODE *node;

	if ((node = malloc(sizeof(NODE))) == NULL) {
		log("Out of memory");
		return NULL;
	}

	node->parent = NULL;
	node->child = NULL;
	node->name = NULL;
	node->basename = NULL;
	node->location = NULL;
	node->namechanged = 0;
	node->entry = archive_entry_new();
	node->modified = 0;
	memset(&node->hh, 0, sizeof(node->hh));

	if (node->entry == NULL) {
		log("Out of memory");
		free(node);
		return NULL;
	}

	return node;
}

static FORMATRAW_CACHE *
init_rawcache()
{
	FORMATRAW_CACHE *rawcache;

	if ((rawcache = malloc(sizeof(FORMATRAW_CACHE))) == NULL) {
		log("Out of memory");
		return NULL;
	}

	memset(&rawcache->st, 0, sizeof(rawcache->st));
	memset(&rawcache->archive, 0, sizeof(rawcache->archive));
	rawcache->opened=0;
	rawcache->offset_uncompressed=0;
	return rawcache;
}

static void
free_node(NODE *node)
{
	NODE *child, *tmp;

	free(node->name);
	archive_entry_free(node->entry);

	// Clean up any children
	HASH_ITER(hh, node->child, child, tmp) {
		HASH_DEL(node->child, child);
		free_node(child);
	}

	free(node);
}

/* not used now
#static void
free_rawcache(FORMATRAW_CACHE *rawcache)
{
	free(rawcache);
}
*/


static void
remove_child(NODE *node)
{
	if (node->parent) {
		HASH_DEL(node->parent->child, node);
		log("removed '%s' from parent '%s' (was first child)", node->name, node->parent->name);
	} else {
		root = NULL;
	}
}

static void
insert_as_child(NODE *node, NODE *parent)
{
	node->parent = parent;
	HASH_ADD_KEYPTR(hh, parent->child, node->basename, strlen(node->basename), node);
	log("inserted '%s' as child of '%s'", node->name, parent->name);
}

/*
 * inserts "node" into tree starting at "root" according to the path
 * specified in node->name
 * @return 0 on success, 0-errno else (ENOENT or ENOTDIR)
 */
static int
insert_by_path(NODE *root, NODE *node)
{
	char *temp;
	NODE *cur = root;
	char *key = node->name;

	key++;
	while ((temp = strchr(key, '/'))) {
		size_t namlen = temp - key;
		char nam[namlen + 1];
		NODE *last = cur;

		strncpy(nam, key, namlen);
		nam[namlen] = '\0';
		if (cur != root || strcmp(cur->basename, nam) != 0) {
			cur = cur->child;
			while (cur && strcmp(cur->basename, nam) != 0)
			{
				cur = cur->hh.next;
			}
		}
		if (! cur) {
			/* parent path not found, create a temporary one */
			NODE *tempnode;
			if ((tempnode = init_node()) == NULL)
				return -ENOMEM;

			if ((tempnode->name = malloc(
						strlen(last->name) + namlen + 2)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
			if (last != root) {
				sprintf(tempnode->name, "%s/%s", last->name, nam);
			} else {
				sprintf(tempnode->name, "/%s", nam);
			}
			tempnode->basename = strrchr(tempnode->name, '/') + 1;

			archive_entry_free(tempnode->entry);

			if ((tempnode->entry = archive_entry_clone(root->entry)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
			/* insert it recursively */
			insert_by_path(root, tempnode);
			/* now inserting node should work, correct cur for it */
			cur = tempnode;
		}
		/* iterate */
		key = temp + 1;
	}
	if (S_ISDIR(archive_entry_mode(cur->entry))) {
		/* check if a child of this name already exists */
		NODE *tempnode = NULL;

		HASH_FIND(hh, cur->child, node->basename, strlen(node->basename), tempnode);

		if (tempnode) {
			/* this is a dupe due to a temporarily inserted
			   node, just update the entry */
			archive_entry_free(node->entry);
			if ((node->entry = archive_entry_clone(
						tempnode->entry)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
		} else {
			insert_as_child(node, cur);
		}
	} else {
		return -ENOTDIR;
	}
	return 0;
}

static int
build_tree(const char *mtpt)
{
	struct archive *archive;
	struct stat st;
	int format;
	int compression;
	NODE *cur;
	char *subtree_filter = NULL;
	regex_t subtree;
	int regex_error;
	regmatch_t regmatch;
	char error_buffer[256];

#define PREFIX		"^\\.\\?"

	if (options.subtree_filter) {
		subtree_filter = malloc(strlen(options.subtree_filter) + strlen(PREFIX) + 1);
		if (!subtree_filter) {
			log("Not enough memory");
			return -ENOMEM;
		}
		strcpy(subtree_filter, PREFIX);
		subtree_filter = strcat(subtree_filter, options.subtree_filter);
		/* \? is only a special char on Mac if REG_ENHANCED is specified  */
#if defined REG_ENHANCED
		regex_error = regcomp(&subtree, subtree_filter, REG_ENHANCED);
#else
		regex_error = regcomp(&subtree, subtree_filter, 0);
#endif
		if (regex_error) {
			regerror(regex_error, &subtree, error_buffer, 256);
			log("Regex build error: %s\n", error_buffer);
			return -regex_error;
		}
		options.readonly = 1;
	}
	/* open archive */
	if ((archive = archive_read_new()) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}
	if (archive_read_support_filter_all(archive) != ARCHIVE_OK) {
		fprintf(stderr, "%s\n", archive_error_string(archive));
		return archive_errno(archive);
	}
	if (options.formatraw) {
		if (archive_read_support_format_raw(archive) != ARCHIVE_OK) {
			fprintf(stderr, "%s\n", archive_error_string(archive));
			return archive_errno(archive);
		}
		options.readonly = 1;
	} else {
		if (archive_read_support_format_all(archive) != ARCHIVE_OK) {
			fprintf(stderr, "%s\n", archive_error_string(archive));
			return archive_errno(archive);
		}
	}
	if (options.password) {
		if (archive_read_add_passphrase(archive, user_passphrase) != ARCHIVE_OK) {
			fprintf(stderr, "%s\n", archive_error_string(archive));
			return archive_errno(archive);
		}
	}
	if (archive_read_open_fd(archive, archiveFd, BLOCK_SIZE) != ARCHIVE_OK) {
		fprintf(stderr, "%s\n", archive_error_string(archive));
		return archive_errno(archive);
	}
	/* check if format or compression prohibits writability */
	format = archive_format(archive);
        log("mounted archive format is %s (0x%x)",
            archive_format_name(archive), format);
	compression = archive_filter_code(archive, 0);
        log("mounted archive compression is %s (0x%x)",
            archive_filter_name(archive, 0), compression);
	if (format & ARCHIVE_FORMAT_ISO9660
		|| format & ARCHIVE_FORMAT_ISO9660_ROCKRIDGE
		|| format & ARCHIVE_FORMAT_ZIP
		|| compression == ARCHIVE_COMPRESSION_COMPRESS)
	{
		archiveWriteable = 0;
	}
	/* create root node */
	if ((root = init_node()) == NULL)
		return -ENOMEM;

	root->name = strdup("/");
	root->basename = &root->name[1];

	/* fill root->entry */
	if (fstat(archiveFd, &st) != 0) {
		perror("Error stat'ing archiveFile");
		return errno;
	}
	archive_entry_set_gid(root->entry, getgid());
	archive_entry_set_uid(root->entry, getuid());
	archive_entry_set_mode(root->entry, st.st_mtime);
	archive_entry_set_pathname(root->entry, "/");
	archive_entry_set_size(root->entry, st.st_size);
	stat(mtpt, &st);
	archive_entry_set_mode(root->entry, st.st_mode);

	if ((cur = init_node()) == NULL) {
		return -ENOMEM;
	}

	/* read all entries in archive, create node for each */
	while (archive_read_next_header2(archive, cur->entry) == ARCHIVE_OK) {
		const char *name;
		/* find name of node */
		name = archive_entry_pathname(cur->entry);
		if (memcmp(name, "./\0", 3) == 0) {
			/* special case: the directory "./" must be skipped! */
			continue;
		}
		if (options.subtree_filter) {
			regex_error = regexec(&subtree, name, 1, &regmatch, REG_NOTEOL);
			if (regex_error) {
				if (regex_error == REG_NOMATCH)
					continue;
				regerror(regex_error, &subtree, error_buffer, 256);
				log("Regex match error: %s\n", error_buffer);
				return -regex_error;
			}
			/* strip subtree from name */
			name += regmatch.rm_eo;
		}
		/* create node and clone the entry */
		/* normalize the name to start with "/" */
		if (strncmp(name, "./", 2) == 0) {
			/* remove the "." of "./" */
			cur->name = strdup(name + 1);
		} else if (name[0] != '/') {
			/* prepend a '/' to name */
			if ((cur->name = malloc(strlen(name) + 2)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
			sprintf(cur->name, "/%s", name);
		} else {
			/* just set the name */
			cur->name = strdup(name);
		}
		int len = strlen(cur->name) - 1;
		if (0 < len) {
			/* remove trailing '/' for directories */
			if (cur->name[len] == '/') {
				cur->name[len] = '\0';
			}
			cur->basename = strrchr(cur->name, '/') + 1;

			/* references */
			if (insert_by_path(root, cur) != 0) {
				log("ERROR: could not insert %s into tree",
					cur->name);
				return -ENOENT;
			}
		} else {
			/* this is the directory the subtree filter matches,
			   do not respect it */
		}

		if ((cur = init_node()) == NULL) {
			return -ENOMEM;
		}

		archive_read_data_skip(archive);
	}
	/* free the last unused NODE */
	free_node(cur);

	/* close archive */
	archive_read_free(archive);
	lseek(archiveFd, 0, SEEK_SET);
	if (options.subtree_filter) {
		regfree(&subtree);
		free(subtree_filter);
	}
	return 0;
}

static NODE *
find_modified_node(NODE *start)
{
	NODE *ret = NULL;
	NODE *run = start;

	while (run) {
		if (run->modified) {
			ret = run;
			break;
		}
		if (run->child) {
			if ((ret = find_modified_node(run->child))) {
				break;
			}
		}
		run = run->hh.next;
	}
	return ret;
}

static void
correct_hardlinks_to_node(const NODE *start, const char *old_name,
	const char *new_name)
{
	const NODE *run = start;

	while (run) {
		const char *tmp;
		if ((tmp = archive_entry_hardlink(run->entry))) {
			if (strcmp(tmp, old_name) == 0) {
				/* the node in "run" is a hardlink to "node",
				 * correct the path */
				//log("correcting hardlink '%s' from '%s' to '%s'", run->name, old_name, new_name);
				archive_entry_set_hardlink(
					run->entry, new_name);
			}
		}
		if (run->child) {
			correct_hardlinks_to_node(run->child, old_name, new_name);
		}
		run = run->hh.next;
	}
}

void
correct_name_in_entry (NODE *node)
{
	if (root->child &&
		node->name[0] == '/' &&
		archive_entry_pathname(root->child->entry)[0] != '/')
	{
		log ("correcting name in entry to '%s'", node->name+1);
		archive_entry_set_pathname(node->entry, node->name + 1);
	} else {
		log ("correcting name in entry to '%s'", node->name);
		archive_entry_set_pathname(node->entry, node->name);
	}
}

static NODE *
get_node_for_path(NODE *start, const char *path)
{
	NODE *ret = NULL;

	//log("get_node_for_path path: '%s' start: '%s'", path, start->name);

	/* Check if start is a perfect match */
	if (strcmp(path, start->name + (*path=='/'?0:1)) == 0) {
		//log("  get_node_for_path path: '%s' start: '%s' return: '%s'", path, start->name, start->name);
		return start;
	}

	/* Check if one of the children match */
	if (start->child) {
		const char * basename;
		const char * baseend;

		/* Find the part of the path we are now looking for */
		basename = path + strlen(start->name) - (*path=='/'?0:1);
		if (*basename == '/')
			basename++;

		baseend = strchrnul(basename, '/');

		//log("get_node_for_path path: '%s' start: '%s' basename: '%s' len: %ld", path, start->name, basename, baseend - basename);

		HASH_FIND(hh, start->child, basename, baseend - basename, ret);

		if (ret) {
			ret = get_node_for_path(ret, path);
		}
	}

	//log("  get_node_for_path path: '%s' start: '%s' return: '%s'", path, start->name, ret == NULL ? "(null)" : ret->name);
	return ret;
}


static NODE *
get_node_for_entry(NODE *start, struct archive_entry *entry)
{
	NODE *ret = NULL;
	NODE *run = start;
	const char *path = archive_entry_pathname(entry);

	if (*path == '/') {
		path++;
	}
	while (run) {
		const char *name = archive_entry_pathname(run->entry);
		if (*name == '/') {
			name++;
		}
		if (strcmp(path, name) == 0) {
			ret = run;
			break;
		}
		if (run->child) {
			if ((ret = get_node_for_entry(run->child, entry))) {
				break;
			}
		}
		run = run->hh.next;
	}
	return ret;
}

static int
rename_recursively(NODE *start, const char *from, const char *to)
{
	char *individualName;
	char *newName;
	int ret = 0;
	NODE *node = start;
	/* removing and re-inserting nodes while iterating through
	   the hashtable is a bad idea, so we copy all node ptrs
	   into an array first and iterate over that instead */
	size_t count = HASH_COUNT(start);
	NODE *nodes[count];
	log ("%s has %zu items", start->parent->name, count);
	NODE **dst = &nodes[0];
	while (node) {
		*dst = node;
		++dst;
		node = node->hh.next;
	}

	size_t i;
	for (i=0; i<count; ++i) {
		node = nodes[i];
		if (node->child) {
			/* recurse */
			ret = rename_recursively(node->child, from, to);
		}
		remove_child(node);
		/* change node name */
		individualName = node->name + strlen(from);
		if (*to != '/') {
			if ((newName = (char *)malloc(strlen(to) +
						strlen(individualName) + 2)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
			sprintf(newName, "/%s%s", to, individualName);
		} else {
			if ((newName = (char *)malloc(strlen(to) +
						strlen(individualName) + 1)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
			sprintf(newName, "%s%s", to, individualName);
		}
		log ("new name: '%s'", newName);
		correct_hardlinks_to_node(root, node->name, newName);
		free(node->name);
		node->name = newName;
		node->basename = strrchr(node->name, '/') + 1;
		node->namechanged = 1;
		insert_by_path(root, node);
	}
	return ret;
}

static int
get_temp_file_name(const char *path, char **location)
{
	int fh;

	/* create name for temp file */
	if ((*location = (char *)malloc(
				strlen(P_tmpdir) +
				strlen("/archivemount_XXXXXX") +
				1)) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}
	sprintf(*location, "%s/archivemount_XXXXXX", P_tmpdir);
	if ((fh = mkstemp(*location))  == -1) {
		log("Could not create temp file name %s: %s",
			*location, strerror(errno));
		free(*location);
		return 0 - errno;
	}
	close(fh);
	unlink(*location);
	return 0;
}

/**
 * Updates given nodes node->entry by stat'ing node->location. Does not update
 * the name!
 */
static int
update_entry_stat(NODE *node)
{
	struct stat st;
	struct passwd *pwd;
	struct group *grp;

	if (lstat(node->location, &st) != 0) {
		return 0 - errno;
	}
	archive_entry_set_gid(node->entry, st.st_gid);
	archive_entry_set_uid(node->entry, st.st_uid);
	archive_entry_set_mtime(node->entry, st.st_mtime, 0);
	archive_entry_set_size(node->entry, st.st_size);
	archive_entry_set_mode(node->entry, st.st_mode);
	archive_entry_set_rdevmajor(node->entry, st.st_dev);
	archive_entry_set_rdevminor(node->entry, st.st_dev);
	pwd = getpwuid(st.st_uid);
	if (pwd) {
		archive_entry_set_uname(node->entry, strdup(pwd->pw_name));
	}
	grp = getgrgid(st.st_gid);
	if (grp) {
		archive_entry_set_gname(node->entry, strdup(grp->gr_name));
	}
	return 0;
}

/*
 * write a new or modified file to the new archive; used from save()
 */
static void
write_new_modded_file(NODE *node, struct archive_entry *wentry,
	struct archive *newarc)
{
	if (node->location) {
		struct stat st;
		int fh = 0;
		off_t offset = 0;
		void *buf;
		ssize_t len = 0;
		/* copy stat info */
		if (lstat(node->location, &st) != 0) {
			log("Could not lstat temporary file %s: %s",
				node->location,
				strerror(errno));
			return;
		}
		archive_entry_copy_stat(wentry, &st);
		/* open temporary file */
		fh = open(node->location, O_RDONLY);
		if (fh == -1) {
			log("Fatal error opening modified file %s at "
				"location %s, giving up",
				node->name, node->location);
			return;
		}
		/* write header */
		archive_write_header(newarc, wentry);
		if (S_ISREG(st.st_mode)) {
			/* regular file, copy data */
			if ((buf = malloc(MAXBUF)) == NULL) {
				log("Out of memory");
				return;
			}
			while ((len = pread(fh, buf, (size_t)MAXBUF,
						offset)) > 0)
			{
				archive_write_data(newarc, buf, len);
				offset += len;
			}
			free(buf);
		}
		if (len == -1) {
			log("Error reading temporary file %s for file %s: %s",
				node->location,
				node->name,
				strerror(errno));
			close(fh);
			return;
		}
		/* clean up */
		close(fh);
		if (S_ISDIR(st.st_mode)) {
			if (rmdir(node->location) == -1) {
				log("WARNING: rmdir '%s' failed: %s",
					node->location, strerror(errno));
			}
		} else {
			if (unlink(node->location) == -1) {
				log("WARNING: unlinking '%s' failed: %s",
					node->location, strerror(errno));
			}
		}
	} else {
		/* no data, only write header (e.g. when node is a link!) */
		//log("writing header for file %s", archive_entry_pathname(wentry));
		archive_write_header(newarc, wentry);
	}
	/* mark file as written */
	node->modified = 0;
}

static int
save(const char *archiveFile)
{
	struct archive *oldarc;
	struct archive *newarc;
	struct archive_entry *entry;
	int tempfile;
	int format;
	int compression;
	char *oldfilename;
	NODE *node;

	oldfilename = malloc(strlen(archiveFile) + 5 + 1);
	if (!oldfilename) {
		log("Could not allocate memory for oldfilename");
		return 0 - ENOMEM;
	}
	/* unfortunately libarchive does not support modification of
	 * compressed archives, so a new archive has to be written */
	/* rename old archive */
	sprintf(oldfilename, "%s.orig", archiveFile);
	close(archiveFd);
	if (rename(archiveFile, oldfilename) < 0) {
		int err = errno;
		char *buf = NULL;
		char *unknown = "<unknown>";
		if (getcwd(buf, 0) == NULL) {
			/* getcwd failed, set buf to sth. reasonable */
			buf = unknown;
		}
		log("Could not rename old archive file (%s/%s): %s",
			buf, archiveFile, strerror(err));
		free(buf);
		archiveFd = open(archiveFile, O_RDONLY);
		return 0 - err;
	}
	archiveFd = open(oldfilename, O_RDONLY);
	free(oldfilename);
	/* open old archive */
	if ((oldarc = archive_read_new()) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}
	if (archive_read_support_filter_all(oldarc) != ARCHIVE_OK) {
		log("%s", archive_error_string(oldarc));
		return archive_errno(oldarc);
	}
	if (archive_read_support_format_all(oldarc) != ARCHIVE_OK) {
		log("%s", archive_error_string(oldarc));
		return archive_errno(oldarc);
	}
	if (options.password) {
		if (archive_read_add_passphrase(oldarc, user_passphrase) != ARCHIVE_OK) {
			fprintf(stderr, "%s\n", archive_error_string(oldarc));
			return archive_errno(oldarc);
		}
	}
	if (archive_read_open_fd(oldarc, archiveFd, BLOCK_SIZE) != ARCHIVE_OK) {
		log("%s", archive_error_string(oldarc));
		return archive_errno(oldarc);
	}
        /* Read first header of oldarc so that archive format is set. */
        if (archive_read_next_header(oldarc, &entry) != ARCHIVE_OK) {
		log("%s", archive_error_string(oldarc));
		return archive_errno(oldarc);
        }
	format = archive_format(oldarc);
	compression = archive_filter_code(oldarc, 0);
        log("mounted archive format is %s (0x%x)",
            archive_format_name(oldarc), format);
        log("mounted archive compression is %s (0x%x)",
            archive_filter_name(oldarc, 0), compression);
	/* open new archive */
	if ((newarc = archive_write_new()) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}
	switch(compression) {
		case ARCHIVE_COMPRESSION_GZIP:
			archive_write_add_filter_gzip(newarc);
			break;
		case ARCHIVE_COMPRESSION_BZIP2:
			archive_write_add_filter_bzip2(newarc);
			break;
		case ARCHIVE_COMPRESSION_COMPRESS:
		case ARCHIVE_COMPRESSION_NONE:
		default:
			archive_write_add_filter_none(newarc);
			break;
	}
	if (archive_write_set_format(newarc, format) != ARCHIVE_OK) {
		log("writing archives of format %d (%s) is not "
			"supported", format,
			archive_format_name(oldarc));
		return -ENOTSUP;
	}
	tempfile = open(archiveFile,
		O_WRONLY | O_CREAT | O_EXCL,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (tempfile == -1) {
		log("could not open new archive file for writing");
		return 0 - errno;
	}
	if (options.password) {
		/* When libarchive gains support for multiple kinds of encryption and
		 * an API to say which kind is in use, this should use copy oldarc's
		 * encryption settings.  For now, just set the one kind of encryption
		 * that libarchive supports. */
		if (archive_write_set_options(newarc, "zip:encryption=aes256") != ARCHIVE_OK) {
			log("Could not set encryption for new archive: %s", archive_error_string(newarc));
			return archive_errno(newarc);
		}
		if (archive_write_set_passphrase(newarc, user_passphrase) != ARCHIVE_OK) {
			log("Could not set passphrase for new archive: %s", archive_error_string(newarc));
			return archive_errno(newarc);
		}
	}
	if (archive_write_open_fd(newarc, tempfile) != ARCHIVE_OK) {
		log("%s", archive_error_string(newarc));
		return archive_errno(newarc);
	}
	do {
		off_t offset;
		const void *buf;
		struct archive_entry *wentry;
		size_t len;
		const char *name;
		/* find corresponding node */
		name = archive_entry_pathname(entry);
		node = get_node_for_entry(root, entry);
		if (! node) {
			log("WARNING: no such node for '%s'", name);
			archive_read_data_skip(oldarc);
			continue;
		}
		/* create new entry, copy metadata */
		if ((wentry = archive_entry_new()) == NULL) {
			log("Out of memory");
			return -ENOMEM;
		}
		if (archive_entry_gname_w(node->entry)) {
			archive_entry_copy_gname_w(wentry,
				archive_entry_gname_w(node->entry));
		}
		if (archive_entry_hardlink(node->entry)) {
			archive_entry_copy_hardlink(wentry,
				archive_entry_hardlink(node->entry));
		}
		if (archive_entry_hardlink_w(node->entry)) {
			archive_entry_copy_hardlink_w(wentry,
				archive_entry_hardlink_w(
					node->entry));
		}
		archive_entry_copy_stat(wentry,
			archive_entry_stat(node->entry));
		if (archive_entry_symlink_w(node->entry)) {
			archive_entry_copy_symlink_w(wentry,
				archive_entry_symlink_w(
					node->entry));
		}
		if (archive_entry_uname_w(node->entry)) {
			archive_entry_copy_uname_w(wentry,
				archive_entry_uname_w(node->entry));
		}
		/* set correct name */
		if (node->namechanged) {
			if (*name == '/') {
				archive_entry_set_pathname(
						wentry, node->name);
			} else {
				archive_entry_set_pathname(
						wentry, node->name + 1);
			}
		} else {
			archive_entry_set_pathname(wentry, name);
		}
		/* write header and copy data */
		if (node->modified) {
			/* file was modified */
			write_new_modded_file(node, wentry, newarc);
		} else {
			/* file was not modified */
			archive_entry_copy_stat(wentry,
				archive_entry_stat(node->entry));
			archive_write_header(newarc, wentry);
			while (archive_read_data_block(oldarc, &buf,
					&len, &offset) == ARCHIVE_OK)
			{
				archive_write_data(newarc, buf, len);
			}
		}
		/* clean up */
		archive_entry_free(wentry);
	} while (archive_read_next_header(oldarc, &entry) == ARCHIVE_OK);
	/* find new files to add (those do still have modified flag set */
	while ((node = find_modified_node(root))) {
		if (node->namechanged) {
			correct_name_in_entry (node);
		}
		write_new_modded_file(node, node->entry, newarc);
	}
	/* clean up, re-open the new archive for reading */
	archive_read_free(oldarc);
	archive_write_free(newarc);
	close(tempfile);
	close(archiveFd);
	archiveFd = open(archiveFile, O_RDONLY);
	if (options.nobackup) {
		if (remove(oldfilename) < 0) {
			log("Could not remove .orig archive file (%s): %s",
				oldfilename, strerror(errno));
			return 0 - errno;
		}
	}
	return 0;
}


  /*****************/
 /* API functions */
/*****************/

static int
_ar_open_raw(void)
	//_ar_open_raw(const char *path, struct fuse_file_info *fi)
{
	// open archive and search first entry

	const char path[] = "/data";

	int ret = -1;
	const char *realpath;
	NODE *node;
	log("_ar_open_raw called, path: '%s'", path);


	if (rawcache->opened!=0) {
		log("already opened");
		return 0;
	}

	options.readonly = 1;

	/* find node */

	node = get_node_for_path(root, path);
	if (! node) {
		log("get_node_for_path error");
		return -ENOENT;
	}

	//	struct archive *archive;
	struct archive_entry *entry;
	int archive_ret;
	/* search file in archive */
	realpath = archive_entry_pathname(node->entry);
	if ((rawcache->archive = archive_read_new()) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}
	archive_ret = archive_read_support_filter_all(rawcache->archive);
	if (archive_ret != ARCHIVE_OK) {
		log("archive_read_support_filter_all(): %s (%d)\n",
			archive_error_string(rawcache->archive), archive_ret);
		return -EIO;
	}

	archive_ret = archive_read_support_format_raw(rawcache->archive);
	if (archive_ret != ARCHIVE_OK) {
		log("archive_read_support_format_raw(): %s (%d)\n",
			archive_error_string(rawcache->archive), archive_ret);
		return -EIO;
	}
	if (options.password) {
		if (archive_read_add_passphrase(rawcache->archive, user_passphrase) != ARCHIVE_OK) {
			fprintf(stderr, "%s\n", archive_error_string(rawcache->archive));
			return archive_errno(rawcache->archive);
		}
	}

	archive_ret = archive_read_open_fd(rawcache->archive, archiveFd, BLOCK_SIZE);
	if (archive_ret != ARCHIVE_OK) {
		log("archive_read_open_fd(): %s (%d)\n",
			archive_error_string(rawcache->archive), archive_ret);
		return -EIO;
	}
	/* search for file to read - "/data" must be the first entry */
	while ((archive_ret = archive_read_next_header(
				rawcache->archive, &entry)) == ARCHIVE_OK) {
		const char *name;
		name = archive_entry_pathname(entry);
		if (strcmp(realpath, name) == 0) {
			break;
		}

	}
	rawcache->opened=1;
	rawcache->offset_uncompressed=0;

	return ret;
}

static int
_ar_read_raw(const char *path, char *buf, size_t size, off_t offset,
	struct fuse_file_info *fi)
{
	int ret = -1;
	NODE *node;
	(void)fi;

	log("_ar_read_raw called, path: '%s'", path);
	/* find node */
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}

	if (offset < rawcache->offset_uncompressed) {
		//rewind archive

		/* close archive */
		archive_read_free(rawcache->archive);
		lseek(archiveFd, 0, SEEK_SET);

		rawcache->opened=0;

		/* reopen */
		_ar_open_raw();
	}

	void *trash;
	if ((trash = malloc(MAXBUF)) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}
	/* skip offset */
	offset-=rawcache->offset_uncompressed;

	while (offset > 0) {
		int skip = offset > MAXBUF ? MAXBUF : offset;
		ret = archive_read_data(
			rawcache->archive, trash, skip);
		if (ret == ARCHIVE_FATAL
			|| ret == ARCHIVE_WARN
			|| ret == ARCHIVE_RETRY)
		{
			log("ar_read_raw (skipping offset): %s",
				archive_error_string(rawcache->archive));
			errno = archive_errno(rawcache->archive);
			ret = -1;
			break;
		}
		rawcache->offset_uncompressed+=skip;
		offset -= skip;
	}
	free(trash);

	if (offset) {
		/* there was an error */
		log("ar_read_raw (offset!=0)");
		return -EIO;
	}
	/* read data */
	ret = archive_read_data(rawcache->archive, buf, size);
	if (ret == ARCHIVE_FATAL
		|| ret == ARCHIVE_WARN
		|| ret == ARCHIVE_RETRY)
	{
		log("ar_read_raw (reading data): %s",
			archive_error_string(rawcache->archive));
		errno = archive_errno(rawcache->archive);
		ret = -1;
	}
	rawcache->offset_uncompressed +=size;
	return ret;
}

static int
_ar_read(const char *path, char *buf, size_t size, off_t offset,
	struct fuse_file_info *fi)
{
	int ret = -1;
	const char *realpath;
	NODE *node;
	(void)fi;
	log("_ar_read called, path: '%s'", path);
	/* find node */
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_read(archive_entry_hardlink(node->entry),
			buf, size, offset, fi);
	}
	if (archive_entry_symlink(node->entry)) {
		/* file is a symlink, recurse into it */
		return _ar_read(archive_entry_symlink(node->entry),
			buf, size, offset, fi);
	}
	if (node->modified) {
		/* the file is new or modified, read temporary file instead */
		int fh;
		fh = open(node->location, O_RDONLY);
		if (fh == -1) {
			log("Fatal error opening modified file '%s' at "
				"location '%s', giving up",
				path, node->location);
			return 0 - errno;
		}
		/* copy data */
		if ((ret = pread(fh, buf, size, offset)) == -1) {
			log("Error reading temporary file '%s': %s",
				node->location, strerror(errno));
			close(fh);
			ret = 0 - errno;
		}
		/* clean up */
		close(fh);
	} else {
		struct archive *archive;
		struct archive_entry *entry;
		int archive_ret;
		/* search file in archive */
		realpath = archive_entry_pathname(node->entry);
		if ((archive = archive_read_new()) == NULL) {
			log("Out of memory");
			return -ENOMEM;
		}
		archive_ret = archive_read_support_filter_all(archive);
		if (archive_ret != ARCHIVE_OK) {
			log("archive_read_support_filter_all(): %s (%d)\n",
				archive_error_string(archive), archive_ret);
			return -EIO;
		}
		if (options.formatraw) {
			archive_ret = archive_read_support_format_raw(archive);
			if (archive_ret != ARCHIVE_OK) {
				log("archive_read_support_format_all(): %s (%d)\n",
					archive_error_string(archive), archive_ret);
				return -EIO;
			}
			options.readonly = 1;
		} else {
			archive_ret = archive_read_support_format_all(archive);
			if (archive_ret != ARCHIVE_OK) {
				log("archive_read_support_format_all(): %s (%d)\n",
					archive_error_string(archive), archive_ret);
				return -EIO;
			}
		}
		if (options.password) {
			if (archive_read_add_passphrase(archive, user_passphrase) != ARCHIVE_OK) {
				fprintf(stderr, "%s\n", archive_error_string(archive));
				return archive_errno(archive);
			}
		}
		archive_ret = archive_read_open_fd(archive, archiveFd, BLOCK_SIZE);
		if (archive_ret != ARCHIVE_OK) {
			log("archive_read_open_fd(): %s (%d)\n",
				archive_error_string(archive), archive_ret);
			return -EIO;
		}
		/* search for file to read */
		while ((archive_ret = archive_read_next_header(
					archive, &entry)) == ARCHIVE_OK) {
			const char *name;
			name = archive_entry_pathname(entry);
			if (strcmp(realpath, name) == 0) {
				void *trash;
				if ((trash = malloc(MAXBUF)) == NULL) {
					log("Out of memory");
					return -ENOMEM;
				}
				/* skip offset */
				while (offset > 0) {
					int skip = offset > MAXBUF ? MAXBUF : offset;
					ret = archive_read_data(
						archive, trash, skip);
					if (ret == ARCHIVE_FATAL
						|| ret == ARCHIVE_WARN
						|| ret == ARCHIVE_RETRY)
					{
						log("ar_read (skipping offset): %s",
							archive_error_string(archive));
						errno = archive_errno(archive);
						ret = -1;
						break;
					}
					offset -= skip;
				}
				free(trash);
				if (offset) {
					/* there was an error */
					break;
				}
				/* read data */
				ret = archive_read_data(archive, buf, size);
				if (ret == ARCHIVE_FATAL
					|| ret == ARCHIVE_WARN
					|| ret == ARCHIVE_RETRY)
				{
					log("ar_read (reading data): %s",
						archive_error_string(archive));
					errno = archive_errno(archive);
					ret = -1;
				}
				break;
			}
			archive_read_data_skip(archive);
		}
		/* close archive */
		archive_read_free(archive);
		lseek(archiveFd, 0, SEEK_SET);
	}
	return ret;
}

static int
ar_read(const char *path, char *buf, size_t size, off_t offset,
	struct fuse_file_info *fi)
{
	log("ar_read called, path: '%s'", path);
	int ret = pthread_mutex_lock(&lock);
	if (ret) {
		log("failed to get lock: %s\n", strerror(ret));
		return -EIO;
	} else {
		if (options.formatraw) {
			ret = _ar_read_raw(path, buf, size, offset, fi);
		} else	{
			ret = _ar_read(path, buf, size, offset, fi);
		}
		pthread_mutex_unlock(&lock);
	}
	return ret;
}

static off_t
_ar_getsizeraw(const char *path)
{
	off_t offset = 0, ret;
	NODE *node;
	const char *realpath;

	log("ar_getsizeraw called, path: '%s'", path);
	/* find node */
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}

	struct archive *archive;
	struct archive_entry *entry;
	int archive_ret;
	/* search file in archive */
	realpath = archive_entry_pathname(node->entry);

	if ((archive = archive_read_new()) == NULL) {
		log("Out of memory");
		return -ENOMEM;
	}

	archive_ret = archive_read_support_filter_all(archive);

	if (archive_ret != ARCHIVE_OK) {
		log("archive_read_support_filter_all(): %s (%d)\n",
			archive_error_string(archive), archive_ret);
		return -EIO;
	}

	if (options.formatraw) {
		archive_ret = archive_read_support_format_raw(archive);
		if (archive_ret != ARCHIVE_OK) {
			log("archive_read_support_format_raw(): %s (%d)\n",
				archive_error_string(archive), archive_ret);
			return -EIO;
		}
		options.readonly = 1;
		options.formatraw = 1;
	}

	if (options.password) {
		if (archive_read_add_passphrase(archive, user_passphrase) != ARCHIVE_OK) {
			fprintf(stderr, "%s\n", archive_error_string(archive));
			return archive_errno(archive);
		}
	}

	archive_ret = archive_read_open_fd(archive, archiveFd, BLOCK_SIZE);
	if (archive_ret != ARCHIVE_OK) {
		log("archive_read_open_fd(): %s (%d)\n",
			archive_error_string(archive), archive_ret);
		return -EIO;
	}

	/* search for file to read */
	while ((archive_ret = archive_read_next_header(
				archive, &entry)) == ARCHIVE_OK)
	{
		const char *name;
		name = archive_entry_pathname(entry);
		if (strcmp(realpath, name) == 0) {
			void *trash;
			if ((trash = malloc(MAXBUF)) == NULL) {
				log("Out of memory");
				return -ENOMEM;
			}
			/* read until no more data */
			ssize_t readed=MAXBUF;
			while (readed != 0) {
				ret = archive_read_data(
					archive, trash, MAXBUF);
				readed=ret;

				if (ret == ARCHIVE_FATAL
					|| ret == ARCHIVE_WARN
					|| ret == ARCHIVE_RETRY)
				{
					log("ar_read (skipping offset): %s",
						archive_error_string(archive));
					errno = archive_errno(archive);
					ret = -1;
					break;
				}
				offset += ret;
				// log("tmp offset =%ld (%ld)",offset,offset/1024/1024);
			}
			free(trash);
			break;
		}
		archive_read_data_skip(archive);
	} // end of search for file to read
	/* close archive */
	archive_read_free(archive);
	lseek(archiveFd, 0, SEEK_SET);

	return offset;
}

static int
_ar_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	NODE *node;
	int ret;
	off_t size;

	//log("_ar_getattr called, path: '%s'", path);
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* a hardlink, recurse into it */
		ret = _ar_getattr(archive_entry_hardlink(
				node->entry), stbuf, fi);
		return ret;
	}
	if (options.formatraw && ! node->child) {
		fstat(archiveFd, stbuf);
		size=rawcache->st.st_size;
		if (size < 0) return -1;
		stbuf->st_size = size;
	} else {
		memcpy(stbuf, archive_entry_stat(node->entry),
			sizeof(struct stat));
		if (options.formatraw && node->child)
			stbuf->st_size = 4096;
	}
	stbuf->st_blocks  = (stbuf->st_size + 511) / 512;
	stbuf->st_blksize = 4096;
	/* when sharing via Samba nlinks have to be at
	   least 2 for directories or directories will
	   be shown as files, and 1 for files or they
	   cannot be opened */
	if (S_ISDIR(archive_entry_mode(node->entry))) {
		if (stbuf->st_nlink < 2) {
			stbuf->st_nlink = 2;
		}
	} else {
		if (stbuf->st_nlink < 1) {
			stbuf->st_nlink = 1;
		}
	}

	if (options.readonly) {
		stbuf->st_mode = stbuf->st_mode & 0777555;
	}

	return 0;
}

static int
#ifdef HAVE_FUSE3
ar_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
#else
ar_getattr(const char *path, struct stat *stbuf)
#endif
{
	//log("ar_getattr called, path: '%s'", path);
	int ret = pthread_mutex_lock(&lock);
	if (ret) {
		log("failed to get lock: %s\n", strerror(ret));
		return -EIO;
	} else {
		ret = _ar_getattr(path, stbuf, 
#ifdef HAVE_FUSE3
		                  fi);
#else
                                  NULL);
#endif
		pthread_mutex_unlock(&lock);
	}
	return ret;
}

/*
 * mkdir is nearly identical to mknod...
 */
static int
ar_mkdir(const char *path, mode_t mode)
{
	NODE *node;
	char *location;
	int tmp;

	log("ar_mkdir called, path '%s', mode %o", path, mode);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	/* check for existing node */
	node = get_node_for_path(root, path);
	if (node) {
		pthread_mutex_unlock(&lock);
		return -EEXIST;
	}
	/* create name for temp dir */
	if ((tmp = get_temp_file_name(path, &location)) < 0) {
		pthread_mutex_unlock(&lock);
		return tmp;
	}
	/* create temp dir */
	if (mkdir(location, mode) == -1) {
		log("Could not create temporary dir %s: %s",
			location, strerror(errno));
		free(location);
		pthread_mutex_unlock(&lock);
		return 0 - errno;
	}
	/* build node */
	if ((node = init_node()) == NULL) {
		pthread_mutex_unlock(&lock);
		return -ENOMEM;
	}
	node->location = location;
	node->modified = 1;
	node->name = strdup(path);
	node->basename = strrchr(node->name, '/') + 1;
	node->namechanged = 0;
	/* build entry */
	if (root->child &&
		node->name[0] == '/' &&
		archive_entry_pathname(root->child->entry)[0] != '/')
	{
		archive_entry_set_pathname(node->entry, node->name + 1);
	} else {
		archive_entry_set_pathname(node->entry, node->name);
	}
	if ((tmp = update_entry_stat(node)) < 0) {
		log("mkdir: error stat'ing dir %s: %s", node->location,
			strerror(0 - tmp));
		rmdir(location);
		free(location);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return tmp;
	}
	/* add node to tree */
	if (insert_by_path(root, node) != 0) {
		log("ERROR: could not insert %s into tree",
			node->name);
		rmdir(location);
		free(location);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	/* clean up */
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * ar_rmdir is called for directories only and does not need to do any
 * recursive stuff
 */
static int
ar_rmdir(const char *path)
{
	NODE *node;

	log("ar_rmdir called, path '%s'", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	node = get_node_for_path(root, path);
	if (! node) {
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	if (node->child) {
		pthread_mutex_unlock(&lock);
		return -ENOTEMPTY;
	}
	if (node->name[strlen(node->name)-1] == '.') {
		pthread_mutex_unlock(&lock);
		return -EINVAL;
	}
	if (! S_ISDIR(archive_entry_mode(node->entry))) {
		pthread_mutex_unlock(&lock);
		return -ENOTDIR;
	}
	if (node->location) {
		/* remove temp directory */
		if (rmdir(node->location) == -1) {
			int err = errno;
			log("ERROR: removing temp directory %s failed: %s",
				node->location, strerror(err));
			pthread_mutex_unlock(&lock);
			return err;
		}
		free(node->location);
	}
	remove_child(node);
	free_node(node);
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
ar_symlink(const char *from, const char *to)
{
	NODE *node;
	struct stat st;
	struct passwd *pwd;
	struct group *grp;

	log("symlink called, %s -> %s", from, to);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	/* check for existing node */
	node = get_node_for_path(root, to);
	if (node) {
		pthread_mutex_unlock(&lock);
		return -EEXIST;
	}
	/* build node */
	if ((node = init_node()) == NULL) {
		pthread_mutex_unlock(&lock);
		return -ENOMEM;
	}
	node->name = strdup(to);
	node->basename = strrchr(node->name, '/') + 1;
	node->modified = 1;
	/* build stat info */
	st.st_dev = 0;
	st.st_ino = 0;
	st.st_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;
	st.st_nlink = 1;
	st.st_uid = getuid();
	st.st_gid = getgid();
	st.st_rdev = 0;
	st.st_size = strlen(from);
	st.st_blksize = 4096;
	st.st_blocks = 0;
	st.st_atime = st.st_ctime = st.st_mtime = time(NULL);
	/* build entry */
	if (root->child &&
		node->name[0] == '/' &&
		archive_entry_pathname(root->child->entry)[0] != '/')
	{
		archive_entry_set_pathname(node->entry, node->name + 1);
	} else {
		archive_entry_set_pathname(node->entry, node->name);
	}
	archive_entry_copy_stat(node->entry, &st);
	archive_entry_set_symlink(node->entry, strdup(from));
	/* get user/group name */
	pwd = getpwuid(st.st_uid);
	if (pwd) {
		/* a name was found for the uid */
		archive_entry_set_uname(node->entry, strdup(pwd->pw_name));
	} else {
		if (errno == EINTR || errno == EIO || errno == EMFILE ||
			errno == ENFILE || errno == ENOMEM ||
			errno == ERANGE)
		{
			log("ERROR calling getpwuid: %s", strerror(errno));
			free_node(node);
			pthread_mutex_unlock(&lock);
			return 0 - errno;
		}
		/* on other errors the uid just could
		   not be resolved into a name */
	}
	grp = getgrgid(st.st_gid);
	if (grp) {
		/* a name was found for the uid */
		archive_entry_set_gname(node->entry, strdup(grp->gr_name));
	} else {
		if (errno == EINTR || errno == EIO || errno == EMFILE ||
			errno == ENFILE || errno == ENOMEM ||
			errno == ERANGE)
		{
			log("ERROR calling getgrgid: %s", strerror(errno));
			free_node(node);
			pthread_mutex_unlock(&lock);
			return 0 - errno;
		}
		/* on other errors the gid just could
		   not be resolved into a name */
	}
	/* add node to tree */
	if (insert_by_path(root, node) != 0) {
		log("ERROR: could not insert symlink %s into tree",
			node->name);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	/* clean up */
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
ar_link(const char *from, const char *to)
{
	NODE *node;
	NODE *fromnode;
	struct stat st;
	struct passwd *pwd;
	struct group *grp;

	log("ar_link called, %s -> %s", from, to);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	/* find source node */
	fromnode = get_node_for_path(root, from);
	if (! fromnode) {
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	/* check for existing target */
	node = get_node_for_path(root, to);
	if (node) {
		pthread_mutex_unlock(&lock);
		return -EEXIST;
	}
	/* extract originals stat info */
	_ar_getattr(from, &st, NULL);
	/* build new node */
	if ((node = init_node()) == NULL) {
		pthread_mutex_unlock(&lock);
		return -ENOMEM;
	}
	node->name = strdup(to);
	node->basename = strrchr(node->name, '/') + 1;
	node->modified = 1;
	/* build entry */
	if (node->name[0] == '/' &&
		archive_entry_pathname(fromnode->entry)[0] != '/')
	{
		archive_entry_set_pathname(node->entry, node->name + 1);
	} else {
		archive_entry_set_pathname(node->entry, node->name);
	}
	archive_entry_copy_stat(node->entry, &st);
	archive_entry_set_hardlink(node->entry, strdup(from));
	/* get user/group name */
	pwd = getpwuid(st.st_uid);
	if (pwd) {
		/* a name was found for the uid */
		archive_entry_set_uname(node->entry, strdup(pwd->pw_name));
	} else {
		if (errno == EINTR || errno == EIO || errno == EMFILE ||
			errno == ENFILE || errno == ENOMEM ||
			errno == ERANGE)
		{
			log("ERROR calling getpwuid: %s", strerror(errno));
			free_node(node);
			pthread_mutex_unlock(&lock);
			return 0 - errno;
		}
		/* on other errors the uid just could
		   not be resolved into a name */
	}
	grp = getgrgid(st.st_gid);
	if (grp) {
		/* a name was found for the uid */
		archive_entry_set_gname(node->entry, strdup(grp->gr_name));
	} else {
		if (errno == EINTR || errno == EIO || errno == EMFILE ||
			errno == ENFILE || errno == ENOMEM ||
			errno == ERANGE)
		{
			log("ERROR calling getgrgid: %s", strerror(errno));
			free_node(node);
			pthread_mutex_unlock(&lock);
			return 0 - errno;
		}
		/* on other errors the gid just could
		   not be resolved into a name */
	}
	/* add node to tree */
	if (insert_by_path(root, node) != 0) {
		log("ERROR: could not insert hardlink %s into tree",
			node->name);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	/* clean up */
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
_ar_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	NODE *node;
	char *location;
	int ret;
	int tmp;
	int fh;

	log("_ar_truncate called, path '%s'", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_truncate(archive_entry_hardlink(
				node->entry), size, fi);
	}
	if (archive_entry_symlink(node->entry)) {
		/* file is a symlink, recurse into it */
		return _ar_truncate(archive_entry_symlink(
				node->entry), size, fi);
	}
	if (node->location) {
		/* open existing temp file */
		location = node->location;
		if ((fh = open(location, O_WRONLY)) == -1) {
			log("error opening temp file %s: %s",
				location, strerror(errno));
			unlink(location);
			return 0 - errno;
		}
	} else {
		/* create new temp file */
		char *tmpbuf = NULL;
		int tmpoffset = 0;
		int64_t tmpsize;
		struct fuse_file_info fi;
		if ((tmp = get_temp_file_name(path, &location)) < 0) {
			return tmp;
		}
		if ((fh = open(location, O_WRONLY | O_CREAT | O_EXCL,
					archive_entry_mode(node->entry))) == -1)
		{
			log("error opening temp file %s: %s",
				location, strerror(errno));
			unlink(location);
			return 0 - errno;
		}
		/* copy original file to temporary file */
		tmpsize = archive_entry_size(node->entry);
		if ((tmpbuf = (char *)malloc(MAXBUF)) == NULL) {
			log("Out of memory");
			return -ENOMEM;
		}
		while (tmpsize) {
			int len = tmpsize > MAXBUF ? MAXBUF : tmpsize;
			/* read */
			if ((tmp = _ar_read(path, tmpbuf, len, tmpoffset, &fi))
				< 0)
			{
				log("ERROR reading while copying %s to "
					"temporary location %s: %s",
					path, location,
					strerror(0 - tmp));
				close(fh);
				unlink(location);
				free(tmpbuf);
				return tmp;
			}
			/* write */
			if (write(fh, tmpbuf, tmp) == -1) {
				tmp = 0 - errno;
				log("ERROR writing while copying %s to "
					"temporary location %s: %s",
					path, location,
					strerror(errno));
				close(fh);
				unlink(location);
				free(tmpbuf);
				return tmp;
			}
			tmpsize -= len;
			tmpoffset += len;
			if (tmpoffset >= size) {
				/* copied enough, exit the loop */
				break;
			}
		}
		/* clean up */
		free(tmpbuf);
		lseek(fh, 0, SEEK_SET);
	}
	/* truncate temporary file */
	if ((ret = truncate(location, size)) == -1) {
		tmp = 0 - errno;
		log("ERROR truncating %s (temporary location %s): %s",
			path, location, strerror(errno));
		close(fh);
		unlink(location);
		return tmp;
	}
	/* record location, update entry */
	node->location = location;
	node->modified = 1;
	if ((tmp = update_entry_stat(node)) < 0) {
		log("write: error stat'ing file %s: %s", node->location,
			strerror(0 - tmp));
		close(fh);
		unlink(location);
		return tmp;
	}
	/* clean up */
	close(fh);
	archiveModified = 1;
	return ret;
}

static int
#ifdef HAVE_FUSE3
ar_truncate(const char *path, off_t size, struct fuse_file_info *fi)
#else
ar_truncate(const char *path, off_t size)
#endif
{
	int ret;
	log("ar_truncate called, path '%s'", path);
	pthread_mutex_lock(&lock);
	ret = _ar_truncate(path, size, 
#ifdef HAVE_FUSE3
                           fi
#else
                           NULL
#endif
                          );
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
_ar_write(const char *path, const char *buf, size_t size,
	off_t offset, struct fuse_file_info *fi)
{
	NODE *node;
	char *location;
	int ret;
	int tmp;
	int fh;

	log("_ar_write called, path '%s'", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (S_ISLNK(archive_entry_mode(node->entry))) {
		/* file is a symlink, recurse into it */
		return _ar_write(archive_entry_symlink(node->entry),
			buf, size, offset, fi);
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_write(archive_entry_hardlink(node->entry),
			buf, size, offset, fi);
	}
	if (archive_entry_symlink(node->entry)) {
		/* file is a symlink, recurse into it */
		return _ar_write(archive_entry_symlink(node->entry),
			buf, size, offset, fi);
	}
	if (node->location) {
		/* open existing temp file */
		location = node->location;
		if ((fh = open(location, O_WRONLY)) == -1) {
			log("error opening temp file %s: %s",
				location, strerror(errno));
			unlink(location);
			return 0 - errno;
		}
	} else {
		/* create new temp file */
		char *tmpbuf = NULL;
		int tmpoffset = 0;
		int64_t tmpsize;
		if ((tmp = get_temp_file_name(path, &location)) < 0) {
			return tmp;
		}
		if ((fh = open(location, O_WRONLY | O_CREAT | O_EXCL,
					archive_entry_mode(node->entry))) == -1)
		{
			log("error opening temp file %s: %s",
				location, strerror(errno));
			unlink(location);
			return 0 - errno;
		}
		/* copy original file to temporary file */
		tmpsize = archive_entry_size(node->entry);
		if ((tmpbuf = (char *)malloc(MAXBUF)) == NULL) {
			log("Out of memory");
			return -ENOMEM;
		}
		while (tmpsize) {
			int len = tmpsize > MAXBUF ? MAXBUF : tmpsize;
			/* read */
			if ((tmp = _ar_read(path, tmpbuf, len, tmpoffset, fi))
				< 0)
			{
				log("ERROR reading while copying %s to "
					"temporary location %s: %s",
					path, location,
					strerror(0 - tmp));
				close(fh);
				unlink(location);
				free(tmpbuf);
				return tmp;
			}
			/* write */
			if (write(fh, tmpbuf, len) == -1) {
				tmp = 0 - errno;
				log("ERROR writing while copying %s to "
					"temporary location %s: %s",
					path, location,
					strerror(errno));
				close(fh);
				unlink(location);
				free(tmpbuf);
				return tmp;
			}
			tmpsize -= len;
			tmpoffset += len;
		}
		/* clean up */
		free(tmpbuf);
		lseek(fh, 0, SEEK_SET);
	}
	/* write changes to temporary file */
	if ((ret = pwrite(fh, buf, size, offset)) == -1) {
		tmp = 0 - errno;
		log("ERROR writing changes to %s (temporary "
			"location %s): %s",
			path, location, strerror(errno));
		close(fh);
		unlink(location);
		return tmp;
	}
	/* record location, update entry */
	node->location = location;
	node->modified = 1;
	if ((tmp = update_entry_stat(node)) < 0) {
		log("write: error stat'ing file %s: %s", node->location,
			strerror(0 - tmp));
		close(fh);
		unlink(location);
		return tmp;
	}
	/* clean up */
	close(fh);
	archiveModified = 1;
	return ret;
}

static int
ar_write(const char *path, const char *buf, size_t size,
	off_t offset, struct fuse_file_info *fi)
{
	int ret;
	log("ar_write called, path '%s'", path);
	pthread_mutex_lock(&lock);
	ret = _ar_write(path, buf, size, offset, fi);
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
ar_mknod(const char *path, mode_t mode, dev_t rdev)
{
	NODE *node;
	char *location;
	int tmp;

	log("ar_mknod called, path %s", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	/* check for existing node */
	node = get_node_for_path(root, path);
	if (node) {
		pthread_mutex_unlock(&lock);
		return -EEXIST;
	}
	/* create name for temp file */
	if ((tmp = get_temp_file_name(path, &location)) < 0) {
		pthread_mutex_unlock(&lock);
		return tmp;
	}
	/* create temp file */
	if (mknod(location, mode, rdev) == -1) {
		log("Could not create temporary file %s: %s",
			location, strerror(errno));
		free(location);
		pthread_mutex_unlock(&lock);
		return 0 - errno;
	}
	/* build node */
	if ((node = init_node()) == NULL) {
		pthread_mutex_unlock(&lock);
		return -ENOMEM;
	}
	node->location = location;
	node->modified = 1;
	node->name = strdup(path);
	node->basename = strrchr(node->name, '/') + 1;

	/* build entry */
	if (root->child &&
		node->name[0] == '/' &&
		archive_entry_pathname(root->child->entry)[0] != '/')
	{
		archive_entry_set_pathname(node->entry, node->name + 1);
	} else {
		archive_entry_set_pathname(node->entry, node->name);
	}
	if ((tmp = update_entry_stat(node)) < 0) {
		log("mknod: error stat'ing file %s: %s", node->location,
			strerror(0 - tmp));
		unlink(location);
		free(location);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return tmp;
	}
	/* add node to tree */
	if (insert_by_path(root, node) != 0) {
		log("ERROR: could not insert %s into tree",
			node->name);
		unlink(location);
		free(location);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	/* clean up */
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
_ar_unlink(const char *path)
{
	NODE *node;

	log("_ar_unlink called, %s", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (S_ISDIR(archive_entry_mode(node->entry))) {
		return -EISDIR;
	}
	if (node->location) {
		/* remove temporary file */
		if (unlink(node->location) == -1) {
			int err = errno;
			log("ERROR: could not unlink temporary file '%s': %s",
				node->location, strerror(err));
			return err;
		}
		free(node->location);
	}
	remove_child(node);
	free_node(node);
	archiveModified = 1;
	return 0;
}

static int
ar_unlink(const char *path)
{
	log("ar_unlink called, path '%s'", path);
	int ret;
	pthread_mutex_lock(&lock);
	ret = _ar_unlink(path);
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
_ar_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	NODE *node;

	log("_ar_chmod called, path '%s', mode: %o", path, mode);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_chmod(archive_entry_hardlink(node->entry), mode, fi);
	}
	if (archive_entry_symlink(node->entry)) {
		/* file is a symlink, recurse into it */
		return _ar_chmod(archive_entry_symlink(node->entry), mode, fi);
	}
#ifdef __APPLE__
	/* Make sure the full mode, including file type information, is used */
	mode = (0777000 & archive_entry_mode(node->entry)) | (0000777 & mode);
#endif // __APPLE__
	archive_entry_set_mode(node->entry, mode);
	archiveModified = 1;
	return 0;
}

static int
#ifdef HAVE_FUSE3
ar_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
#else
ar_chmod(const char *path, mode_t mode)
#endif
{
	log("ar_chmod called, path '%s', mode: %o", path, mode);
	int ret;
	pthread_mutex_lock(&lock);
	ret = _ar_chmod(path, mode,
#ifdef HAVE_FUSE3
                        fi
#else
                        NULL
#endif
                        );
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
_ar_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	NODE *node;

	log("_ar_chown called, %s", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_chown(archive_entry_hardlink(node->entry),
			uid, gid, NULL);
	}
	/* changing ownership of symlinks is allowed, however */
	archive_entry_set_uid(node->entry, uid);
	archive_entry_set_gid(node->entry, gid);
	archiveModified = 1;
	return 0;
}

static int
#ifdef HAVE_FUSE3
ar_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
#else
ar_chown(const char *path, uid_t uid, gid_t gid)
#endif
{
	log("ar_chown called, %s", path);
	int ret;
	pthread_mutex_lock(&lock);
	ret = _ar_chown(path, uid, gid,
#ifdef HAVE_FUSE3
                        fi
#else
                        NULL
#endif
                        );
	pthread_mutex_unlock(&lock);
	return ret;
}

#ifdef HAVE_FUSE3
static int
_ar_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
	NODE *node;

	log("_ar_utimens called, %s", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_utimens(archive_entry_hardlink(node->entry), tv, fi);
	}
	if (archive_entry_symlink(node->entry)) {
		/* file is a symlink, recurse into it */
		return _ar_utimens(archive_entry_symlink(node->entry), tv, fi);
	}
	archive_entry_set_mtime(node->entry, tv[0].tv_sec, tv[0].tv_nsec);
	archive_entry_set_atime(node->entry, tv[1].tv_sec, tv[1].tv_nsec);
	archiveModified = 1;
	return 0;
}

int
ar_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
	log("ar_utimens called, %s", path);
	int ret;
	pthread_mutex_lock(&lock);
	ret = _ar_utimens(path, tv, fi);
	pthread_mutex_unlock(&lock);
	return ret;
}
#else
static int
_ar_utime(const char *path, struct utimbuf *buf)
{
	NODE *node;

	log("_ar_utime called, %s", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		return -ENOENT;
	}
	if (archive_entry_hardlink(node->entry)) {
		/* file is a hardlink, recurse into it */
		return _ar_utime(archive_entry_hardlink(node->entry), buf);
	}
	if (archive_entry_symlink(node->entry)) {
		/* file is a symlink, recurse into it */
		return _ar_utime(archive_entry_symlink(node->entry), buf);
	}
	archive_entry_set_mtime(node->entry, buf->modtime, 0);
	archive_entry_set_atime(node->entry, buf->actime, 0);
	archiveModified = 1;
	return 0;
}

static int
ar_utime(const char *path, struct utimbuf *buf)
{
	log("ar_utime called, %s", path);
	int ret;
	pthread_mutex_lock(&lock);
	ret = _ar_utime(path, buf);
	pthread_mutex_unlock(&lock);
	return ret;
}
#endif
static int
ar_statfs(const char *path, struct statvfs *stbuf)
{
	(void)path;

	log("ar_statfs called, %s", path);

	/* Adapted the following from sshfs.c */

	stbuf->f_namemax = 255;
	stbuf->f_bsize = 4096;
	/*
	 * df seems to use f_bsize instead of f_frsize, so make them
	 * the same
	 */
	stbuf->f_frsize = stbuf->f_bsize;
	stbuf->f_blocks = stbuf->f_bfree =  stbuf->f_bavail =
		1000ULL * 1024 * 1024 * 1024 / stbuf->f_frsize;
	stbuf->f_files = stbuf->f_ffree = 1000000000;
	return 0;
}

static int
#ifdef HAVE_FUSE3
ar_rename(const char *from, const char *to, unsigned int flags)
#else
ar_rename(const char *from, const char *to)
#endif
{
	NODE *from_node;
	int ret = 0;
	char *temp_name;

	log("ar_rename called, from: '%s', to: '%s'", from, to);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	from_node = get_node_for_path(root, from);
	if (! from_node) {
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	{
		/* before actually renaming the from_node, we must remove
		 * the to_node if it exists */
		NODE *to_node;
		to_node = get_node_for_path(root, to);
		if (to_node) {
			ret = _ar_unlink(to_node->name);
			if (0 != ret) {
				return ret;
			}
		}
	}
	/* meta data is changed in save() */
	/* change from_node name */
	if (*to != '/') {
		if ((temp_name = malloc(strlen(to) + 2)) == NULL) {
			log("Out of memory");
			pthread_mutex_unlock(&lock);
			return -ENOMEM;
		}
		sprintf(temp_name, "/%s", to);
	} else {
		if ((temp_name = strdup(to)) == NULL) {
			log("Out of memory");
			pthread_mutex_unlock(&lock);
			return -ENOMEM;
		}
	}
	remove_child(from_node);
	correct_hardlinks_to_node(root, from_node->name, temp_name);
	free(from_node->name);
	from_node->name = temp_name;
	from_node->basename = strrchr(from_node->name, '/') + 1;
	from_node->namechanged = 1;
	ret = insert_by_path(root, from_node);
	if (0 != ret) {
		log ("failed to re-insert node %s", from_node->name);
	}
	if (from_node->child) {
		/* it is a directory, recursive change of all from_nodes
		 * below it is required */
		ret = rename_recursively(from_node->child, from, to);
	}
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
ar_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
	/* Just a stub.  This method is optional and can safely be left
	   unimplemented */
	(void)path;
	(void)isdatasync;
	(void)fi;
	return 0;
}

static int
ar_readlink(const char *path, char *buf, size_t size)
{
	NODE *node;
	const char *tmp;

	log("ar_readlink called, path '%s'", path);
	int ret = pthread_mutex_lock(&lock);
	if (ret) {
		fprintf(stderr, "could not acquire lock for archive: %s\n", strerror(ret));
		return ret;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	if (! S_ISLNK(archive_entry_mode(node->entry))) {
		pthread_mutex_unlock(&lock);
		return -ENOLINK;
	}
	tmp = archive_entry_symlink(node->entry);
	snprintf(buf, size, "%s", tmp);
	pthread_mutex_unlock(&lock);

	return 0;
}

static int
ar_open(const char *path, struct fuse_file_info *fi)
{
	NODE *node;

	log("ar_open called, path '%s'", path);
	int ret = pthread_mutex_lock(&lock);
	if (ret) {
		fprintf(stderr, "could not acquire lock for archive: %s\n", strerror(ret));
		return ret;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	if (fi->flags & O_WRONLY || fi->flags & O_RDWR) {
		if (! archiveWriteable) {
			pthread_mutex_unlock(&lock);
			return -EROFS;
		}
	}
	/* no need to recurse into links since function doesn't do anything */
	/* no need to save a handle here since archives are stream based */
	fi->fh = 0;
	if (options.formatraw)
		_ar_open_raw();
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
ar_release(const char *path, struct fuse_file_info *fi)
{
	(void)fi;
	(void)path;
	log("ar_release called, path '%s'", path);
	return 0;
}

#ifdef HAVE_FUSE3
#define DIR_FILLER(F,B,N,S,O) F(B,N,S,O,FUSE_FILL_DIR_PLUS)
#else
#define DIR_FILLER(F,B,N,S,O) F(B,N,S,O)
#endif

static int
#ifdef HAVE_FUSE3
ar_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
#else
ar_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info *fi)
#endif
{
	NODE *node;
	(void) offset;
	(void) fi;

	//log("ar_readdir called, path: '%s' offset: %d", path, offset);
	int ret = -EIO;
	if (pthread_mutex_lock(&lock)) {
		log("could not acquire lock for archive: %s\n", strerror(ret));
		return ret;
	}
	node = get_node_for_path(root, path);
	if (! node) {
		log("path '%s' not found", path);
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}

	DIR_FILLER(filler, buf, ".", NULL, 0);
	DIR_FILLER(filler, buf, "..", NULL, 0);

	node = node->child;

	while (node) {
		const struct stat *st;
		struct stat st_copy;
		if (archive_entry_hardlink(node->entry)) {
			/* file is a hardlink, stat'ing it somehow does not
			 * work; stat the original instead */
			NODE *orig = get_node_for_path(root, archive_entry_hardlink(node->entry));
			if (! orig) {
				return -ENOENT;
			}
			st = archive_entry_stat(orig->entry);
		} else {
			st = archive_entry_stat(node->entry);
		}
		/* Make a copy so we can set blocks/blksize. These are not
		 * set by libarchive. Issue 191 */
		memcpy(&st_copy, st, sizeof(st_copy));
		st_copy.st_blocks  = (st_copy.st_size + 511) / 512;
		st_copy.st_blksize = 4096;

		if (DIR_FILLER(filler, buf, node->basename, &st_copy, 0)) {
			pthread_mutex_unlock(&lock);
			return -ENOMEM;
		}

		node = node->hh.next;
	}

	pthread_mutex_unlock(&lock);
	return 0;
}


static int
ar_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	NODE *node;
	char *location;
	int tmp;

	/* the implementation of this function is mostly copy-paste from
	   mknod, with the exception that the temp file is created with
	   creat() instead of mknod() */
	log("ar_create called, path '%s'", path);
	if (! archiveWriteable || options.readonly) {
		return -EROFS;
	}
	pthread_mutex_lock(&lock);
	/* check for existing node */
	node = get_node_for_path(root, path);
	if (node) {
		pthread_mutex_unlock(&lock);
		return -EEXIST;
	}
	/* create name for temp file */
	if ((tmp = get_temp_file_name(path, &location)) < 0) {
		pthread_mutex_unlock(&lock);
		return tmp;
	}
	/* create temp file */
	if (creat(location, mode) == -1) {
		log("Could not create temporary file %s: %s",
			location, strerror(errno));
		free(location);
		pthread_mutex_unlock(&lock);
		return 0 - errno;
	}
	/* build node */
	if ((node = init_node()) == NULL) {
		pthread_mutex_unlock(&lock);
		return -ENOMEM;
	}
	node->location = location;
	node->modified = 1;
	node->name = strdup(path);
	node->basename = strrchr(node->name, '/') + 1;

	/* build entry */
	correct_name_in_entry(node);
	if ((tmp = update_entry_stat(node)) < 0) {
		log("mknod: error stat'ing file %s: %s", node->location,
			strerror(0 - tmp));
		unlink(location);
		free(location);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return tmp;
	}
	/* add node to tree */
	if (insert_by_path(root, node) != 0) {
		log("ERROR: could not insert %s into tree",
			node->name);
		unlink(location);
		free(location);
		free_node(node);
		pthread_mutex_unlock(&lock);
		return -ENOENT;
	}
	/* clean up */
	archiveModified = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static struct fuse_operations ar_oper = {
	.getattr	= ar_getattr,
	.readlink	= ar_readlink,
	.mknod		= ar_mknod,
	.mkdir		= ar_mkdir,
	.symlink	= ar_symlink,
	.unlink		= ar_unlink,
	.rmdir		= ar_rmdir,
	.rename		= ar_rename,
	.link		= ar_link,
	.chmod		= ar_chmod,
	.chown		= ar_chown,
	.truncate	= ar_truncate,
#ifdef HAVE_FUSE3
        .utimens         = ar_utimens,
#else
	.utime		= ar_utime,
#endif
	.open		= ar_open,
	.read		= ar_read,
	.write		= ar_write,
	.statfs		= ar_statfs,
	//.flush	  = ar_flush,  // int(*flush)(const char *, struct fuse_file_info *)
	.release	= ar_release,
	.fsync		= ar_fsync,
/*
#ifdef HAVE_SETXATTR
	.setxattr	= ar_setxattr,
	.getxattr	= ar_getxattr,
	.listxattr	= ar_listxattr,
	.removexattr	= ar_removexattr,
#endif
*/
	//.opendir	  = ar_opendir,    // int(*opendir)(const char *, struct fuse_file_info *)
	.readdir	= ar_readdir,
	//.releasedir	  = ar_releasedir, // int(*releasedir)(const char *, struct fuse_file_info *)
	//.fsyncdir	  = ar_fsyncdir,   // int(*fsyncdir)(const char *, int, struct fuse_file_info *)
	//.init		  = ar_init,	   // void *(*init)(struct fuse_conn_info *conn)
	//.destroy	  = ar_destroy,    // void(*destroy)(void *)
	//.access	  = ar_access,	   // int(*access)(const char *, int)
	.create		= ar_create,
	//.ftruncate	  = ar_ftruncate,  // int(*ftruncate)(const char *, off_t, struct fuse_file_info *)
	//.fgetattr	  = ar_fgetattr,   // int(*fgetattr)(const char *, struct stat *, struct fuse_file_info *)
	//.lock		  = ar_lock,	   // int(*lock)(const char *, struct fuse_file_info *, int cmd, struct flock *)
	//.utimens	  = ar_utimens,    // int(*utimens)(const char *, const struct timespec tv[2])
	//.bmap		  = ar_bmap,	   // int(*bmap)(const char *, size_t blocksize, uint64_t *idx)
};

void
showUsage()
{
	fprintf(stderr, "Usage: archivemount <fuse-options> <archive> <mountpoint>\n");
	fprintf(stderr, "Usage:	      (-v|--version)\n");
}

void setEcho(int echo)
{
	struct termios t;
	tcgetattr(STDIN_FILENO, &t);
	t.c_lflag = (t.c_lflag & ~ECHO) | (echo ? ECHO : 0);
	tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

/* This is basically getline(3), re-implemented to avoid requiring
 * _POSIX_C_SOURCE >= 200809L. */
ssize_t getLine(char **lineptr, size_t *n, FILE *stream) {
	int can_realloc = 0;
	ssize_t count = 0;
	if (*lineptr == NULL && *n == 0) {
		can_realloc = 1;
		*n = 16;
		*lineptr = malloc(*n);
		if (*lineptr == NULL) return -1;
	}
	for (;;) {
		if (count >= *n - 1) {
			if (can_realloc) {
				*n *= 2;
				lineptr = realloc(lineptr, *n);
				if (*lineptr == NULL) return -1;
			} else {
				(*lineptr)[*n] = '\0';
				return *n;
			}
		}
		int c = fgetc(stream);
		switch (c) {
			default:    (*lineptr)[count++] = c;    break;
			case '\n':  (*lineptr)[count++] = c;    /* fall through */
			case EOF:   (*lineptr)[count]   = '\0';
			            return (c == '\n' || feof(stream)) ? count : -1;
		}
	}
}

ssize_t getPassphrase(char **lineptr, size_t *n, FILE *stream) {
	ssize_t ret = getLine(lineptr, n, stream);
	/* Strip newline off the end */
	if (ret > 0 && (*lineptr)[ret - 1] == '\n') {
		(*lineptr)[--ret] = '\0';
	}
	return ret;
}

int
main(int argc, char **argv)
{
	struct stat st;
	int oldpwd;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	/* parse cmdline args */
	memset(&options, 0, sizeof(struct options));
	if (fuse_opt_parse(&args, &options, ar_opts, ar_opt_proc) == -1)
		return -1;
	if (archiveFile==NULL) {
		fprintf(stderr, "missing archive file\n");
		fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
		exit(1);
	}
	if (mtpt==NULL) {
		fprintf(stderr, "missing mount point\n");
		fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
		exit(1);
	}

	/* check if mtpt is ok and writeable */
	if (stat(mtpt, &st) != 0) {
		perror("Error stat'ing mountpoint");
		exit(EXIT_FAILURE);
	}
	if (! S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Problem with mountpoint: %s\n",
			strerror(ENOTDIR));
		exit(EXIT_FAILURE);
	}

	if (options.password) {
		setEcho(0);
		fputs("Enter passphrase:", stderr);
		getPassphrase(&user_passphrase, &user_passphrase_size, stdin);
		fputs("\n", stderr);
		setEcho(1);
	}

	if (!options.readonly) {
		/* check if archive is writeable */
		archiveFd = open(archiveFile, O_RDWR);
		if (archiveFd != -1) {
			archiveWriteable = 1;
			close(archiveFd);
		}
	}
	/* We want the temporary fuser version of the archive to be writable,*/
	/* despite never actually writing the changes to disk.*/
	if (options.nosave) { archiveWriteable = 1; }

	/* open archive and read meta data */
	archiveFd = open(archiveFile, O_RDONLY);
	if (archiveFd == -1) {
		perror("opening archive failed");
		return EXIT_FAILURE;
	}
	if (build_tree(mtpt) != 0) {
		exit(EXIT_FAILURE);
	}

	if (options.formatraw) {
		/* create rawcache */
		if ((rawcache=init_rawcache()) == NULL)
			return -ENOMEM;
		fprintf(stderr,"Calculating uncompressed file size. Please wait.\n");
		rawcache->st.st_size=_ar_getsizeraw("/data");
		//log("cache st_size = %ld",rawcache->st.st_size);
	}

	/* save directory this was started from */
	oldpwd = open(".", 0);

	/* Initialize the node tree lock */
	pthread_mutex_init(&lock, NULL);

	/* always use fuse in single-threaded mode
	 * multithreading is broken with libarchive :-(
	 */
	fuse_opt_add_arg(&args, "-s");

#ifdef HAVE_FUSE3
		/* now do the real mount */
		int fuse_ret;
		fuse_ret = fuse_main(args.argc, args.argv, &ar_oper, NULL);
#else
#if FUSE_VERSION >= 26
	{
		struct fuse *fuse;
		struct fuse_chan *ch;
		char *mountpoint;
		int multithreaded;
		int foreground;
#ifdef FUSE_NUMA
		int numa;
#endif
		int res;

		res = fuse_parse_cmdline(&args, &mountpoint, &multithreaded,
#ifdef FUSE_NUMA
			&foreground, &numa);
#else
		&foreground);
#endif
		if (res == -1)
			exit(1);

		ch = fuse_mount(mountpoint, &args);
		if (!ch)
			exit(1);

		res = fcntl(fuse_chan_fd(ch), F_SETFD, FD_CLOEXEC);
		if (res == -1)
			perror("WARNING: failed to set FD_CLOEXEC on fuse device");

		fuse = fuse_new(ch, &args, &ar_oper,
			sizeof(struct fuse_operations), NULL);
		if (fuse == NULL) {
			fuse_unmount(mountpoint, ch);
			exit(1);
		}

		/* now do the real mount */
		res = fuse_daemonize(foreground);
		if (res != -1)
			res = fuse_set_signal_handlers(fuse_get_session(fuse));

		if (res == -1) {
			fuse_unmount(mountpoint, ch);
			fuse_destroy(fuse);
			exit(1);
		}

		if (multithreaded)
			res = fuse_loop_mt(fuse);
		else
			res = fuse_loop(fuse);

		if (res == -1)
			res = 1;
		else
			res = 0;

		fuse_remove_signal_handlers(fuse_get_session(fuse));
		fuse_unmount(mountpoint, ch);
		fuse_destroy(fuse);
		free(mountpoint);
	}
#else
	{
		/* now do the real mount */
		int fuse_ret;
		fuse_ret = fuse_main(args.argc, args.argv, &ar_oper, NULL);
	}
#endif
#endif

	/* go back to saved dir */
	{
		int fchdir_ret;
		fchdir_ret = fchdir(oldpwd);
		if (fchdir_ret != 0) {
			log("fchdir() to old path failed\n");
		}
	}

	/* save changes if modified */
	if (archiveWriteable && !options.readonly && archiveModified && !options.nosave) {
		if (save(archiveFile) != 0) {
			log("Saving new archive failed\n");
		}
	}

	/* clean up */
	close(archiveFd);
	if (options.password) {
		memset(user_passphrase, 0, user_passphrase_size);
		free(user_passphrase);
	}

	return EXIT_SUCCESS;
}

/*
vim:ts=8:softtabstop=8:sw=8:noexpandtab
*/


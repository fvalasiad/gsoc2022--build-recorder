
/*
Copyright (C) 2022 Valasiadis Fotios
Copyright (C) 2022 Alexios Zavras
SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include	"config.h"

#include	<errno.h>
#include	<stdlib.h>
#include	<stdio.h>
#include	<stddef.h>
#include	<string.h>
#include	<unistd.h>
#include	<fcntl.h>

#include	<sys/ptrace.h>
#include	<linux/ptrace.h>

#include	<sys/signal.h>
#include	<sys/syscall.h>
#include	<sys/wait.h>
#include	<linux/limits.h>

#include	"types.h"
#include	"hash.h"
#include	"record.h"

/*
 * variables for the list of processes,
 * its size and the array size. As well as
 * a list of their respective pids with the
 * same size and array size.
 */

int *pids;
PROCESS_INFO *pinfo;
int numpinfo;
int pinfo_size;

FILE_INFO *finfo;
int numfinfo;
int finfo_size;

#define	DEFAULT_PINFO_SIZE	32
#define	DEFAULT_FINFO_SIZE	32

/*
 * memory allocators for pinfo
 */

void
init(void)
{
    pinfo_size = DEFAULT_PINFO_SIZE;
    pinfo = calloc(pinfo_size, sizeof (PROCESS_INFO));
    if (!pinfo) {
	perror("tracer.c:init():calloc(pinfo)");
	exit(EXIT_FAILURE);
    }
    pids = malloc(pinfo_size * sizeof (int));
    if (!pids) {
	perror("tracer.c:init():malloc(pids)");
	exit(EXIT_FAILURE);
    }
    numpinfo = -1;

    finfo_size = DEFAULT_FINFO_SIZE;
    finfo = calloc(finfo_size, sizeof (FILE_INFO));
    if (!finfo) {
	perror("tracer.c:init():malloc(finfo)");
	exit(EXIT_FAILURE);
    }

    numfinfo = -1;
}

PROCESS_INFO *
next_pinfo(pid_t pid)
{
    if (numpinfo == pinfo_size - 1) {
	pinfo_size *= 2;
	pinfo = reallocarray(pinfo, pinfo_size, sizeof (PROCESS_INFO));
	if (pinfo == NULL) {
	    perror("tracer.c:next_pinfo:reallocarray(pinfo)");
	    exit(EXIT_FAILURE);
	}

	pids = reallocarray(pids, pinfo_size, sizeof (int));
	if (pids == NULL) {
	    perror("tracer.c:next_pinfo:reallocarray(pids)");
	    exit(EXIT_FAILURE);
	}
    }

    pids[numpinfo + 1] = pid;
    return pinfo + (++numpinfo);
}

FILE_INFO *
next_finfo(void)
{
    if (numfinfo == finfo_size - 1) {
	finfo_size *= 2;
	finfo = reallocarray(finfo, finfo_size, sizeof (FILE_INFO));
	if (finfo == NULL) {
	    perror("tracer.c:next_finfo:reallocarray(finfo)");
	    exit(EXIT_FAILURE);
	}
    }

    return finfo + (++numfinfo);
}

void
pinfo_new(PROCESS_INFO *self, char ignore_one_sigstop)
{
    static int pcount = 0;

    sprintf(self->outname, ":p%d", pcount++);
    self->finfo_size = DEFAULT_FINFO_SIZE;
    self->numfinfo = -1;
    self->finfo = malloc(self->finfo_size * sizeof (FILE_INFO));
    self->fds = malloc(self->finfo_size * sizeof (int));
    self->ignore_one_sigstop = ignore_one_sigstop;
}

void
finfo_new(FILE_INFO *self, char *path, char *abspath, char *hash, size_t sz)
{
    static int fcount = 0;

    self->path = path;
    self->abspath = abspath;
    self->hash = hash;
    self->size = sz;
    sprintf(self->outname, ":f%d", fcount++);
}

PROCESS_INFO *
find_pinfo(pid_t pid)
{
    int i = numpinfo;

    while (i >= 0 && pids[i] != pid) {
	--i;
    }

    if (i < 0) {
	return NULL;
    }

    return pinfo + i;
}

FILE_INFO *
find_finfo(char *abspath, char *hash)
{
    int i = numfinfo;

    while (i >= 0) {
	if (!strcmp(abspath, finfo[i].abspath)
	    && ((hash == NULL && finfo[i].hash == NULL)
		|| (hash != NULL && finfo[i].hash != NULL
		    && !strcmp(hash, finfo[i].hash)))) {

	    break;
	}

	--i;
    }

    if (i < 0) {
	return NULL;
    }

    return finfo + i;
}

FILE_INFO *
pinfo_find_finfo(PROCESS_INFO *self, int fd)
{
    int i = self->numfinfo;

    while (i >= 0 && self->fds[i] != fd) {
	--i;
    }

    if (i < 0) {
	return NULL;
    }

    return self->finfo + i;
}

FILE_INFO *
pinfo_next_finfo(PROCESS_INFO *self, int fd)
{
    if (self->numfinfo == self->finfo_size - 1) {
	self->finfo_size *= 2;
	self->finfo =
		reallocarray(self->finfo, self->finfo_size, sizeof (FILE_INFO));
	self->fds =
		reallocarray(self->fds, self->finfo_size, sizeof (FILE_INFO));
	if (self->finfo == NULL) {
	    perror("tracer.c:pinfo_next_finfo:reallocarray(pinfo->finfo)");
	    exit(EXIT_FAILURE);
	}
    }

    self->fds[self->numfinfo + 1] = fd;
    return self->finfo + (++self->numfinfo);
}

char *
get_str_from_process(pid_t pid, void *addr)
{
    static char buf[PATH_MAX];
    char *dest = buf;
    union {
	long lval;
	char cval[sizeof (long)];
    } data;

    size_t i = 0;

    do {
	data.lval =
		ptrace(PTRACE_PEEKDATA, pid, (char *) addr + i * sizeof (long),
		       NULL);
	for (unsigned j = 0; j < sizeof (long); j++) {
	    *dest++ = data.cval[j];
	    if (data.cval[j] == 0)
		break;
	}
	++i;
    } while (dest[-1]);

    return strdup(buf);
}

char *
absolutepath(pid_t pid, int dirfd, char *addr)
{
    char symbpath[PATH_MAX];

    if (*addr == '/') {
	return realpath(addr, NULL);
    }
    if (dirfd == AT_FDCWD) {
	sprintf(symbpath, "/proc/%d/cwd/%s", pid, addr);
	return realpath(symbpath, NULL);
    }

    sprintf(symbpath, "/proc/%d/fd/%d/%s", pid, dirfd, addr);
    return realpath(symbpath, NULL);
}

char *
find_in_path(char *path)
{
    static char buf[PATH_MAX];
    char *ret;
    char *PATH = strdup(getenv("PATH"));

    if (!PATH) {
	perror("tracer.c:find_in_path():strdup(getenv(PATH))");
	exit(EXIT_FAILURE);
    }
    char *it = PATH;
    char *last;

    do {
	last = strchr(it, ':');
	if (last) {
	    *last = '\0';
	}

	sprintf(buf, "%s/%s", it, path);
	ret = realpath(buf, NULL);
	if (!ret && (errno != 0 && errno != ENOENT)) {
	    fprintf(stderr, "tracer.c:find_in_path:realpath(%s): %s\n", path,
		    strerror(errno));
	    exit(EXIT_FAILURE);
	}
	it = last + 1;
    } while (last != NULL && ret == NULL);

    free(PATH);

    return ret;
}

static size_t
get_file_size(char *fname)
{
    struct stat fstat;

    if (stat(fname, &fstat)) {
	fprintf(stderr, "tracer.c:get_file_size(%s): %s\n", fname,
		strerror(errno));
	return -1;
    }

    return fstat.st_size;
}

static void
handle_open(pid_t pid, PROCESS_INFO *pi, int fd, int dirfd, void *path,
	    int purpose)
{
    path = get_str_from_process(pid, path);
    char *abspath = absolutepath(pid, dirfd, path);

    if (abspath == NULL) {
	fprintf(stderr, "tracer.c:handle_open:absolutepath(%s): %s\n",
		(char *) path, strerror(errno));
	exit(EXIT_FAILURE);
    }

    FILE_INFO *f = NULL;

    if ((purpose & O_ACCMODE) == O_RDONLY) {
	char *hash = get_file_hash(abspath);
	size_t sz = get_file_size(abspath);

	f = find_finfo(abspath, hash);
	if (!f) {
	    f = next_finfo();
	    finfo_new(f, path, abspath, hash, sz);
	    record_file(f->outname, path, abspath);
	    record_hash(f->outname, hash);
	    record_size(f->outname, sz);
	} else {
	    free(path);
	    free(abspath);
	    free(hash);
	}
    } else {
	f = pinfo_next_finfo(pi, fd);
	finfo_new(f, path, abspath, NULL, -1);
	record_file(f->outname, path, abspath);
    }

    record_fileuse(pi->outname, f->outname, purpose);
}

static void
handle_execve(pid_t pid, PROCESS_INFO *pi, int dirfd, char *path)
{
    record_process_start(pid, pi->outname);

    char *abspath = absolutepath(pid, dirfd, path);

    if (!abspath) {
	if (errno != ENOENT) {
	    fprintf(stderr, "tracer.c:handle_execve:absolutepath(%s): %s\n",
		    path, strerror(errno));
	    exit(EXIT_FAILURE);
	}

	abspath = find_in_path(path);

	if (!abspath) {
	    fprintf(stderr, "tracer.c:handle_execve:find_in_path(%s): %s\n",
		    path, strerror(errno));
	    exit(EXIT_FAILURE);
	}
    }

    char *hash = get_file_hash(abspath);
    size_t sz = get_file_size(abspath);

    FILE_INFO *f;

    if (!(f = find_finfo(abspath, hash))) {
	f = next_finfo();

	finfo_new(f, path, abspath, hash, sz);
	record_file(f->outname, path, abspath);
	record_hash(f->outname, f->hash);
	record_size(f->outname, sz);
    } else {
	free(abspath);
	free(hash);
	free(path);
    }

    record_exec(pi->outname, f->outname);
}

static void
handle_rename_entry(pid_t pid, PROCESS_INFO *pi, int olddirfd, char *oldpath)
{
    pi->entry_info = absolutepath(pid, olddirfd, oldpath);
    free(oldpath);
}

static void
handle_rename_exit(pid_t pid, PROCESS_INFO *pi, char *oldpath, int newdirfd,
		   char *newpath)
{
    char *oldabspath = pi->entry_info;
    char *newabspath = absolutepath(pid, newdirfd, newpath);
    size_t sz = get_file_size(newabspath);

    char *hash = get_file_hash(newabspath);

    FILE_INFO *from = find_finfo(oldabspath, hash);

    if (!from) {
	from = next_finfo();
	finfo_new(from, oldpath, oldabspath, hash, sz);
	record_file(from->outname, oldpath, oldabspath);
	record_hash(from->outname, hash);
	record_size(from->outname, sz);
    } else {
	free(oldpath);
	free(oldabspath);
    }

    FILE_INFO *to = find_finfo(newabspath, hash);

    if (!to) {
	to = next_finfo();
	finfo_new(to, newpath, newabspath, hash, sz);
	record_file(to->outname, newpath, newabspath);
	record_hash(to->outname, hash);
	record_size(to->outname, sz);
    } else {
	free(newpath);
	free(newabspath);
	if (from->hash != hash) {
	    free(hash);
	}
    }

    record_rename(pi->outname, from->outname, to->outname);
}

static void
handle_syscall_entry(pid_t pid, PROCESS_INFO *pi,
		     const struct ptrace_syscall_info *entry)
{
    int olddirfd;
    char *oldpath;

    switch (entry->entry.nr) {
#ifdef HAVE_SYS_RENAME
	case SYS_rename:
	    // int rename(const char *oldpath, const char *newpath);
	    oldpath = get_str_from_process(pid, (void *) entry->entry.args[0]);
	    handle_rename_entry(pid, pi, AT_FDCWD, oldpath);
	    break;
#endif
#ifdef HAVE_SYS_RENAMEAT
	case SYS_renameat:
	    // int renameat(int olddirfd, const char *oldpath, int newdirfd,
	    // const char *newpath);
	    olddirfd = entry->entry.args[0];
	    oldpath = get_str_from_process(pid, (void *) entry->entry.args[1]);
	    handle_rename_entry(pid, pi, olddirfd, oldpath);
	    break;
#endif
#ifdef HAVE_SYS_RENAMEAT2
	case SYS_renameat2:
	    // int renameat2(int olddirfd, const char *oldpath, int newdirfd,
	    // const char *newpath, unsigned int flags);
	    olddirfd = entry->entry.args[0];
	    oldpath = get_str_from_process(pid, (void *) entry->entry.args[1]);
	    handle_rename_entry(pid, pi, olddirfd, oldpath);
	    break;
#endif
#ifdef HAVE_SYS_EXECVE
	case SYS_execve:
	    pi->entry_info =
		    get_str_from_process(pid, (void *) entry->entry.args[0]);
	    break;
#endif
#ifdef HAVE_SYS_EXECVEAT
	case SYS_execveat:
	    pi->entry_info =
		    get_str_from_process(pid, (void *) entry->entry.args[1]);
	    break;
#endif
    }
}

static void
handle_syscall_exit(pid_t pid, PROCESS_INFO *pi,
		    const struct ptrace_syscall_info *entry,
		    const struct ptrace_syscall_info *exit)
{
    if (exit->exit.rval < 0) {
	return;			       // return on syscall failure
    }

    int fd;
    void *path;
    int flags;
    int dirfd;
    FILE_INFO *f;
    char *oldpath;
    int newdirfd;
    char *newpath;

    switch (entry->entry.nr) {
#ifdef HAVE_SYS_OPEN
	case SYS_open:
	    // int open(const char *pathname, int flags, ...);
	    fd = (int) exit->exit.rval;
	    path = (void *) entry->entry.args[0];
	    flags = (int) entry->entry.args[1];

	    handle_open(pid, pi, fd, AT_FDCWD, path, flags);
	    break;
#endif
#ifdef HAVE_SYS_CREAT
	case SYS_creat:
	    // int creat(const char *pathname, ...);
	    fd = (int) exit->exit.rval;
	    path = (void *) entry->entry.args[0];

	    handle_open(pid, pi, fd, AT_FDCWD, path,
			O_CREAT | O_WRONLY | O_TRUNC);
	    break;
#endif
#ifdef HAVE_SYS_OPENAT
	case SYS_openat:
	    // int openat(int dirfd, const char *pathname, int flags, ...);
	    fd = (int) exit->exit.rval;
	    dirfd = (int) entry->entry.args[0];
	    path = (void *) entry->entry.args[1];
	    flags = (int) entry->entry.args[2];

	    handle_open(pid, pi, fd, dirfd, path, flags);
	    break;
#endif
#ifdef HAVE_SYS_CLOSE
	case SYS_close:
	    // int close(int fd);
	    fd = (int) entry->entry.args[0];

	    f = pinfo_find_finfo(pi, fd);

	    if (f != NULL) {
		f->hash = get_file_hash(f->abspath);
		f->size = get_file_size(f->abspath);

		record_hash(f->outname, f->hash);
		record_size(f->outname, f->size);

		// Add it to global cache list
		*next_finfo() = *f;

		// Remove the file from the process' list
		for (int i = f - pi->finfo; i < pi->numfinfo; ++i) {
		    pi->finfo[i] = pi->finfo[i + 1];
		}

		for (int i = f - pi->finfo; i < pi->numfinfo; ++i) {
		    pi->fds[i] = pi->fds[i + 1];
		}

		--pi->numfinfo;
	    }
	    break;
#endif
#ifdef HAVE_SYS_EXECVE
	case SYS_execve:
	    // int execve(const char *pathname, char *const argv[],
	    // char *const envp[]);
	    path = pi->entry_info;

	    handle_execve(pid, pi, AT_FDCWD, path);
	    break;
#endif
#ifdef HAVE_SYS_EXECVEAT
	case SYS_execveat:
	    // int execveat(int dirfd, const char *pathname,
	    // const char *const argv[], const char * const envp[],
	    // int flags);
	    dirfd = entry->entry.args[0];
	    path = pi->entry_info;

	    handle_execve(pid, pi, dirfd, path);
	    break;
#endif
#ifdef HAVE_SYS_RENAME
	case SYS_rename:
	    // int rename(const char *oldpath, const char *newpath);
	    oldpath = get_str_from_process(pid, (void *) entry->entry.args[0]);
	    newpath = get_str_from_process(pid, (void *) entry->entry.args[1]);

	    handle_rename_exit(pid, pi, oldpath, AT_FDCWD, newpath);
	    break;
#endif
#ifdef HAVE_SYS_RENAMEAT
	case SYS_renameat:
	    // int renameat(int olddirfd, const char *oldpath, int newdirfd,
	    // const char *newpath);
	    oldpath = get_str_from_process(pid, (void *) entry->entry.args[1]);
	    newdirfd = entry->entry.args[2];
	    newpath = get_str_from_process(pid, (void *) entry->entry.args[3]);

	    handle_rename_exit(pid, pi, oldpath, newdirfd, newpath);
	    break;
#endif
#ifdef HAVE_SYS_RENAMEAT2
	case SYS_renameat2:
	    // int renameat2(int olddirfd, const char *oldpath, int newdirfd,
	    // const char *newpath, unsigned int flags);
	    oldpath = get_str_from_process(pid, (void *) entry->entry.args[1]);
	    newdirfd = entry->entry.args[2];
	    newpath = get_str_from_process(pid, (void *) entry->entry.args[3]);

	    handle_rename_exit(pid, pi, oldpath, newdirfd, newpath);
	    break;
#endif
    }
}

static void
tracer_main(pid_t pid, PROCESS_INFO *pi, char *path, char **envp)
{
    waitpid(pid, NULL, 0);

    record_process_env(pi->outname, envp);
    handle_execve(pid, pi, AT_FDCWD, path);

    ptrace(PTRACE_SETOPTIONS, pid, NULL,	// Options are inherited
	   PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE |
	   PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK);

    struct ptrace_syscall_info info;
    static size_t running = 1;

    int status;
    PROCESS_INFO *process_state;

    // Starting tracee
    if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) < 0) {
	perror("tracer.c:tracer_main():ptrace(tracee PTRACE_SYSCALL)");
	exit(EXIT_FAILURE);
    }

    while (running) {
	pid = wait(&status);

	if (pid < 0) {
	    perror("tracer.c:tracer_main():wait()");
	    exit(EXIT_FAILURE);
	}

	unsigned int restart_sig = 0;

	if (WIFSTOPPED(status)) {
	    switch (WSTOPSIG(status)) {
		case SIGTRAP | 0x80:
		    process_state = find_pinfo(pid);
		    if (!process_state) {
			fprintf(stderr,
				"tracer.c:tracer_main():find_pinfo() on syscall sigtrap\n");
			exit(EXIT_FAILURE);
		    }

		    if (ptrace
			(PTRACE_GET_SYSCALL_INFO, pid, (void *) sizeof (info),
			 &info) < 0) {
			perror("tracer.c:tracer_main():ptrace(PTRACE_GET_SYSCALL_INFO)");
			exit(EXIT_FAILURE);
		    }

		    switch (info.op) {
			case PTRACE_SYSCALL_INFO_ENTRY:
			    process_state->state = info;
			    handle_syscall_entry(pid, process_state, &info);
			    break;
			case PTRACE_SYSCALL_INFO_EXIT:
			    handle_syscall_exit(pid, process_state,
						&process_state->state, &info);
			    break;
			default:
			    fprintf(stderr,
				    "tracer.c:tracer_main():WSTOPSIG(%d) expected PTRACE_SYSCALL_INFO_ENTRY or PTRACE_SYSCALL_INFOO_EXIT: %s\n",
				    WSTOPSIG(status), strerror(errno));
			    exit(EXIT_FAILURE);
		    }

		    break;
		case SIGSTOP:
		    // We only want to ignore post-attach SIGSTOP, for the
		    // rest we shouldn't mess with.
		    if ((process_state = find_pinfo(pid))) {
			if (process_state->ignore_one_sigstop == 0) {
			    restart_sig = WSTOPSIG(status);
			} else {
			    ++running;
			    process_state->ignore_one_sigstop = 0;
			}
		    } else {
			++running;
			PROCESS_INFO *pi = next_pinfo(pid);

			pinfo_new(pi, 0);
		    }
		    break;
		case SIGTRAP:
		    // Also ignore SIGTRAPs since they are
		    // generated by ptrace(2)
		    switch (status >> 8) {
			case SIGTRAP | (PTRACE_EVENT_VFORK << 8):
			case SIGTRAP | (PTRACE_EVENT_FORK << 8):
			case SIGTRAP | (PTRACE_EVENT_CLONE << 8):
			    pid_t child;

			    if (ptrace(PTRACE_GETEVENTMSG, pid, NULL, &child) <
				0) {
				perror("tracer.c:tracer_main():ptrace(PTRACE_GETEVENTMSG)");
				exit(EXIT_FAILURE);
			    }

			    PROCESS_INFO *child_pi = find_pinfo(child);

			    if (!child_pi) {
				child_pi = next_pinfo(child);
				pinfo_new(child_pi, 1);
			    }
			    record_process_create(pi->outname,
						  child_pi->outname);
		    }
		    break;
		default:
		    restart_sig = WSTOPSIG(status);
	    }

	    // Restarting process 
	    if (ptrace(PTRACE_SYSCALL, pid, NULL, restart_sig) < 0) {
		perror("tracer.c:tracer_main():ptrace(): failed restarting process");
		exit(EXIT_FAILURE);
	    }
	} else if (WIFEXITED(status)) {	// child process exited 
	    --running;

	    process_state = find_pinfo(pid);
	    if (!process_state) {
		fprintf(stderr,
			"tracer.c:tracer_main():find_pinfo on WIFEXITED\n");
		exit(EXIT_FAILURE);
	    }

	    record_process_end(process_state->outname);

	    free(process_state->cmd_line);
	    free(process_state->finfo);
	    free(process_state->fds);

	    for (int i = process_state - pinfo; i < numpinfo; ++i) {
		pinfo[i] = pinfo[i + 1];
	    }
	    for (int i = process_state - pinfo; i < numpinfo; ++i) {
		pids[i] = pids[i + 1];
	    }
	    --numpinfo;
	}
    }
}

void
trace(pid_t pid, char *path, char **envp)
{
    PROCESS_INFO *pi;

    pi = next_pinfo(pid);

    pinfo_new(pi, 0);

    tracer_main(pid, pi, path, envp);
}

void
run_tracee(char **av)
{
    ptrace(PTRACE_TRACEME, NULL, NULL, NULL);
    execvp(*av, av);
    perror("tracer.c:run_tracee():execvp(): after child exec()");
    exit(EXIT_FAILURE);
}

void
run_and_record_fnames(char **av, char **envp)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
	perror("tracer.c:run_and_record_fnames(): in original fork()");
	exit(EXIT_FAILURE);
    } else if (pid == 0)
	run_tracee(av);

    init();
    trace(pid, *av, envp);
}

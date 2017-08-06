/*
 * Copyright (C) 2014-2017 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "fseccomp.h"
#include "../include/seccomp.h"
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/types.h>

static void add_default_list(int fd, int allow_debuggers) {
	int r;
	if (!allow_debuggers)
		r = syscall_check_list("@default-nodebuggers", filter_add_blacklist, fd, 0);
	else
		r = syscall_check_list("@default", filter_add_blacklist, fd, 0);

	assert(r == 0);
//#ifdef SYS_mknod - emoved in 0.9.29 - it breaks Zotero extension
//		filter_add_blacklist(SYS_mknod, 0);
//#endif
// breaking Firefox nightly when playing youtube videos
// TODO: test again when firefox sandbox is finally released
//#ifdef SYS_get_mempolicy
//	filter_add_blacklist(fd, SYS_get_mempolicy, 0);
//#endif
//#ifdef SYS_quotactl - in use by Firefox
//	filter_add_blacklist(fd, SYS_quotactl, 0);
//#endif
}

// default list
void seccomp_default(const char *fname, int allow_debuggers) {
	assert(fname);

	// open file
	int fd = open(fname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Error fseccomp: cannot open %s file\n", fname);
		exit(1);
	}

	// build filter
	filter_init(fd);
	add_default_list(fd, allow_debuggers);
	filter_end_blacklist(fd);

	// close file
	close(fd);
}

// drop list
void seccomp_drop(const char *fname, char *list, int allow_debuggers) {
	assert(fname);
	(void) allow_debuggers; // todo: to implemnet it

	// open file
	int fd = open(fname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Error fseccomp: cannot open %s file\n", fname);
		exit(1);
	}

	// build filter
	filter_init(fd);
	if (syscall_check_list(list, filter_add_blacklist, fd, 0)) {
		fprintf(stderr, "Error fseccomp: cannot build seccomp filter\n");
		exit(1);
	}
	filter_end_blacklist(fd);

	// close file
	close(fd);
}

// default+drop
void seccomp_default_drop(const char *fname, char *list, int allow_debuggers) {
	assert(fname);

	// open file
	int fd = open(fname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Error fseccomp: cannot open %s file\n", fname);
		exit(1);
	}

	// build filter
	filter_init(fd);
	add_default_list(fd, allow_debuggers);
	if (syscall_check_list(list, filter_add_blacklist, fd, 0)) {
		fprintf(stderr, "Error fseccomp: cannot build seccomp filter\n");
		exit(1);
	}
	filter_end_blacklist(fd);

	// close file
	close(fd);
}

void seccomp_keep(const char *fname, char *list) {
	// open file
	int fd = open(fname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Error fseccomp: cannot open %s file\n", fname);
		exit(1);
	}

	// build filter
	filter_init(fd);
	// these syscalls are used by firejail after the seccomp filter is initialized
	int r;
	r = syscall_check_list("@default-keep", filter_add_whitelist, fd, 0);
	assert(r == 0);

	if (syscall_check_list(list, filter_add_whitelist, fd, 0)) {
		fprintf(stderr, "Error fseccomp: cannot build seccomp filter\n");
		exit(1);
	}

	filter_end_whitelist(fd);

	// close file
	close(fd);
}

void memory_deny_write_execute(const char *fname) {
	// open file
	int fd = open(fname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Error fseccomp: cannot open %s file\n", fname);
		exit(1);
	}

	filter_init(fd);

	// build filter
	static const struct sock_filter filter[] = {
#ifndef __x86_64__
		// block old multiplexing mmap syscall for i386
		BLACKLIST(SYS_mmap),
#endif
		// block mmap(,,x|PROT_WRITE|PROT_EXEC) so W&X memory can't be created
#ifndef __x86_64__
		// mmap2 is used for mmap on i386 these days
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_mmap2, 0, 5),
#else
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_mmap, 0, 5),
#endif
		EXAMINE_ARGUMENT(2),
		BPF_STMT(BPF_ALU+BPF_AND+BPF_K, PROT_WRITE|PROT_EXEC),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, PROT_WRITE|PROT_EXEC, 0, 1),
		KILL_PROCESS,
		RETURN_ALLOW,
		// block mprotect(,,PROT_EXEC) so writable memory can't be turned into executable
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_mprotect, 0, 5),
		EXAMINE_ARGUMENT(2),
		BPF_STMT(BPF_ALU+BPF_AND+BPF_K, PROT_EXEC),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, PROT_EXEC, 0, 1),
		KILL_PROCESS,
		RETURN_ALLOW,
		// block shmat(,,x|SHM_EXEC) so W&X shared memory can't be created
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SYS_shmat, 0, 5),
		EXAMINE_ARGUMENT(2),
		BPF_STMT(BPF_ALU+BPF_AND+BPF_K, SHM_EXEC),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, SHM_EXEC, 0, 1),
		KILL_PROCESS,
		RETURN_ALLOW
	};
	write_to_file(fd, filter, sizeof(filter));

	filter_end_blacklist(fd);

	// close file
	close(fd);
}

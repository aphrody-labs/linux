// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for BUN_ACCEL_IOC_COPY_BATCH on /dev/bun_accel.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/bun_accel.h>

#include "../kselftest_harness.h"

FIXTURE(copy_batch)
{
	int dev;
	int src;
	int dst;
	char src_path[64];
	char dst_path[64];
};

FIXTURE_SETUP(copy_batch)
{
	strcpy(self->src_path, "/tmp/bun_accel_src_XXXXXX");
	strcpy(self->dst_path, "/tmp/bun_accel_dst_XXXXXX");
	ASSERT_NE(NULL, mkdtemp(self->src_path));
	ASSERT_NE(NULL, mkdtemp(self->dst_path));

	self->dev = open("/dev/bun_accel", O_RDONLY | O_CLOEXEC);
	if (self->dev < 0)
		SKIP(return, "/dev/bun_accel: %s", strerror(errno));

	self->src = open(self->src_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	self->dst = open(self->dst_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	ASSERT_GE(self->src, 0);
	ASSERT_GE(self->dst, 0);
}

FIXTURE_TEARDOWN(copy_batch)
{
	char cmd[160];

	if (self->dev >= 0) {
		close(self->dev);
		close(self->src);
		close(self->dst);
	}
	snprintf(cmd, sizeof(cmd), "rm -rf %s %s", self->src_path, self->dst_path);
	if (system(cmd))
		TH_LOG("cleanup failed");
}

static void write_file(int dir, const char *name, const char *data)
{
	int fd = openat(dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd < 0 || write(fd, data, strlen(data)) != (ssize_t)strlen(data))
		abort();
	close(fd);
}

static int run(int dev, struct bun_accel_copy *entries, __u32 count,
	       __u32 *done)
{
	struct bun_accel_batch batch = {
		.entries = (__u64)(uintptr_t)entries,
		.count = count,
	};
	int ret = ioctl(dev, BUN_ACCEL_IOC_COPY_BATCH, &batch);

	*done = batch.done;
	return ret;
}

TEST_F(copy_batch, copies_files_and_reports_each_result)
{
	struct bun_accel_copy entries[3] = {
		{ .src_dirfd = self->src, .dst_dirfd = self->dst,
		  .src_path = (__u64)(uintptr_t)"a", .dst_path = (__u64)(uintptr_t)"a",
		  .mode = 0644 },
		{ .src_dirfd = self->src, .dst_dirfd = self->dst,
		  .src_path = (__u64)(uintptr_t)"missing", .dst_path = (__u64)(uintptr_t)"b",
		  .mode = 0644 },
		{ .src_dirfd = self->src, .dst_dirfd = self->dst,
		  .src_path = (__u64)(uintptr_t)"a", .dst_path = (__u64)(uintptr_t)"a",
		  .mode = 0644 },
	};
	char buf[16] = {};
	__u32 done = 0;
	int fd;

	write_file(self->src, "a", "hello bun");
	ASSERT_EQ(0, run(self->dev, entries, 3, &done));
	EXPECT_EQ(3, done);
	EXPECT_EQ(9, entries[0].result);
	EXPECT_EQ(-ENOENT, entries[1].result);
	EXPECT_EQ(-EEXIST, entries[2].result);

	fd = openat(self->dst, "a", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	EXPECT_EQ(9, read(fd, buf, sizeof(buf)));
	EXPECT_STREQ("hello bun", buf);
	close(fd);
}

TEST_F(copy_batch, source_mode_copies_permission_bits)
{
	struct bun_accel_copy entry = {
		.src_dirfd = self->src, .dst_dirfd = self->dst,
		.src_path = (__u64)(uintptr_t)"run.sh",
		.dst_path = (__u64)(uintptr_t)"run.sh",
		.flags = BUN_ACCEL_SOURCE_MODE,
	};
	struct stat st;
	__u32 done = 0;
	mode_t mask = umask(022);

	write_file(self->src, "run.sh", "#!/bin/sh\n");
	ASSERT_EQ(0, fchmodat(self->src, "run.sh", 0755, 0));
	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	umask(mask);
	EXPECT_EQ(10, entry.result);
	ASSERT_EQ(0, fstatat(self->dst, "run.sh", &st, 0));
	EXPECT_EQ(0755, st.st_mode & 07777);

	entry.dst_path = (__u64)(uintptr_t)"other.sh";
	entry.mode = 0644;
	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	EXPECT_EQ(-EINVAL, entry.result);
}

TEST_F(copy_batch, names_stay_beneath_their_directory)
{
	struct bun_accel_copy entry = {
		.src_dirfd = self->src, .dst_dirfd = self->dst,
		.src_path = (__u64)(uintptr_t)"../../../../etc/hostname",
		.dst_path = (__u64)(uintptr_t)"escaped", .mode = 0644,
	};
	__u32 done = 0;

	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	EXPECT_EQ(-ENOENT, entry.result);
}

TEST_F(copy_batch, symlinks_are_not_followed)
{
	struct bun_accel_copy entry = {
		.src_dirfd = self->src, .dst_dirfd = self->dst,
		.src_path = (__u64)(uintptr_t)"link",
		.dst_path = (__u64)(uintptr_t)"link", .mode = 0644,
	};
	__u32 done = 0;

	write_file(self->src, "target", "x");
	ASSERT_EQ(0, symlinkat("target", self->src, "link"));
	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	EXPECT_EQ(-ELOOP, entry.result);
}

TEST_F(copy_batch, rejects_invalid_batches)
{
	struct bun_accel_copy entry = {
		.src_dirfd = self->src, .dst_dirfd = self->dst,
		.src_path = (__u64)(uintptr_t)"a", .dst_path = (__u64)(uintptr_t)"c",
		.mode = 0644, .flags = 1U << 7,
	};
	struct bun_accel_batch batch = { .count = BUN_ACCEL_MAX_BATCH + 1 };
	__u32 done = 0;

	EXPECT_EQ(-1, ioctl(self->dev, BUN_ACCEL_IOC_COPY_BATCH, &batch));
	EXPECT_EQ(EINVAL, errno);

	write_file(self->src, "a", "x");
	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	EXPECT_EQ(-EINVAL, entry.result);

	entry.flags = 0;
	entry.src_dirfd = -1;
	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	EXPECT_EQ(-EBADF, entry.result);
}

TEST_F(copy_batch, special_files_are_rejected)
{
	struct bun_accel_copy entry = {
		.src_dirfd = self->src, .dst_dirfd = self->dst,
		.src_path = (__u64)(uintptr_t)"fifo",
		.dst_path = (__u64)(uintptr_t)"fifo", .mode = 0644,
	};
	__u32 done = 0;

	ASSERT_EQ(0, mkfifoat(self->src, "fifo", 0644));
	ASSERT_EQ(0, run(self->dev, &entry, 1, &done));
	EXPECT_EQ(-EINVAL, entry.result);
	EXPECT_EQ(-1, faccessat(self->dst, "fifo", F_OK, 0));
}

#define NOBODY 65534

/*
 * Runs in a child that dropped root but kept the directory and device
 * descriptors root opened: holding them must not grant root's rights.
 */
static int copy_as_nobody(int dev, int src, int dst)
{
	struct bun_accel_copy entries[3] = {
		/* A root-only source cannot be read. */
		{ .src_dirfd = src, .dst_dirfd = dst,
		  .src_path = (__u64)(uintptr_t)"secret",
		  .dst_path = (__u64)(uintptr_t)"out/stolen", .mode = 0644 },
		/* A root-owned directory cannot receive a file. */
		{ .src_dirfd = src, .dst_dirfd = dst,
		  .src_path = (__u64)(uintptr_t)"public",
		  .dst_path = (__u64)(uintptr_t)"planted", .mode = 0644 },
		/* An existing root file is never overwritten. */
		{ .src_dirfd = src, .dst_dirfd = dst,
		  .src_path = (__u64)(uintptr_t)"public",
		  .dst_path = (__u64)(uintptr_t)"out/root_file", .mode = 0644 },
	};
	struct bun_accel_batch batch = {
		.entries = (__u64)(uintptr_t)entries,
		.count = 3,
	};

	if (setgroups(0, NULL) || setresgid(NOBODY, NOBODY, NOBODY) ||
	    setresuid(NOBODY, NOBODY, NOBODY))
		return 10;
	if (ioctl(dev, BUN_ACCEL_IOC_COPY_BATCH, &batch) || batch.done != 3)
		return 11;
	if (entries[0].result != -EACCES)
		return 1;
	if (entries[1].result != -EACCES)
		return 2;
	if (entries[2].result != -EEXIST)
		return 3;
	return 0;
}

TEST_F(copy_batch, unprivileged_user_cannot_write_root_files)
{
	struct stat st;
	int status;
	pid_t pid;
	int fd;

	if (geteuid() != 0)
		SKIP(return, "needs root to drop privileges");

	write_file(self->src, "secret", "root only");
	ASSERT_EQ(0, fchmodat(self->src, "secret", 0600, 0));
	write_file(self->src, "public", "shared");
	ASSERT_EQ(0, fchmod(self->src, 0755));
	ASSERT_EQ(0, fchmod(self->dst, 0755));
	ASSERT_EQ(0, mkdirat(self->dst, "out", 0755));
	ASSERT_EQ(0, fchownat(self->dst, "out", NOBODY, NOBODY, 0));
	fd = openat(self->dst, "out/root_file", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	ASSERT_GE(fd, 0);
	close(fd);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
		_exit(copy_as_nobody(self->dev, self->src, self->dst));
	ASSERT_EQ(pid, waitpid(pid, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	EXPECT_EQ(-1, faccessat(self->dst, "out/stolen", F_OK, 0));
	EXPECT_EQ(-1, faccessat(self->dst, "planted", F_OK, 0));
	ASSERT_EQ(0, fstatat(self->dst, "out/root_file", &st, 0));
	EXPECT_EQ(0, st.st_uid);
	EXPECT_EQ(0, st.st_size);
}

TEST_HARNESS_MAIN

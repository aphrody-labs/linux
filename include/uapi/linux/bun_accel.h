/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Batched file copies for package managers (/dev/bun_accel).
 */
#ifndef _UAPI_LINUX_BUN_ACCEL_H
#define _UAPI_LINUX_BUN_ACCEL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Fail with -EOPNOTSUPP or -EXDEV instead of copying the data. */
#define BUN_ACCEL_CLONE_ONLY	(1U << 0)
/* Create the destination with the permission bits of the source; @mode must be 0. */
#define BUN_ACCEL_SOURCE_MODE	(1U << 1)

/*
 * One copy: open @src_path beneath @src_dirfd, create @dst_path beneath
 * @dst_dirfd with O_CREAT | O_EXCL and @mode (minus the umask), then copy
 * the whole file. Each directory acts as the root of its name, so ".." and
 * absolute symbolic links stop there; the last component must not be a
 * symbolic link and the source must be a regular file (-EINVAL otherwise).
 * Lookups, opens and the copy use the caller's credentials, as openat(2)
 * and copy_file_range(2) would.
 *
 * @result receives the number of bytes copied or a negative errno. A
 * destination created before a copy error is left in place.
 */
struct bun_accel_copy {
	__s32 src_dirfd;
	__s32 dst_dirfd;
	__aligned_u64 src_path;
	__aligned_u64 dst_path;
	__u32 mode;
	__u32 flags;
	__s64 result;
};

/*
 * @entries points to @count struct bun_accel_copy. @done receives the
 * number of entries processed; it is smaller than @count when a signal
 * interrupted the batch. @flags and @reserved must be zero.
 */
struct bun_accel_batch {
	__aligned_u64 entries;
	__u32 count;
	__u32 flags;
	__u32 done;
	__u32 reserved;
};

#define BUN_ACCEL_MAX_BATCH	1024

#define BUN_ACCEL_IOC_COPY_BATCH	_IOWR(0xB9, 0x01, struct bun_accel_batch)

#endif /* _UAPI_LINUX_BUN_ACCEL_H */

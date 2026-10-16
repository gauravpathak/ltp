// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 SUSE LLC <gaurav.pathak@suse.com>
 */

/*\
 * This test validates the STATX_WRITE_ATOMIC feature (introduced in Linux 6.13).
 * It ensures that supported filesystems (xfs and ext4) correctly report their
 * atomic write limits to user space when queried via statx().
 *
 * Minimum Linux version required is v6.12.
 */

#define _GNU_SOURCE
#include "tst_test.h"
#include "lapi/stat.h"

#define MNTPOINT "mnt_point"
#define TESTFILE MNTPOINT "/testfile"
#define MODE 0644

static int file_fd = -1;
static struct ltp_statx *buff;

static void verify_statx(void)
{
	TST_EXP_PASS(statx(AT_FDCWD, TESTFILE, 0, STATX_BASIC_STATS | STATX_WRITE_ATOMIC, &buff->buff),
			"statx(AT_FDCWD, %s, 0, STATX_BASIC_STATS | STATX_WRITE_ATOMIC, &buf)", TESTFILE);

	if (!TST_PASS)
		return;

	/* Check if the kernel populated the atomic limit struct members */
	if (!(buff->data.stx_mask & STATX_WRITE_ATOMIC))
		tst_brk(TCONF, "Kernel may be too old to report atomic limits: STATX_WRITE_ATOMIC missing from stx_mask");

	/* Check if the filesystem supports reporting the atomic write attribute */
	if (!(buff->data.stx_attributes_mask & STATX_ATTR_WRITE_ATOMIC)) {
		tst_res(TFAIL, "Inconsistent: stx_mask has STATX_WRITE_ATOMIC but stx_attributes_mask lacks STATX_ATTR_WRITE_ATOMIC");
		return;
	}

	/* Check if the file/device actively supports atomic writes */
	if (!(buff->data.stx_attributes & STATX_ATTR_WRITE_ATOMIC)) {
		if (buff->data.stx_atomic_write_unit_min || buff->data.stx_atomic_write_unit_max ||
				buff->data.stx_atomic_write_unit_max_opt || buff->data.stx_atomic_write_segments_max) {
			tst_res(TFAIL,
				"Inconsistent: STATX_ATTR_WRITE_ATOMIC unset but limits nonzero (min=%u max=%u opt=%u segs=%u)",
				buff->data.stx_atomic_write_unit_min, buff->data.stx_atomic_write_unit_max,
				buff->data.stx_atomic_write_unit_max_opt, buff->data.stx_atomic_write_segments_max);
			return;
		}

		tst_brk(TCONF, "STATX_WRITE_ATOMIC unsupported by block dev or filesystem: STATX_ATTR_WRITE_ATOMIC missing from stx_attributes");
	}

	TST_EXP_EXPR(buff->data.stx_atomic_write_unit_min <= buff->data.stx_atomic_write_unit_max,
			"stx_atomic_write_unit_min(%u) <= stx_atomic_write_unit_max(%u)",
			buff->data.stx_atomic_write_unit_min, buff->data.stx_atomic_write_unit_max);

	TST_EXP_EXPR(buff->data.stx_atomic_write_segments_max > 0,
			"stx_atomic_write_segments_max(%u) > 0", buff->data.stx_atomic_write_segments_max);

	TST_EXP_EXPR(buff->data.stx_atomic_write_unit_min > 0 &&
			__builtin_popcount(buff->data.stx_atomic_write_unit_min) == 1,
			"stx_atomic_write_unit_min(%u) is a power of 2",
			buff->data.stx_atomic_write_unit_min);

	TST_EXP_EXPR(buff->data.stx_atomic_write_unit_max > 0 &&
			__builtin_popcount(buff->data.stx_atomic_write_unit_max) == 1,
			"stx_atomic_write_unit_max(%u) is a power of 2",
			buff->data.stx_atomic_write_unit_max);

	if (buff->data.stx_atomic_write_unit_max_opt == 0) {
		tst_res(TPASS, "stx_atomic_write_unit_max_opt is 0 (no optimized max reported)");
	} else {
		if (buff->data.stx_atomic_write_unit_max_opt > buff->data.stx_atomic_write_unit_max)
			tst_res(TFAIL, "stx_atomic_write_unit_max_opt (%u) exceeds max (%u)",
					buff->data.stx_atomic_write_unit_max_opt,
					buff->data.stx_atomic_write_unit_max);

		else if (buff->data.stx_atomic_write_unit_max_opt < buff->data.stx_atomic_write_unit_min)
			tst_res(TFAIL, "stx_atomic_write_unit_max_opt (%u) is less than min (%u)",
					buff->data.stx_atomic_write_unit_max_opt,
					buff->data.stx_atomic_write_unit_min);
		else
			tst_res(TPASS, "stx_atomic_write_unit_max_opt (%u) is within valid range [%u, %u]",
					buff->data.stx_atomic_write_unit_max_opt,
					buff->data.stx_atomic_write_unit_min,
					buff->data.stx_atomic_write_unit_max);

		TST_EXP_EXPR(__builtin_popcount(buff->data.stx_atomic_write_unit_max_opt) == 1,
				"stx_atomic_write_unit_max_opt(%u) is a power of 2",
				buff->data.stx_atomic_write_unit_max_opt);
	}
}

static void setup(void)
{
	char *data_buff = NULL;
	struct stat sb;

	SAFE_STAT(MNTPOINT, &sb);

	data_buff = SAFE_MEMALIGN(sb.st_blksize, sb.st_blksize);
	memset(data_buff, '@', sb.st_blksize);

	file_fd = SAFE_OPEN(TESTFILE, O_RDWR | O_CREAT | O_DIRECT, MODE);
	SAFE_WRITE(SAFE_WRITE_ALL, file_fd, data_buff, sb.st_blksize);

	SAFE_CLOSE(file_fd);
	free(data_buff);
}

static struct tst_test test = {
	.test_all = verify_statx,
	.setup = setup,
	.min_kver = "6.11",
	.needs_root = 1,
	.mntpoint = MNTPOINT,
	.mount_device = 1,
	.bufs = (struct tst_buffers []) {
		{&buff, .size = sizeof(*buff)},
		{}
	},
	.filesystems = (struct tst_fs[]) {
		{
			.type = "xfs",
			.min_kver = "6.12",
			.mkfs_opts = (const char *const []){"-f", "-bsize=16K", NULL},
		},
		{
			.type = "ext4",
			.mkfs_opts = (const char *const []){"-O", "bigalloc", "-b", "4096", "-C", "65536", NULL},
		},
		{}
	},
};

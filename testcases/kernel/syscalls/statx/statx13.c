// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 SUSE LLC <gaurav.pathak@suse.com>
 */

/*\
 * Test :manpage:`statx(2)` support for the ``STATX_WRITE_ATOMIC`` mask on a
 * regular file.
 *
 * STATX_WRITE_ATOMIC was added in Linux v6.11, initially for block
 * devices. Regular-file reporting of atomic write limits on XFS and ext4
 * was added later, in Linux v6.13, which is what this test exercises.
 *
 * There are two independent ways a filesystem can end up reporting atomic
 * write support on a regular file, and this test exercises both:
 *
 * - Hardware-backed: the underlying block device itself advertises atomic
 *   write support (queried via statx(2) on the raw device before
 *   formatting). Both XFS and ext4 propagate the device's limits in this
 *   case. Loop devices never advertise this (the loop driver doesn't wire
 *   up atomic-write queue limits at all), so this path requires LTP_DEV to
 *   point at a real NVMe/SCSI device (or a scsi_debug-emulated one).
 *
 * - Software-backed (XFS only, Linux v6.16+): xfs_inode_can_sw_atomic_write()
 *   lets XFS complete an atomic write via CoW-based out-of-place writes
 *   instead of hardware support, gated purely by xfs_has_reflink() -- a
 *   filesystem feature, not a device capability. reflink is on by default
 *   for v5 (CRC-enabled) XFS filesystems, so a plain mkfs.xfs on top of a
 *   loop device will report working atomic write limits on a v6.16+
 *   kernel, with no special device needed. ext4 has no equivalent
 *   software fallback, so it always needs the hardware-backed path.
 *
 * When the test device does support hardware atomic writes, the
 * filesystem block size is derived at runtime from the device's own
 * atomic write unit limits, capped to 64KiB -- the hard block size
 * ceiling enforced by both mkfs.xfs and mkfs.ext4 regardless of kernel
 * capability -- and otherwise handled differently per filesystem:
 *
 * - XFS: without CONFIG_TRANSPARENT_HUGEPAGE, the block size is capped to
 *   the page size, since mkfs.xfs has no page-size pre-check and the
 *   kernel would otherwise reject a larger block size with ENOSYS at
 *   mount (mapping_max_folio_size_supported()). With THP available, the
 *   device's own atomic write max is used directly, except that a
 *   device reporting at least 64KiB is capped to exactly 16KiB rather
 *   than its full max, to exercise a non-default block size without
 *   nearing the 64KiB mkfs ceiling.
 *
 * - ext4: the block size always stays at or below the page size. A
 *   larger atomic write unit is instead obtained via a bigalloc cluster
 *   (ext4_atomic_write_init() reports stx_atomic_write_unit_max based on
 *   the cluster size, not the block size), since mke2fs refuses
 *   (without -F) a block size above the page size unless the kernel
 *   advertises /sys/fs/ext4/features/blocksize_gt_pagesize, while its
 *   cluster size (-C) has no such page-size restriction at all.
 *
 * When the test device does not support hardware atomic writes (e.g. the
 * default loop-backed image LTP creates when LTP_DEV isn't set), ext4
 * TCONFs immediately, while XFS is still formatted (with reflink forced
 * on) and mounted, relying on verify_statx() to TCONF on its own if the
 * running kernel predates the v6.16 software atomic write support.
 *
 * The test needs to run as root because it formats and mounts its own
 * XFS and ext4 test filesystems.
 */

#define _GNU_SOURCE
#include <sys/mount.h>
#include "tst_test.h"
#include "lapi/stat.h"

#define MNTPOINT "mnt_point"
#define TESTFILE MNTPOINT "/testfile"
#define MODE 0644

/*
 * Both xfsprogs (XFS_MAX_BLOCKSIZE) and e2fsprogs (EXT2_MAX_BLOCK_SIZE) cap
 * the mkfs block size at 64KiB regardless of kernel/THP capability; mkfs
 * rejects anything larger with "invalid block size" before the kernel is
 * even involved.
 */
#define MKFS_MAX_BLOCK_SIZE 65536

static int file_fd = -1;
static int mount_flag;
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

#define PATH_EXT4_BS_GT_PS "/sys/fs/ext4/features/blocksize_gt_pagesize"

static int has_thp(void)
{
	return access(PATH_MM_THP, F_OK) == 0;
}

/*
 * Whether the running kernel can use a filesystem block size larger than
 * PAGE_SIZE for the given fs_type.
 *
 * - xfs: gated by mapping_max_folio_size_supported(), which collapses to
 *   PAGE_SIZE unless CONFIG_TRANSPARENT_HUGEPAGE is compiled in (see
 *   xfs_fs_fill_super() in fs/xfs/xfs_super.c). mkfs.xfs itself has no
 *   page-size pre-check, so THP availability is the right/only signal.
 *
 * - ext4: gated by its own dedicated kernel feature, independent of THP.
 *   mke2fs itself refuses (without -F) a block size above the system page
 *   size unless /sys/fs/ext4/features/blocksize_gt_pagesize exists (see
 *   mke2fs.c), so that sysfs flag -- not THP -- is the correct signal.
 */
static int fs_supports_large_block(const char *fs_type)
{
	if (!strcmp(fs_type, "ext4"))
		return access(PATH_EXT4_BS_GT_PS, F_OK) == 0;

	return has_thp();
}

static void setup(void)
{
	struct ltp_statx dev_stx;
	char *data_buff = NULL;
	struct stat sb;
	char opt_bsize[32];
	char opt_cluster[32];
	const char *fs_opts[8];
	unsigned int min, max, bsize = 0;
	int dev_has_atomic;

	SAFE_STATX(AT_FDCWD, tst_device->dev, 0, STATX_WRITE_ATOMIC, &dev_stx);

	/*
	 * stx_mask only gains STATX_WRITE_ATOMIC when bdev_can_atomic_write()
	 * is true (see block/bdev.c:bdev_statx()), i.e. this is a device
	 * capability signal, not a kernel version one: .min_kver = "6.13"
	 * above already guarantees the kernel is new enough for the field to
	 * exist at all. So any of these three being unset simply means this
	 * particular device (most commonly a loop device) has no atomic
	 * write support to report.
	 */
	dev_has_atomic = (dev_stx.data.stx_mask & STATX_WRITE_ATOMIC) &&
		(dev_stx.data.stx_attributes_mask & STATX_ATTR_WRITE_ATOMIC) &&
		(dev_stx.data.stx_attributes & STATX_ATTR_WRITE_ATOMIC);

	if (!dev_has_atomic && strcmp(tst_device->fs_type, "xfs")) {
		/*
		 * Unlike XFS, ext4 has no device-independent (software)
		 * atomic write mechanism: ext4_atomic_write_init() always
		 * requires bdev_can_atomic_write() to be true. So without
		 * device support there is nothing for ext4 to exercise here.
		 */
		tst_brk(TCONF,
			"%s does not support atomic writes (loop devices never do) and ext4 has no software atomic write fallback; set LTP_DEV to a real NVMe/SCSI device, or load scsi_debug",
			tst_device->dev);
	}

	if (!dev_has_atomic) {
		/*
		 * No hardware atomic write support, but XFS can still
		 * complete atomic writes via CoW-based out-of-place writes
		 * (xfs_inode_can_sw_atomic_write()) as long as reflink is
		 * enabled, independently of the backing device -- this
		 * works even on a plain loop device, but only since Linux
		 * v6.16. Force reflink on explicitly rather than relying on
		 * mkfs.xfs defaults, and skip the device-limit-based block
		 * size selection below entirely since it doesn't apply here.
		 * If the running kernel predates v6.16, verify_statx() will
		 * TCONF on its own once it sees STATX_WRITE_ATOMIC missing.
		 */
		tst_res(TINFO,
			"%s has no atomic write support; relying on XFS's software (CoW-based, reflink) atomic writes instead (requires Linux v6.16+)",
			tst_device->dev);

		fs_opts[0] = "-f";
		fs_opts[1] = "-m";
		fs_opts[2] = "reflink=1";
		fs_opts[3] = NULL;

		SAFE_MKFS(tst_device->dev, tst_device->fs_type, fs_opts, NULL);
		goto mount_fs;
	}

	min = dev_stx.data.stx_atomic_write_unit_min;
	max = dev_stx.data.stx_atomic_write_unit_max;

	if (!min || !max)
		tst_brk(TCONF, "%s reports zero atomic write unit limits", tst_device->dev);

	if (!strcmp(tst_device->fs_type, "xfs")) {
		if (max >= 65536 && fs_supports_large_block(tst_device->fs_type))
			bsize = 16384;
		else if (fs_supports_large_block(tst_device->fs_type))
			bsize = max;
		else
			bsize = MIN(max, (unsigned int)getpagesize());
	} else {
		bsize = MIN(max, (unsigned int)getpagesize());
	}

	bsize = MIN(bsize, (unsigned int)MKFS_MAX_BLOCK_SIZE);

	if (bsize < min) {
		tst_brk(TCONF,
			"Device's minimum atomic write unit (%u) exceeds the largest block size this test can configure (%u)",
			min, bsize);
	}

	if (!strcmp(tst_device->fs_type, "xfs")) {
		tst_res(TINFO, "Device atomic write unit: [%u, %u], THP %s, using xfs block size %u",
				min, max, has_thp() ? "available" : "unavailable", bsize);

		snprintf(opt_bsize, sizeof(opt_bsize), "-bsize=%u", bsize);
		fs_opts[0] = "-f";
		fs_opts[1] = opt_bsize;
		fs_opts[2] = NULL;
	} else {
		unsigned int cluster = MIN(max, 16 * bsize);

		if (cluster < bsize)
			cluster = bsize;

		tst_res(TINFO, "Device atomic write unit: [%u, %u], using ext4 block size %u, cluster size %u",
				min, max, bsize, cluster);

		snprintf(opt_bsize, sizeof(opt_bsize), "%u", bsize);
		snprintf(opt_cluster, sizeof(opt_cluster), "%u", cluster);
		fs_opts[0] = "-O";
		fs_opts[1] = "bigalloc";
		fs_opts[2] = "-b";
		fs_opts[3] = opt_bsize;
		fs_opts[4] = "-C";
		fs_opts[5] = opt_cluster;
		fs_opts[6] = NULL;
	}

	SAFE_MKFS(tst_device->dev, tst_device->fs_type, fs_opts, NULL);

mount_fs:

	TEST(mount(tst_device->dev, MNTPOINT, tst_device->fs_type, 0, NULL));
	if (TST_RET) {
		if (TST_ERR == EINVAL || TST_ERR == ENOSYS || TST_ERR == EOPNOTSUPP) {
			if (bsize)
				tst_brk(TCONF | TERRNO, "mount() of %s with block size %u not supported",
						tst_device->fs_type, bsize);
			tst_brk(TCONF | TERRNO, "mount() of %s not supported",
					tst_device->fs_type);
		}
		tst_brk(TBROK | TERRNO, "mount(%s, %s, %s, 0, NULL) failed",
				tst_device->dev, MNTPOINT, tst_device->fs_type);
	}
	mount_flag = 1;

	SAFE_STAT(MNTPOINT, &sb);

	data_buff = SAFE_MEMALIGN(sb.st_blksize, sb.st_blksize);
	memset(data_buff, '@', sb.st_blksize);

	file_fd = SAFE_OPEN(TESTFILE, O_RDWR | O_CREAT | O_DIRECT, MODE);
	SAFE_WRITE(SAFE_WRITE_ALL, file_fd, data_buff, sb.st_blksize);

	SAFE_CLOSE(file_fd);
	free(data_buff);
}

static void cleanup(void)
{
	if (mount_flag)
		tst_umount(MNTPOINT);
}

static struct tst_test test = {
	.test_all = verify_statx,
	.setup = setup,
	.cleanup = cleanup,
	.min_kver = "6.13",
	.needs_root = 1,
	.needs_device = 1,
	.mntpoint = MNTPOINT,
	.bufs = (struct tst_buffers []) {
		{&buff, .size = sizeof(*buff)},
		{}
	},
	.filesystems = (struct tst_fs[]) {
		{.type = "xfs"},
		{.type = "ext4"},
		{}
	},
};

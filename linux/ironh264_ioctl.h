/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef IRONH264_IOCTL_H
#define IRONH264_IOCTL_H
#include <linux/ioctl.h>
#include <linux/types.h>

/* One completed packed-YUYV frame. The fd remains owned by the caller.
 * Storage includes complete 16-row macroblocks. The result is one Annex B
 * access unit with parameter sets at each IDR. All addresses are userspace. */
struct ironh264_frame {
	__s32 dmabuf_fd;
	__u32 width, height, stride;
	__aligned_u64 output;
	__u32 capacity, bytes, keyframe, reserved;
};
#define IRONH264_ENCODE _IOWR('H', 0x60, struct ironh264_frame)
#define IRONH264_MAX_AU (8U * 1024 * 1024 + 64U)
#endif

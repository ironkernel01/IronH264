# SPDX-License-Identifier: GPL-2.0-only
ironh264_dir := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
obj-y += ironh264.o
ironh264-y := ironh264_linux.o ironh264_registers.o
CFLAGS_ironh264_linux.o += -I$(ironh264_dir)/../ip/rtlnet_h264_enc/driver
$(obj)/ironh264_linux.o: $(ironh264_dir)/ironh264_linux.c FORCE
	$(call if_changed_dep,cc_o_c)
$(obj)/ironh264_registers.o: $(ironh264_dir)/../ip/rtlnet_h264_enc/driver/rtlnet_h264_enc.c FORCE
	$(call if_changed_dep,cc_o_c)

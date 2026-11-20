# tb_rtlnet_h264_mbcavlc sources, compile order (paths relative to the repository root).
-f cores/rtlnet_common/hdl/rtlnet_common.f
-f cores/rtlnet_common/testbench/rtlnet_tb_common.f
+incdir+ip/rtlnet_h264_enc/testbench
ip/rtlnet_h264_enc/hdl/rtlnet_h264_pkg.sv
ip/rtlnet_h264_enc/hdl/rtlnet_h264_cavlc_tab.sv
ip/rtlnet_h264_enc/hdl/rtlnet_h264_cavlc_blk.sv
ip/rtlnet_h264_enc/hdl/rtlnet_h264_mb_cavlc.sv
ip/rtlnet_h264_enc/testbench/tb_rtlnet_h264_mbcavlc.sv

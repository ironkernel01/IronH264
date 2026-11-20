# IronH264

IronH264 reads packed YUYV frames from DDR and writes an Annex B H.264 stream, picture indices and reconstructed reference pictures to DDR. The core, register driver, testbenches and source license live under `ip/rtlnet_h264_enc/`.

`ironh264_axi` presents one encoder engine with a 64-bit AXI4 master, AXI4-Lite control and a picture interrupt. Software supplies SPS and PPS using the register driver. The ordered source list is `ip/rtlnet_h264_enc/hdl/rtlnet_h264_enc.f`. References under `cores/rtlnet_common` resolve to RTLNet's shared primitives.

The default implements H.264 Constrained Baseline with CAVLC. The High Profile option accepts a separately supplied netlist matched to the target FPGA.

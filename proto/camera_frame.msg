# Camera JPEG frame for ULog recording
# Records JPEG-encoded camera frames to .ulg files on SD card.
#
# ESP32-S31 DVP path: OV3660 640x480 YUV422 YUYV → PPA (rotate 180°
# + downscale) → 320x240 YUV422 → HW JPEG encode (quality 45, YUV422 subsampling)
# → typically 4-10KB per frame. Buffer sized 10240 (10KB) to cover
# worst-case frames at 320x240 quality 45.
#
# Variable-length JPEG: jpeg_size indicates actual data length, jpeg_data[] is
# a fixed-size buffer that may be partially filled. ULog writer uses
# o_size_no_padding to avoid writing trailing _padding, but still writes the
# full jpeg_data[10240] array (including unused bytes).
# uORB queues are PSRAM-backed, so the 2×buffer cost no longer hits internal-SRAM.
uint64  timestamp          # Timestamp (µs since boot)
uint32  frame_index        # Sequential frame counter
uint16  width              # Image width (pixels, e.g. 320)
uint16  height             # Image height (pixels, e.g. 240)
uint16  jpeg_size          # Actual JPEG data length in jpeg_data[] (max 10240)
uint8   format             # Pixel format: 0=JPEG, 1=RGB565, 2=BGR888
uint8[10240] jpeg_data      # JPEG frame data (320x240 quality 30, ≤10KB)

uint8 ORB_QUEUE_LENGTH = 2

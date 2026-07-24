# Camera JPEG frame for ULog recording
# Records JPEG-encoded camera frames to .ulg files on SD card.
#
# ESP32-S31 DVP path: OV3660 640x480 RGB565 → HW JPEG encode (quality 10)
# → typically 5-12KB per frame. Buffer sized 15360 (15KB) to cover
# worst-case frames at 640x480 quality 10.
# NOTE: ULog DATA message msg_size field is uint16_t (max 65535 bytes),
# so total payload must fit in 16 bits. 15360 + header fits comfortably.
#
# Variable-length JPEG: jpeg_size indicates actual data length, jpeg_data[] is
# a fixed-size buffer that may be partially filled. ULog writer uses
# o_size_no_padding to avoid writing trailing _padding, but still writes the
# full jpeg_data[15360] array (including unused bytes).
# uORB queues are PSRAM-backed, so the 2×buffer cost no longer hits internal-SRAM.
uint64  timestamp          # Timestamp (µs since boot)
uint32  frame_index        # Sequential frame counter
uint16  width              # Image width (pixels)
uint16  height            # Image height (pixels)
uint16  jpeg_size          # Actual JPEG data length in jpeg_data[] (max 15360)
uint8   format            # Pixel format: 0=JPEG, 1=RGB565, 2=BGR888
uint8[15360] jpeg_data     # JPEG frame data (640x480 quality 10, ≤15KB)

uint8 ORB_QUEUE_LENGTH = 2

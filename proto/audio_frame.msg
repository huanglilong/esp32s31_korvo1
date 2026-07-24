# Audio frame for ULog continuous recording
# AAC-ADTS encoded audio data, continuously captured from I2S mic input.
# AAC encoder: 16kHz stereo 64kbps ADTS → ~512B/frame at ~15.6 fps.
# 1024B buffer gives 2× headroom over typical ~512B frames.
# Variable-length AAC: aac_size indicates actual data length, aac_data[] is fixed-size
# buffer that may be partially filled. ULog writer uses o_size_no_padding to avoid
# writing trailing _padding, but still writes the full aac_data[1024] array
# (including unused bytes). Keep buffer small to minimize ULog write overhead.
uint64  timestamp           # Timestamp (µs since boot)
uint32  frame_index         # Sequential frame counter
uint16  sample_rate         # Sample rate (e.g. 16000)
uint8   channel             # Channel count (1=mono, 2=stereo)
uint8   bits_per_sample     # Bits per sample (16)
uint16  aac_size            # Actual AAC data length in aac_data[] (max 1024)
uint8[1024] aac_data        # AAC-ADTS encoded audio data

uint8 ORB_QUEUE_LENGTH = 4

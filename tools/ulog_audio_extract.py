#!/usr/bin/env python3
"""
ulog_audio_extract.py — Extract AAC audio from PX4 ULog (.ulg) files.

Parses ULog binary format, finds audio_frame topic DATA messages,
extracts AAC-ADTS frames, and writes them to a .aac output file.

ADTS frames are self-contained (include sample rate, channel, bitrate
in each frame header), so concatenating them produces a valid AAC file
playable by any standard audio player.

Usage:
    # Extract audio from a single .ulg file
    python3 ulog_audio_extract.py log.ulg -o output.aac

    # Batch extract from all .ulg files in a directory
    python3 ulog_audio_extract.py /path/to/logs/ -o /path/to/output/

    # Show audio info without extracting
    python3 ulog_audio_extract.py log.ulg --info

ULog File Format: https://docs.px4.io/main/en/dev_log/ulog_file_format.html
"""

import argparse
import os
import struct
import sys
from pathlib import Path
from dataclasses import dataclass, field

# ── ULog constants ──
ULOG_MAGIC = b"ULog\x01\x12\x35\x01"
ULOG_MSG_HEADER_LEN = 3  # msg_size (2) + msg_type (1)

# Message types
MSG_TYPE_FORMAT = ord('F')
MSG_TYPE_DATA = ord('D')
MSG_TYPE_INFO = ord('I')
MSG_TYPE_ADD_LOGGED_MSG = ord('A')
MSG_TYPE_REMOVE_LOGGED_MSG = ord('R')
MSG_TYPE_SYNC = ord('S')
MSG_TYPE_DROPOUT = ord('O')
MSG_TYPE_FLAG_BITS = ord('B')


@dataclass
class AudioFrameFormat:
    """Parsed audio_frame topic format from ULog F messages."""
    topic_name: str = "audio_frame"
    fields: dict = field(default_factory=dict)
    field_order: list = field(default_factory=list)
    struct_format: str = ""
    header_size: int = 0  # size of fields before aac_data
    aac_data_offset: int = 0
    aac_size_offset: int = 0
    aac_data_max_size: int = 8192


@dataclass
class ExtractStats:
    """Statistics from extraction."""
    frames_extracted: int = 0
    bytes_extracted: int = 0
    sample_rate: int = 0
    channel: int = 0
    bits_per_sample: int = 0
    duration_seconds: float = 0.0
    first_timestamp: int = 0
    last_timestamp: int = 0


def parse_format_message(payload: bytes) -> tuple[str, dict, list]:
    """Parse a ULog Format (F) message.

    Format: "topic_name:type0 field0;type1 field1;..."
    Returns (topic_name, fields_dict, field_order).
    """
    text = payload.decode('utf-8', errors='replace')
    colon_pos = text.find(':')
    if colon_pos < 0:
        return "", {}, []
    topic_name = text[:colon_pos]
    fields_str = text[colon_pos + 1:]

    fields = {}
    field_order = []
    for part in fields_str.split(';'):
        part = part.strip()
        if not part:
            continue
        space_pos = part.find(' ')
        if space_pos < 0:
            continue
        ftype = part[:space_pos]
        fname = part[space_pos + 1:]
        # Handle array types like uint8_t[8192]
        array_size = 1
        base_type = ftype
        bracket_pos = ftype.find('[')
        if bracket_pos >= 0:
            base_type = ftype[:bracket_pos]
            try:
                array_size = int(ftype[bracket_pos + 1:ftype.index(']')])
            except (ValueError, IndexError):
                array_size = 1
        fields[fname] = (base_type, array_size)
        field_order.append(fname)

    return topic_name, fields, field_order


# C type to struct format char and size
CTYPE_MAP = {
    'uint8_t':  ('B', 1),
    'uint16_t': ('H', 2),
    'uint32_t': ('I', 4),
    'uint64_t': ('Q', 8),
    'int8_t':   ('b', 1),
    'int16_t':  ('h', 2),
    'int32_t':  ('i', 4),
    'int64_t':  ('q', 8),
    'float':    ('f', 4),
    'float32':  ('f', 4),
    'float64':  ('d', 8),
    'double':   ('d', 8),
    'bool':     ('B', 1),
    'char':     ('c', 1),
}


def build_audio_frame_struct(fields: dict, field_order: list) -> AudioFrameFormat:
    """Build struct format string and offsets for audio_frame topic.

    ULog data is packed (no padding), matching PX4 convention.
    """
    fmt_parts = ['<']  # little-endian
    offset = 0
    aac_data_offset = 0
    aac_size_offset = 0
    header_size = 0

    for fname in field_order:
        if fname.startswith('_padding'):
            # Skip padding fields — ULog uses o_size_no_padding
            continue

        base_type, array_size = fields[fname]

        if fname == 'aac_data':
            aac_data_offset = offset
            # Don't include aac_data in struct format — we read it separately
            header_size = offset
            offset += array_size * CTYPE_MAP.get(base_type, ('B', 1))[1]
            continue

        if fname == 'aac_size':
            aac_size_offset = offset

        type_info = CTYPE_MAP.get(base_type)
        if not type_info:
            raise ValueError(f"Unknown C type: {base_type}")

        fmt_char, type_size = type_info
        if array_size > 1:
            fmt_parts.append(f'{array_size}{fmt_char}')
        else:
            fmt_parts.append(fmt_char)
        offset += type_size * array_size

    # Build the list of header field names (excluding _padding and aac_data)
    header_field_names = []
    for fname in field_order:
        if fname.startswith('_padding') or fname == 'aac_data':
            continue
        header_field_names.append(fname)

    return AudioFrameFormat(
        struct_format=''.join(fmt_parts),
        header_size=header_size,
        aac_data_offset=aac_data_offset,
        aac_size_offset=aac_size_offset,
        aac_data_max_size=fields.get('aac_data', ('uint8_t', 8192))[1],
        field_order=header_field_names,
    )


def extract_audio_from_ulog(ulg_path: str, output_path: str | None = None,
                            info_only: bool = False) -> ExtractStats:
    """Extract AAC audio from a ULog file.

    Args:
        ulg_path: Path to .ulg file
        output_path: Output .aac file path (default: same name with .aac)
        info_only: Only print info, don't write file

    Returns:
        ExtractStats with extraction statistics
    """
    stats = ExtractStats()

    with open(ulg_path, 'rb') as f:
        # ── Read file header ──
        magic = f.read(8)
        if magic != ULOG_MAGIC:
            print(f"Error: Not a ULog file (magic: {magic!r})", file=sys.stderr)
            return stats

        timestamp = struct.unpack('<Q', f.read(8))[0]
        print(f"ULog file: {ulg_path}")
        print(f"  Created at: {timestamp} µs since boot")

        # ── Parse definition section ──
        formats = {}  # topic_name -> (fields, field_order)
        subscriptions = {}  # msg_id -> topic_name
        audio_msg_id = None
        audio_format = None

        while True:
            hdr = f.read(ULOG_MSG_HEADER_LEN)
            if len(hdr) < ULOG_MSG_HEADER_LEN:
                break

            msg_size = struct.unpack('<H', hdr[:2])[0]
            msg_type = hdr[2]

            payload = f.read(msg_size)
            if len(payload) < msg_size:
                break

            if msg_type == MSG_TYPE_FLAG_BITS:
                # Flag bits — just skip
                pass

            elif msg_type == MSG_TYPE_FORMAT:
                topic_name, fields, field_order = parse_format_message(payload)
                if topic_name:
                    formats[topic_name] = (fields, field_order)
                    if topic_name == 'audio_frame':
                        print(f"  Found audio_frame format: {len(fields)} fields")
                        audio_format = build_audio_frame_struct(fields, field_order)

            elif msg_type == MSG_TYPE_ADD_LOGGED_MSG:
                if msg_size < 3:
                    continue
                multi_id = payload[0]
                msg_id = struct.unpack('<H', payload[1:3])[0]
                topic_name_bytes = payload[3:]
                topic_name = topic_name_bytes.decode('utf-8', errors='replace')
                subscriptions[msg_id] = topic_name
                if topic_name == 'audio_frame':
                    audio_msg_id = msg_id
                    print(f"  audio_frame subscription: msg_id={msg_id}")

            elif msg_type == MSG_TYPE_INFO:
                # Info message — extract audio metadata if present
                if msg_size < 1:
                    continue
                key_len = payload[0]
                key_value = payload[1:]
                key = key_value[:key_len].decode('utf-8', errors='replace')
                value = key_value[key_len:]
                # Print audio-related info
                if key.startswith('audio_'):
                    val_str = value.decode('utf-8', errors='replace') if value else ''
                    print(f"  Info: {key} = {val_str}")

            elif msg_type == MSG_TYPE_DATA:
                # First DATA message — definition section is over
                # Seek back and process data section
                f.seek(-(ULOG_MSG_HEADER_LEN + msg_size), 1)
                break

        if audio_msg_id is None:
            print("  No audio_frame topic found in this ULog file")
            return stats

        if audio_format is None:
            print("  No audio_frame format definition found")
            return stats

        # ── Data section ──
        if not info_only:
            if output_path is None:
                output_path = str(Path(ulg_path).with_suffix('.aac'))
            out_file = open(output_path, 'wb')
            print(f"  Output: {output_path}")

        header_struct = struct.Struct(audio_format.struct_format)

        while True:
            hdr = f.read(ULOG_MSG_HEADER_LEN)
            if len(hdr) < ULOG_MSG_HEADER_LEN:
                break

            msg_size = struct.unpack('<H', hdr[:2])[0]
            msg_type = hdr[2]

            if msg_type == MSG_TYPE_DATA:
                payload = f.read(msg_size)
                if len(payload) < msg_size:
                    break

                # Check msg_id
                msg_id = struct.unpack('<H', payload[:2])[0]
                if msg_id != audio_msg_id:
                    continue

                # Parse audio_frame header fields
                data_payload = payload[2:]  # skip msg_id
                if len(data_payload) < audio_format.header_size:
                    continue

                header_values = header_struct.unpack_from(data_payload)

                # Map header values to field names
                field_map = dict(zip(audio_format.field_order, header_values))

                # Get aac_size
                aac_size_data = data_payload[audio_format.aac_size_offset:
                                             audio_format.aac_size_offset + 2]
                aac_size = struct.unpack('<H', aac_size_data)[0]

                if aac_size == 0 or aac_size > audio_format.aac_data_max_size:
                    continue

                # Get aac_data
                aac_data = data_payload[audio_format.aac_data_offset:
                                        audio_format.aac_data_offset + aac_size]

                if not info_only:
                    out_file.write(aac_data)

                stats.frames_extracted += 1
                stats.bytes_extracted += aac_size

                # Track metadata from first frame
                if stats.frames_extracted == 1:
                    stats.sample_rate = field_map.get('sample_rate', 0)
                    stats.channel = field_map.get('channel', 0)
                    stats.bits_per_sample = field_map.get('bits_per_sample', 0)
                    stats.first_timestamp = field_map.get('timestamp', 0)

                stats.last_timestamp = field_map.get('timestamp', 0)

            elif msg_type == MSG_TYPE_SYNC:
                f.read(msg_size)  # skip sync marker

            elif msg_type == MSG_TYPE_DROPOUT:
                f.read(msg_size)

            else:
                # Skip unknown message types
                f.read(msg_size)

    # Calculate duration
    if stats.sample_rate > 0 and stats.frames_extracted > 0:
        # AAC frame = 1024 samples
        total_samples = stats.frames_extracted * 1024
        stats.duration_seconds = total_samples / stats.sample_rate

    if not info_only:
        out_file.close()

    return stats


def print_stats(stats: ExtractStats, ulg_path: str):
    """Print extraction statistics."""
    print(f"\n  Audio extraction summary for {ulg_path}:")
    print(f"    Frames: {stats.frames_extracted}")
    print(f"    AAC bytes: {stats.bytes_extracted}")
    print(f"    Sample rate: {stats.sample_rate} Hz")
    print(f"    Channels: {stats.channel}")
    print(f"    Bits per sample: {stats.bits_per_sample}")
    print(f"    Duration: {stats.duration_seconds:.1f} seconds")
    if stats.first_timestamp > 0:
        print(f"    First timestamp: {stats.first_timestamp} µs")
        print(f"    Last timestamp: {stats.last_timestamp} µs")


def main():
    parser = argparse.ArgumentParser(
        description='Extract AAC audio from PX4 ULog (.ulg) files')
    parser.add_argument('input', help='Input .ulg file or directory')
    parser.add_argument('-o', '--output', help='Output .aac file (or directory for batch)')
    parser.add_argument('--info', action='store_true',
                        help='Show audio info without extracting')
    args = parser.parse_args()

    input_path = Path(args.input)

    if input_path.is_dir():
        # Batch mode
        ulg_files = sorted(input_path.glob('*.ulg'))
        if not ulg_files:
            print(f"No .ulg files found in {input_path}", file=sys.stderr)
            sys.exit(1)

        output_dir = Path(args.output) if args.output else input_path
        output_dir.mkdir(parents=True, exist_ok=True)

        total_frames = 0
        total_bytes = 0
        for ulg_file in ulg_files:
            out_path = str(output_dir / ulg_file.with_suffix('.aac').name)
            stats = extract_audio_from_ulog(str(ulg_file), out_path, info_only=args.info)
            print_stats(stats, str(ulg_file))
            total_frames += stats.frames_extracted
            total_bytes += stats.bytes_extracted

        print(f"\nTotal: {len(ulg_files)} files, {total_frames} frames, {total_bytes} bytes")

    elif input_path.is_file():
        stats = extract_audio_from_ulog(str(input_path), args.output, info_only=args.info)
        print_stats(stats, str(input_path))
        if stats.frames_extracted == 0:
            sys.exit(1)
    else:
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()

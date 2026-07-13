#!/usr/bin/env python3
"""
px_generate_uorb_topic_files.py (simplified for FreeRTOS)
──────────────────────────────────────────────────────────
Parses .msg files (PX4-compatible format) and generates
uORB C headers, source definitions, and topic enum tables.

Usage:
    python tools/msg_gen.py --headers proto/*.msg -o build/generated/uorb/topics
    python tools/msg_gen.py --sources proto/*.msg -o build/generated/uorb/topics
    python tools/msg_gen.py --topics  proto/*.msg -o build/generated/uorb

Dependencies: none (self-contained, no genmsg/empy required)
"""

import argparse
import os
import re
import struct
import sys


# ────────────────────────────────────────────────────────────
#  .msg format parser
# ────────────────────────────────────────────────────────────

TYPE_MAP = {
    # uORB type → C type
    'bool':        'bool',
    'int8':        'int8_t',
    'uint8':       'uint8_t',
    'int16':       'int16_t',
    'uint16':      'uint16_t',
    'int32':       'int32_t',
    'uint32':      'uint32_t',
    'int64':       'int64_t',
    'uint64':      'uint64_t',
    'float32':     'float',
    'float64':     'double',
    'char':        'char',
}

SIZE_MAP = {
    'bool':    1, 'int8': 1, 'uint8': 1,
    'int16':   2, 'uint16': 2,
    'int32':   4, 'uint32': 4,
    'int64':   8, 'uint64': 8,
    'float32': 4, 'float64': 8,
    'char':    1,
}


class MsgField:
    """A single field in a .msg file."""
    def __init__(self, type_raw: str, name: str, array_len: int = 0):
        self.type_raw = type_raw      # e.g. 'uint32' or 'char'
        self.name     = name
        self.array_len = array_len    # 0 = scalar, >0 = static array

    @property
    def c_type(self) -> str:
        base = TYPE_MAP.get(self.type_raw, self.type_raw)
        if self.array_len:
            return f'{base}[{self.array_len}]'
        return base

    @property
    def size(self) -> int:
        base = SIZE_MAP.get(self.type_raw, 4)
        return base * (self.array_len or 1)

    @property
    def alignment(self) -> int:
        """C struct alignment requirement for this field."""
        if self.array_len:
            # Array alignment = element alignment
            return SIZE_MAP.get(self.type_raw, 4)
        # Scalar alignment = size (up to 8 for uint64_t/float64)
        return min(SIZE_MAP.get(self.type_raw, 4), 8)


class MsgSpec:
    """Parsed representation of a single .msg file."""
    def __init__(self, filepath: str):
        self.filepath     = filepath
        self.basename     = os.path.splitext(os.path.basename(filepath))[0]
        self.topic_name   = _to_snake_case(self.basename)
        self.fields       = []          # list of MsgField
        self.constants    = {}          # name → value
        self.aliases      = []          # topic aliases from # TOPICS
        self.queue_depth  = 1           # from ORB_QUEUE_LENGTH

        with open(filepath, 'r') as f:
            for line in f:
                line = line.split('#')[0].strip()   # strip comments
                if not line:
                    continue

                # Constants:  uint8 ORB_QUEUE_LENGTH = 4
                m = re.match(r'^(\w+)\s+(\w+)\s*=\s*(\d+)', line)
                if m:
                    if m.group(2) == 'ORB_QUEUE_LENGTH':
                        self.queue_depth = int(m.group(3))
                    else:
                        self.constants[m.group(2)] = int(m.group(3))
                    continue

                # TOPICS alias line (handled separately)
                if line.startswith('TOPICS '):
                    self.aliases = line[7:].strip().split()
                    continue

                # Regular field:  type name
                # Support:  type[10] name  or  type name
                m = re.match(r'^(\w+)(?:\[(\d+)\])?\s+(\w+)', line)
                if m:
                    type_raw  = m.group(1)
                    array_len = int(m.group(2)) if m.group(2) else 0
                    name      = m.group(3)
                    self.fields.append(MsgField(type_raw, name, array_len))

    @property
    def struct_size(self) -> int:
        return sum(f.size for f in self.fields)

    @property
    def all_topic_names(self) -> list:
        """Primary topic name + any aliases from # TOPICS."""
        names = [self.topic_name]
        for alias in self.aliases:
            names.append(_to_snake_case(alias))
        return names


def _to_snake_case(name: str) -> str:
    """PascalCase or camelCase → snake_case."""
    s = re.sub(r'(.)([A-Z][a-z]+)', r'\1_\2', name)
    s = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', s)
    return s.lower()


# ────────────────────────────────────────────────────────────
#  Code generation helpers
# ────────────────────────────────────────────────────────────

def _guard_name(topic_name: str) -> str:
    return f'UORB_TOPIC_{topic_name.upper()}_H_'


def _license_header() -> str:
    return (
        '/*\n'
        ' * Automatically generated — DO NOT EDIT.\n'
        ' * Generated from .msg files by tools/msg_gen.py\n'
        ' */\n'
    )


def _include_guard_open(guard: str) -> str:
    return f'#ifndef {guard}\n#define {guard}\n\n'


def _include_guard_close(guard: str) -> str:
    return f'\n#endif /* {guard} */\n'


# ────────────────────────────────────────────────────────────
#  ULog format string generation
# ────────────────────────────────────────────────────────────

def _format_string(spec: MsgSpec) -> str:
    """
    Generate the ULog format string for a topic.
    Format: "topic_name:type0 name0;type1 name1;..."
    
    Fields are listed in .msg declaration order (same as C struct layout),
    with explicit _padding fields inserted to match compiler struct alignment.
    This ensures pyulog's field offsets (computed from format string order)
    match the actual C struct binary layout.
    
    pyulog compatibility: pyulog strips trailing _padding fields before
    calculating max_data_size. The ULog writer must write only
    o_size_no_padding bytes (sizeof - trailing padding) per DATA message,
    so that data_size <= pyulog's max_data_size. This matches PX4's
    convention (orb_metadata.o_size_no_padding).
    
    Returns:
        (format_string, padding_end_size) tuple
    """
    max_align = max((f.alignment for f in spec.fields), default=1)
    
    parts = [f"{spec.topic_name}:"]
    offset = 0
    padding_idx = 0
    
    for f in spec.fields:
        field_align = f.alignment
        # Insert inter-field padding if current offset doesn't satisfy alignment
        if field_align > 1 and (offset % field_align) != 0:
            pad_bytes = field_align - (offset % field_align)
            base = TYPE_MAP.get('uint8', 'uint8_t')
            parts.append(f"{base}[{pad_bytes}] _padding{padding_idx};")
            offset += pad_bytes
            padding_idx += 1
        
        base = TYPE_MAP.get(f.type_raw, f.type_raw)
        if f.array_len:
            parts.append(f"{base}[{f.array_len}] {f.name};")
        else:
            parts.append(f"{base} {f.name};")
        offset += f.size
    
    # Trailing padding: struct must be aligned to its largest field alignment
    padding_end_size = 0
    if max_align > 1 and (offset % max_align) != 0:
        pad_bytes = max_align - (offset % max_align)
        base = TYPE_MAP.get('uint8', 'uint8_t')
        parts.append(f"{base}[{pad_bytes}] _padding{padding_idx};")
        padding_end_size = pad_bytes
    
    return (''.join(parts), padding_end_size)


# ────────────────────────────────────────────────────────────
#  Per-topic header (*.h)
# ────────────────────────────────────────────────────────────

def _render_header(spec: MsgSpec) -> str:
    guard = _guard_name(spec.topic_name)
    fmt_str, padding_end_size = _format_string(spec)
    lines = [_license_header(),
             _include_guard_open(guard),
             '#include <cstdint>\n',
             '#include <cstddef>\n\n',
             f'#define ORB_QUEUE_LENGTH_{spec.topic_name.upper()} {spec.queue_depth}\n\n',
             f'#define {spec.topic_name.upper()}_FORMAT_STR "{fmt_str}"\n\n',
             '// NOLINTNEXTLINE(modernize-use-using)\n'
             f'typedef struct {spec.topic_name}_s\n'
             '{\n',
    ]

    for f in spec.fields:
        # C type: base_type name[array_len]  or base_type name
        base = TYPE_MAP.get(f.type_raw, f.type_raw)
        if f.array_len:
            lines.append(f'    {base:<24} {f.name}[{f.array_len}];  ///< @brief\n')
        else:
            lines.append(f'    {base:<24} {f.name};  ///< @brief\n')

    lines.append(
        f'}} {spec.topic_name}_s;\n\n'
        f'#define {spec.topic_name.upper()}_SIZE sizeof({spec.topic_name}_s)\n\n'
        f'// NOLINTNEXTLINE\n'
        f'static constexpr size_t {spec.topic_name}_SIZE_CONST {{ {spec.topic_name.upper()}_SIZE }};\n\n'
        f'/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */\n'
        f'#define {spec.topic_name.upper()}_SIZE_NO_PADDING (sizeof({spec.topic_name}_s) - {padding_end_size})\n\n'
        f'// NOLINTNEXTLINE\n'
        f'static constexpr size_t {spec.topic_name}_SIZE_NO_PADDING_CONST {{ {spec.topic_name.upper()}_SIZE_NO_PADDING }};\n'
    )

    lines.append(_include_guard_close(guard))
    return ''.join(lines)


# ────────────────────────────────────────────────────────────
#  Per-topic source (*.cpp) — ORB_TOPIC_DEFINE
# ────────────────────────────────────────────────────────────

def _render_source(spec: MsgSpec) -> str:
    fmt_str, padding_end_size = _format_string(spec)
    lines = [
        _license_header(),
        f'#include "{spec.topic_name}.h"\n',
        '#include <uorb.h>\n\n',
    ]

    for topic in spec.all_topic_names:
        lines.append(
            f'ORB_TOPIC_DEFINE({topic}, {spec.topic_name}_s, {spec.queue_depth}, "{fmt_str}", {padding_end_size});\n'
        )

    return ''.join(lines)


# ────────────────────────────────────────────────────────────
#  Combined topic enumeration (uORBTopics.hpp)
# ────────────────────────────────────────────────────────────

def _render_topics_hpp(specs: list, all_topics_sorted: list) -> str:
    lines = [
        _license_header(),
        '#pragma once\n\n',
        '#include <cstdint>\n',
        '#include "uorb.h"\n\n',
        f'static constexpr size_t ORB_TOPICS_COUNT {{ {len(all_topics_sorted)} }};\n\n',
        'enum class ORB_ID : uint8_t {\n',
    ]

    for idx, t in enumerate(all_topics_sorted):
        lines.append(f'    {t} = {idx},\n')

    lines.append('    INVALID\n};\n\n')
    lines.append(f'extern const struct orb_metadata *const *orb_get_topics();\n')
    lines.append(f'extern const struct orb_metadata *get_orb_meta(ORB_ID id);\n')
    return ''.join(lines)


# ────────────────────────────────────────────────────────────
#  Combined topic metadata (uORBTopics.cpp)
# ────────────────────────────────────────────────────────────

def _render_topics_cpp(specs: list, all_topics_sorted: list) -> str:
    lines = [
        _license_header(),
        '#include "uORBTopics.hpp"\n\n',
    ]

    # Forward-declare every topic's metadata
    for t in all_topics_sorted:
        lines.append(f'ORB_TOPIC_DECLARE({t});\n')

    lines.append(f'\nstatic const struct orb_metadata *const s_topics[ORB_TOPICS_COUNT] = {{\n')
    for t in all_topics_sorted:
        lines.append(f'    ORB_ID({t}),\n')

    lines.append('};\n\n')

    lines.append(
        'const struct orb_metadata *const *orb_get_topics()\n'
        '{\n'
        '    return s_topics;\n'
        '}\n\n'
    )

    lines.append(
        'const struct orb_metadata *get_orb_meta(ORB_ID id)\n'
        '{\n'
        '    if (static_cast<uint8_t>(id) < ORB_TOPICS_COUNT) {\n'
        '        return s_topics[static_cast<uint8_t>(id)];\n'
        '    }\n'
        '    return nullptr;\n'
        '}\n'
    )

    return ''.join(lines)


# ────────────────────────────────────────────────────────────
#  Main
# ────────────────────────────────────────────────────────────

def parse_msg_files(files: list) -> list:
    specs = []
    for f in files:
        if f.endswith('.msg'):
            specs.append(MsgSpec(f))
    return specs


def collect_all_topics(specs: list) -> list:
    topics = set()
    for s in specs:
        for t in s.all_topic_names:
            topics.add(t)
    return sorted(topics)


def main():
    parser = argparse.ArgumentParser(
        description='Generate uORB C/C++ code from .msg files')
    parser.add_argument('--headers', action='store_true',
                        help='Generate per-topic header files')
    parser.add_argument('--sources', action='store_true',
                        help='Generate per-topic source files')
    parser.add_argument('--topics', action='store_true',
                        help='Generate uORBTopics.hpp and uORBTopics.cpp')
    parser.add_argument('-f', '--files', nargs='+', required=True,
                        help='.msg files to process')
    parser.add_argument('-o', '--output-dir', required=True,
                        help='Output directory')
    args = parser.parse_args()

    if not any([args.headers, args.sources, args.topics]):
        print('Error: at least one of --headers, --sources, --topics required')
        sys.exit(1)

    os.makedirs(args.output_dir, exist_ok=True)

    specs = parse_msg_files(args.files)
    all_topics = collect_all_topics(specs)

    for spec in specs:
        if args.headers:
            out = os.path.join(args.output_dir, f'{spec.topic_name}.h')
            with open(out, 'w') as f:
                f.write(_render_header(spec))
            print(f'  [H]  {out}')

        if args.sources:
            out = os.path.join(args.output_dir, f'{spec.topic_name}.cpp')
            with open(out, 'w') as f:
                f.write(_render_source(spec))
            print(f'  [C]  {out}')

    if args.topics:
        out_h = os.path.join(args.output_dir, 'uORBTopics.hpp')
        with open(out_h, 'w') as f:
            f.write(_render_topics_hpp(specs, all_topics))
        print(f'  [H]  {out_h}')

        out_cpp = os.path.join(args.output_dir, 'uORBTopics.cpp')
        with open(out_cpp, 'w') as f:
            f.write(_render_topics_cpp(specs, all_topics))
        print(f'  [C]  {out_cpp}')


if __name__ == '__main__':
    main()

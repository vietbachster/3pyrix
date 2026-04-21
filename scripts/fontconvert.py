#!/usr/bin/env python3
# /// script
# dependencies = ["freetype-py", "fonttools"]
# ///
"""
Font converter for Papyrix e-reader.

Converts TTF/OTF fonts to:
- C header files (.h) for builtin fonts
- Binary .epdfont files for SD card fonts

Based on epdiy fontconvert and the Crosspoint font pipeline.
"""

import argparse
import math
import struct
import sys
import unicodedata
from collections import namedtuple
from pathlib import Path

import freetype
from ctypes import byref, pointer
from fontTools.ttLib import TTFont
from freetype.raw import FT_Fixed, FT_Get_MM_Var, FT_LOAD_FORCE_AUTOHINT, FT_LOAD_RENDER, FT_MM_Var, FT_Set_Var_Design_Coordinates

# Unicode intervals for multi-language support
# Must not overlap and should be in ascending order for merging.

INTERVALS_BASE = [
    (0x0000, 0x007F),  # Basic Latin (ASCII)
    (0x0080, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
    (0x0180, 0x024F),  # Latin Extended-B (includes Vietnamese O/U horn)
    (0x0250, 0x02AF),  # IPA Extensions
    (0x0300, 0x036F),  # Combining Diacritical Marks
    (0x0370, 0x03FF),  # Greek and Coptic
    (0x0400, 0x04FF),  # Cyrillic
    (0x1E00, 0x1EFF),  # Latin Extended Additional (Vietnamese tones)
    (0x2000, 0x206F),  # General Punctuation
    (0x2070, 0x209F),  # Superscripts and Subscripts
    (0x20A0, 0x20CF),  # Currency Symbols
    (0x2190, 0x21FF),  # Arrows
    (0x2200, 0x22FF),  # Mathematical Operators
    (0xFB00, 0xFB06),  # Alphabetic Presentation Forms (Latin ligatures)
    (0xFFFD, 0xFFFD),  # Replacement Character
]

INTERVALS_THAI = [
    (0x0E00, 0x0E7F),  # Thai
]

INTERVALS_ARABIC = [
    (0x0600, 0x06FF),  # Arabic
    (0x0750, 0x077F),  # Arabic Supplement
    (0xFE70, 0xFEFF),  # Arabic Presentation Forms-B (contextual forms used by ArabicShaper)
]

INTERVALS_HEBREW = [
    (0x0590, 0x05FF),  # Hebrew (letters, points, cantillation marks)
    (0xFB1D, 0xFB4F),  # Alphabetic Presentation Forms (Hebrew ligatures)
]

GlyphProps = namedtuple(
    "GlyphProps",
    ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"],
)

COMBINING_MARKS_START = 0x0300
COMBINING_MARKS_END = 0x036F
FONT_RENDER_DPI = 200
EPDFONT_MAGIC = 0x46445045
EPDFONT_VERSION = 2
GLYPH_BINARY_SIZE = 14

# Standard Unicode ligature codepoints for known input sequences.
# Used when GSUB references a substitute glyph that has no direct cmap entry.
STANDARD_LIGATURE_MAP = {
    (0x66, 0x66): 0xFB00,  # ff
    (0x66, 0x69): 0xFB01,  # fi
    (0x66, 0x6C): 0xFB02,  # fl
    (0x66, 0x66, 0x69): 0xFB03,  # ffi
    (0x66, 0x66, 0x6C): 0xFB04,  # ffl
    (0x17F, 0x74): 0xFB05,  # long-s + t
    (0x73, 0x74): 0xFB06,  # st
}


def norm_floor(val):
    return int(math.floor(val / (1 << 6)))


def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))


def round_ft16_16_to_px(val):
    return int((val + 0x8000) >> 16)


def fp4_from_design_units(du, scale):
    """Scale a design-unit kerning value to integer pixels and clamp to int8."""
    raw = int(round(du * scale))
    return max(-128, min(127, raw))


def chunks(lst, n):
    for i in range(0, len(lst), n):
        yield lst[i : i + n]


def merge_intervals(intervals):
    """Merge overlapping or adjacent intervals."""
    if not intervals:
        return []

    sorted_intervals = sorted(intervals)
    merged = [list(sorted_intervals[0])]
    for i_start, i_end in sorted_intervals[1:]:
        if i_start <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], i_end)
        else:
            merged.append([i_start, i_end])
    return [tuple(x) for x in merged]


def cp_label(cp):
    if cp == 0x5C:
        return "<backslash>"
    return chr(cp) if 0x20 < cp < 0x7F else f"U+{cp:04X}"


def build_load_flags(force_autohint):
    load_flags = FT_LOAD_RENDER
    if force_autohint:
        load_flags |= FT_LOAD_FORCE_AUTOHINT
    return load_flags


def is_combining_mark(code_point):
    try:
        return unicodedata.combining(chr(code_point)) != 0 or unicodedata.category(chr(code_point)).startswith("M")
    except ValueError:
        return COMBINING_MARKS_START <= code_point <= COMBINING_MARKS_END


def load_glyph(font_stack, code_point, load_flags):
    """Load a glyph from the font stack, returning (face, face_index)."""
    for face_index, face in enumerate(font_stack):
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, load_flags)
            return face, face_index
    return None, None


def validate_intervals(font_stack, intervals, load_flags):
    """Filter intervals to only include code points present in the font stack."""
    valid_intervals = []
    for i_start, i_end in intervals:
        start = i_start
        for code_point in range(i_start, i_end + 1):
            face, _ = load_glyph(font_stack, code_point, load_flags)
            if face is None:
                if start < code_point:
                    valid_intervals.append((start, code_point - 1))
                start = code_point + 1
        if start <= i_end:
            valid_intervals.append((start, i_end))
    return valid_intervals


def render_glyph_2bit(bitmap):
    """Render a FreeType bitmap to 2-bit grayscale packed format."""
    pixels4g = []
    px = 0
    for i, value in enumerate(bitmap.buffer):
        x = i % bitmap.width
        if x % 2 == 0:
            px = value >> 4
        else:
            px = px | (value & 0xF0)
            pixels4g.append(px)
            px = 0
        if x == bitmap.width - 1 and bitmap.width % 2 > 0:
            pixels4g.append(px)
            px = 0

    pixels2b = []
    px = 0
    pitch = (bitmap.width // 2) + (bitmap.width % 2)
    for y in range(bitmap.rows):
        for x in range(bitmap.width):
            px = px << 2
            bm = pixels4g[y * pitch + (x // 2)]
            bm = (bm >> ((x % 2) * 4)) & 0xF
            if bm >= 12:
                px += 3
            elif bm >= 8:
                px += 2
            elif bm >= 4:
                px += 1
            if (y * bitmap.width + x) % 4 == 3:
                pixels2b.append(px)
                px = 0
    if (bitmap.width * bitmap.rows) % 4 != 0:
        px = px << (4 - (bitmap.width * bitmap.rows) % 4) * 2
        pixels2b.append(px)

    return bytes(pixels2b)


def render_glyph_1bit(bitmap):
    """Render a FreeType bitmap to 1-bit packed format."""
    pixels4g = []
    px = 0
    for i, value in enumerate(bitmap.buffer):
        x = i % bitmap.width
        if x % 2 == 0:
            px = value >> 4
        else:
            px = px | (value & 0xF0)
            pixels4g.append(px)
            px = 0
        if x == bitmap.width - 1 and bitmap.width % 2 > 0:
            pixels4g.append(px)
            px = 0

    pixelsbw = []
    px = 0
    pitch = (bitmap.width // 2) + (bitmap.width % 2)
    for y in range(bitmap.rows):
        for x in range(bitmap.width):
            px = px << 1
            bm = pixels4g[y * pitch + (x // 2)]
            px += 1 if ((x & 1) == 0 and bm & 0xE > 0) or ((x & 1) == 1 and bm & 0xE0 > 0) else 0
            if (y * bitmap.width + x) % 8 == 7:
                pixelsbw.append(px)
                px = 0
    if (bitmap.width * bitmap.rows) % 8 != 0:
        px = px << (8 - (bitmap.width * bitmap.rows) % 8)
        pixelsbw.append(px)

    return bytes(pixelsbw)


def set_variable_font_weight(face, weight):
    """Set weight axis on a variable font. No-op if the font is not variable."""
    mm_var_p = pointer(FT_MM_Var())
    err = FT_Get_MM_Var(face._FT_Face, byref(mm_var_p))
    if err != 0:
        return

    mm = mm_var_p.contents
    coords = (FT_Fixed * mm.num_axis)()
    for i in range(mm.num_axis):
        axis = mm.axis[i]
        name = axis.name.decode().lower()
        if "weight" in name or "wght" in name:
            coords[i] = FT_Fixed(int(weight * 65536))
        else:
            coords[i] = axis.default
    FT_Set_Var_Design_Coordinates(face._FT_Face, mm.num_axis, coords)


def build_codepoint_face_map(font_stack, codepoints):
    """Map each codepoint to the first face in the fallback stack that serves it."""
    cp_to_face_idx = {}
    for cp in codepoints:
        for face_index, face in enumerate(font_stack):
            if face.get_char_index(cp) > 0:
                cp_to_face_idx[cp] = face_index
                break
    return cp_to_face_idx


def extract_pairpos_subtable(subtable, glyph_to_cp, raw_kern):
    """Extract kerning from a PairPos subtable (Format 1 or 2)."""
    if subtable.Format == 1:
        for index, coverage_glyph in enumerate(subtable.Coverage.glyphs):
            if coverage_glyph not in glyph_to_cp:
                continue
            pair_set = subtable.PairSet[index]
            for pair_value in pair_set.PairValueRecord:
                if pair_value.SecondGlyph not in glyph_to_cp:
                    continue
                x_advance = 0
                if hasattr(pair_value, "Value1") and pair_value.Value1:
                    x_advance = getattr(pair_value.Value1, "XAdvance", 0) or 0
                if x_advance != 0:
                    key = (coverage_glyph, pair_value.SecondGlyph)
                    raw_kern[key] = raw_kern.get(key, 0) + x_advance
    elif subtable.Format == 2:
        class_def1 = subtable.ClassDef1.classDefs if subtable.ClassDef1 else {}
        class_def2 = subtable.ClassDef2.classDefs if subtable.ClassDef2 else {}
        coverage_set = set(subtable.Coverage.glyphs)
        for left_glyph in glyph_to_cp:
            if left_glyph not in coverage_set:
                continue
            class1 = class_def1.get(left_glyph, 0)
            if class1 >= len(subtable.Class1Record):
                continue
            class1_record = subtable.Class1Record[class1]
            for right_glyph in glyph_to_cp:
                class2 = class_def2.get(right_glyph, 0)
                if class2 >= len(class1_record.Class2Record):
                    continue
                class2_record = class1_record.Class2Record[class2]
                x_advance = 0
                if hasattr(class2_record, "Value1") and class2_record.Value1:
                    x_advance = getattr(class2_record.Value1, "XAdvance", 0) or 0
                if x_advance != 0:
                    key = (left_glyph, right_glyph)
                    raw_kern[key] = raw_kern.get(key, 0) + x_advance


def extract_kerning_fonttools(font_path, codepoints, ppem):
    """Extract kerning pairs from a font file using fonttools."""
    font = TTFont(font_path)
    units_per_em = font["head"].unitsPerEm
    cmap = font.getBestCmap() or {}

    glyph_to_cp = {}
    for cp in codepoints:
        glyph_name = cmap.get(cp)
        if glyph_name:
            glyph_to_cp[glyph_name] = cp

    raw_kern = {}

    if "kern" in font:
        for subtable in font["kern"].kernTables:
            if hasattr(subtable, "kernTable"):
                for (left_glyph, right_glyph), value in subtable.kernTable.items():
                    if left_glyph in glyph_to_cp and right_glyph in glyph_to_cp:
                        raw_kern[(left_glyph, right_glyph)] = raw_kern.get((left_glyph, right_glyph), 0) + value

    if "GPOS" in font:
        gpos = font["GPOS"].table
        kern_lookup_indices = set()
        if gpos.FeatureList:
            for feature_record in gpos.FeatureList.FeatureRecord:
                if feature_record.FeatureTag == "kern":
                    kern_lookup_indices.update(feature_record.Feature.LookupListIndex)
        for lookup_index in kern_lookup_indices:
            lookup = gpos.LookupList.Lookup[lookup_index]
            for subtable in lookup.SubTable:
                actual = subtable
                if lookup.LookupType == 9 and hasattr(subtable, "ExtSubTable"):
                    actual = subtable.ExtSubTable
                if hasattr(actual, "Format"):
                    extract_pairpos_subtable(actual, glyph_to_cp, raw_kern)

    font.close()

    scale = ppem / units_per_em
    result = {}
    for (left_glyph, right_glyph), value in raw_kern.items():
        left_cp = glyph_to_cp[left_glyph]
        right_cp = glyph_to_cp[right_glyph]
        adjust = fp4_from_design_units(value, scale)
        if adjust != 0:
            result[(left_cp, right_cp)] = adjust
    return result


def derive_kern_classes(kern_map):
    """Convert pair kerning into class-based tables."""
    if not kern_map:
        return [], [], [], 0, 0

    all_left_cps = {left_cp for left_cp, _ in kern_map}
    all_right_cps = {right_cp for _, right_cp in kern_map}
    sorted_left_cps = sorted(all_left_cps)
    sorted_right_cps = sorted(all_right_cps)

    left_profile_to_class = {}
    left_class_map = {}
    left_class_id = 1
    for left_cp in sorted_left_cps:
        row = tuple(kern_map.get((left_cp, right_cp), 0) for right_cp in sorted_right_cps)
        if row not in left_profile_to_class:
            left_profile_to_class[row] = left_class_id
            left_class_id += 1
        left_class_map[left_cp] = left_profile_to_class[row]

    right_profile_to_class = {}
    right_class_map = {}
    right_class_id = 1
    for right_cp in sorted_right_cps:
        col = tuple(kern_map.get((left_cp, right_cp), 0) for left_cp in sorted_left_cps)
        if col not in right_profile_to_class:
            right_profile_to_class[col] = right_class_id
            right_class_id += 1
        right_class_map[right_cp] = right_profile_to_class[col]

    kern_left_class_count = left_class_id - 1
    kern_right_class_count = right_class_id - 1
    if kern_left_class_count > 255 or kern_right_class_count > 255:
        print(
            "Warning: kerning class count exceeds uint8_t range "
            f"(left={kern_left_class_count}, right={kern_right_class_count})",
            file=sys.stderr,
        )

    kern_matrix = [0] * (kern_left_class_count * kern_right_class_count)
    for (left_cp, right_cp), adjust in kern_map.items():
        left_class = left_class_map[left_cp] - 1
        right_class = right_class_map[right_cp] - 1
        kern_matrix[left_class * kern_right_class_count + right_class] = adjust

    return (
        sorted(left_class_map.items()),
        sorted(right_class_map.items()),
        kern_matrix,
        kern_left_class_count,
        kern_right_class_count,
    )


def extract_ligatures_fonttools(font_path, input_codepoints, all_output_codepoints):
    """Extract pair-based ligature substitutions from a font file using fonttools."""
    font = TTFont(font_path)
    cmap = font.getBestCmap() or {}

    glyph_to_cp = {}
    for cp, glyph_name in cmap.items():
        glyph_to_cp[glyph_name] = cp

    raw_ligatures = {}
    if "GSUB" in font:
        gsub = font["GSUB"].table
        lookup_indices = set()
        if gsub.FeatureList:
            for feature_record in gsub.FeatureList.FeatureRecord:
                if feature_record.FeatureTag in ("liga", "rlig"):
                    lookup_indices.update(feature_record.Feature.LookupListIndex)

        for lookup_index in lookup_indices:
            lookup = gsub.LookupList.Lookup[lookup_index]
            for subtable in lookup.SubTable:
                actual = subtable
                if lookup.LookupType == 7 and hasattr(subtable, "ExtSubTable"):
                    actual = subtable.ExtSubTable
                if not hasattr(actual, "ligatures"):
                    continue
                for first_glyph, ligature_list in actual.ligatures.items():
                    if first_glyph not in glyph_to_cp:
                        continue
                    first_cp = glyph_to_cp[first_glyph]
                    for ligature in ligature_list:
                        component_cps = []
                        valid = True
                        for component_glyph in ligature.Component:
                            if component_glyph not in glyph_to_cp:
                                valid = False
                                break
                            component_cps.append(glyph_to_cp[component_glyph])
                        if not valid:
                            continue

                        sequence = tuple([first_cp] + component_cps)
                        if ligature.LigGlyph in glyph_to_cp:
                            ligature_cp = glyph_to_cp[ligature.LigGlyph]
                        elif sequence in STANDARD_LIGATURE_MAP:
                            ligature_cp = STANDARD_LIGATURE_MAP[sequence]
                        else:
                            continue

                        raw_ligatures[sequence] = ligature_cp

    font.close()

    filtered = {}
    for sequence, ligature_cp in raw_ligatures.items():
        if ligature_cp not in all_output_codepoints:
            continue
        if all(cp in input_codepoints for cp in sequence):
            filtered[sequence] = ligature_cp

    pairs = []
    two_char = {sequence: ligature_cp for sequence, ligature_cp in filtered.items() if len(sequence) == 2}
    for sequence, ligature_cp in two_char.items():
        packed_pair = (sequence[0] << 16) | sequence[1]
        pairs.append((packed_pair, ligature_cp))

    for sequence, ligature_cp in filtered.items():
        if len(sequence) < 3:
            continue
        prefix = sequence[:-1]
        last_cp = sequence[-1]
        if prefix in filtered:
            intermediate_cp = filtered[prefix]
            packed_pair = (intermediate_cp << 16) | last_cp
            pairs.append((packed_pair, ligature_cp))

    unique_pairs = []
    seen_pairs = set()
    for packed_pair, ligature_cp in sorted(pairs, key=lambda pair: pair[0]):
        if packed_pair in seen_pairs:
            continue
        seen_pairs.add(packed_pair)
        unique_pairs.append((packed_pair, ligature_cp))
    return unique_pairs


def extract_font_tables(font_paths, font_stack, glyph_props, size):
    """Extract kerning and ligature tables for the generated glyph set."""
    all_codepoints = [glyph.code_point for glyph in glyph_props]
    all_codepoint_set = set(all_codepoints)

    kernable_codepoints = {cp for cp in all_codepoint_set if not is_combining_mark(cp)}
    cp_to_face_idx = build_codepoint_face_map(font_stack, kernable_codepoints)
    face_to_codepoints = {}
    for cp, face_index in cp_to_face_idx.items():
        face_to_codepoints.setdefault(face_index, set()).add(cp)

    ppem = size * FONT_RENDER_DPI / 72.0
    kern_map = {}
    for face_index, codepoints in face_to_codepoints.items():
        kern_map.update(extract_kerning_fonttools(font_paths[face_index], codepoints, ppem))

    (
        kern_left_classes,
        kern_right_classes,
        kern_matrix,
        kern_left_class_count,
        kern_right_class_count,
    ) = derive_kern_classes(kern_map)

    ligature_input_codepoints = {cp for cp in all_codepoint_set if not is_combining_mark(cp)}
    ligature_cp_to_face_idx = build_codepoint_face_map(font_stack, ligature_input_codepoints)
    ligature_face_to_codepoints = {}
    for cp, face_index in ligature_cp_to_face_idx.items():
        ligature_face_to_codepoints.setdefault(face_index, set()).add(cp)

    ligature_pairs = []
    for face_index, codepoints in ligature_face_to_codepoints.items():
        ligature_pairs.extend(extract_ligatures_fonttools(font_paths[face_index], codepoints, all_codepoint_set))

    unique_pairs = []
    seen_pairs = set()
    for packed_pair, ligature_cp in sorted(ligature_pairs, key=lambda pair: pair[0]):
        if packed_pair in seen_pairs:
            continue
        seen_pairs.add(packed_pair)
        unique_pairs.append((packed_pair, ligature_cp))

    if kern_map:
        matrix_size = kern_left_class_count * kern_right_class_count
        entry_bytes = (len(kern_left_classes) + len(kern_right_classes)) * 3
        print(
            "kerning: "
            f"{len(kern_map)} pairs, {kern_left_class_count} left classes, "
            f"{kern_right_class_count} right classes, {matrix_size + entry_bytes} bytes",
            file=sys.stderr,
        )
    else:
        print("kerning: 0 pairs extracted", file=sys.stderr)

    print(f"ligatures: {len(unique_pairs)} pairs extracted", file=sys.stderr)

    return {
        "kern_left_classes": kern_left_classes,
        "kern_right_classes": kern_right_classes,
        "kern_matrix": kern_matrix,
        "kern_left_class_count": kern_left_class_count,
        "kern_right_class_count": kern_right_class_count,
        "ligature_pairs": unique_pairs,
    }


def convert_font(font_paths, size, intervals, is_2bit, weight=None, force_autohint=False):
    """Convert font files to glyph and table data."""
    font_stack = [freetype.Face(str(path)) for path in font_paths]
    load_flags = build_load_flags(force_autohint)

    for face in font_stack:
        if weight:
            set_variable_font_weight(face, weight)
        face.set_char_size(size << 6, size << 6, FONT_RENDER_DPI, FONT_RENDER_DPI)

    merged_intervals = merge_intervals(intervals)
    valid_intervals = validate_intervals(font_stack, merged_intervals, load_flags)
    if not valid_intervals:
        print("Error: No valid glyphs found in font", file=sys.stderr)
        return None

    total_size = 0
    all_glyphs = []

    for interval_start, interval_end in valid_intervals:
        for code_point in range(interval_start, interval_end + 1):
            face, _ = load_glyph(font_stack, code_point, load_flags)
            if face is None:
                continue

            bitmap = face.glyph.bitmap
            packed = render_glyph_2bit(bitmap) if is_2bit else render_glyph_1bit(bitmap)

            advance_x = 0 if is_combining_mark(code_point) else min(round_ft16_16_to_px(face.glyph.linearHoriAdvance), 255)
            glyph = GlyphProps(
                width=min(bitmap.width, 255),
                height=min(bitmap.rows, 255),
                advance_x=max(0, advance_x),
                left=face.glyph.bitmap_left,
                top=face.glyph.bitmap_top,
                data_length=len(packed),
                data_offset=total_size,
                code_point=code_point,
            )
            total_size += len(packed)
            all_glyphs.append((glyph, packed))

    metric_face, _ = load_glyph(font_stack, ord("|"), load_flags)
    if metric_face is None and all_glyphs:
        metric_face, _ = load_glyph(font_stack, all_glyphs[0][0].code_point, load_flags)

    metrics = {
        "advance_y": norm_ceil(metric_face.size.height),
        "ascender": norm_ceil(metric_face.size.ascender),
        "descender": norm_floor(metric_face.size.descender),
    }

    glyph_props = [glyph for glyph, _ in all_glyphs]
    tables = extract_font_tables([str(path) for path in font_paths], font_stack, glyph_props, size)

    return {
        "glyphs": all_glyphs,
        "intervals": valid_intervals,
        "metrics": metrics,
        "is_2bit": is_2bit,
        "tables": tables,
    }


def build_header_lines(font_name, data, size=None, cmd_line=None):
    """Build C header text for a font."""
    glyphs = data["glyphs"]
    intervals = data["intervals"]
    metrics = data["metrics"]
    is_2bit = data["is_2bit"]
    tables = data["tables"]

    glyph_data = []
    glyph_props = []
    for props, packed in glyphs:
        glyph_data.extend(packed)
        glyph_props.append(props)

    kern_left_classes = tables["kern_left_classes"]
    kern_right_classes = tables["kern_right_classes"]
    kern_matrix = tables["kern_matrix"]
    ligature_pairs = tables["ligature_pairs"]

    lines = [
        "/**",
        " * generated by fontconvert.py",
        f" * name: {font_name}",
    ]
    if size is not None:
        lines.append(f" * size: {size}")
    lines.append(f" * mode: {'2-bit' if is_2bit else '1-bit'}")
    if cmd_line:
        lines.append(f" * command: {cmd_line}")
    lines.extend(
        [
            " */",
            "#pragma once",
            '#include "EpdFontData.h"',
            "",
            f"static const uint8_t PROGMEM {font_name}Bitmaps[{len(glyph_data)}] = {{",
        ]
    )

    for chunk in chunks(glyph_data, 16):
        lines.append("    " + " ".join(f"0x{byte:02X}," for byte in chunk))
    lines.extend(
        [
            "};",
            "",
            f"static const EpdGlyph PROGMEM {font_name}Glyphs[] = {{",
        ]
    )

    for glyph in glyph_props:
        lines.append(
            "    { "
            + ", ".join(str(value) for value in list(glyph[:-1]))
            + f" }}, // {cp_label(glyph.code_point)}"
        )

    lines.extend(
        [
            "};",
            "",
            f"static const EpdUnicodeInterval PROGMEM {font_name}Intervals[] = {{",
        ]
    )

    offset = 0
    for interval_start, interval_end in intervals:
        lines.append(f"    {{ 0x{interval_start:X}, 0x{interval_end:X}, 0x{offset:X} }},")
        offset += interval_end - interval_start + 1
    lines.extend(["};", ""])

    if kern_left_classes:
        lines.append(f"static const EpdKernClassEntry PROGMEM {font_name}KernLeftClasses[] = {{")
        for code_point, class_id in kern_left_classes:
            lines.append(f"    {{ 0x{code_point:04X}, {class_id} }}, // {cp_label(code_point)}")
        lines.extend(["};", ""])

        lines.append(f"static const EpdKernClassEntry PROGMEM {font_name}KernRightClasses[] = {{")
        for code_point, class_id in kern_right_classes:
            lines.append(f"    {{ 0x{code_point:04X}, {class_id} }}, // {cp_label(code_point)}")
        lines.extend(["};", ""])

        lines.append(f"static const int8_t PROGMEM {font_name}KernMatrix[] = {{")
        row_width = tables["kern_right_class_count"]
        for row in range(tables["kern_left_class_count"]):
            start = row * row_width
            row_values = kern_matrix[start : start + row_width]
            lines.append("    " + ", ".join(f"{value:4d}" for value in row_values) + ",")
        lines.extend(["};", ""])

    if ligature_pairs:
        lines.append(f"static const EpdLigaturePair PROGMEM {font_name}LigaturePairs[] = {{")
        for packed_pair, ligature_cp in ligature_pairs:
            left_cp = packed_pair >> 16
            right_cp = packed_pair & 0xFFFF
            lines.append(
                f"    {{ 0x{packed_pair:08X}, 0x{ligature_cp:04X} }}, "
                f"// {cp_label(left_cp)} {cp_label(right_cp)} -> {cp_label(ligature_cp)}"
            )
        lines.extend(["};", ""])

    lines.extend(
        [
            f"static const EpdFontData {font_name} = {{",
            f"    {font_name}Bitmaps,",
            f"    {font_name}Glyphs,",
            f"    {font_name}Intervals,",
            f"    {len(intervals)},",
            f"    {metrics['advance_y']},",
            f"    {metrics['ascender']},",
            f"    {metrics['descender']},",
            f"    {'true' if is_2bit else 'false'},",
            f"    {font_name}KernLeftClasses," if kern_left_classes else "    nullptr,",
            f"    {font_name}KernRightClasses," if kern_right_classes else "    nullptr,",
            f"    {font_name}KernMatrix," if kern_matrix else "    nullptr,",
            f"    {len(kern_left_classes)},",
            f"    {len(kern_right_classes)},",
            f"    {tables['kern_left_class_count']},",
            f"    {tables['kern_right_class_count']},",
            f"    {font_name}LigaturePairs," if ligature_pairs else "    nullptr,",
            f"    {len(ligature_pairs)},",
            "};",
        ]
    )

    return lines


def write_header(output_path, font_name, data, size=None, cmd_line=None):
    """Write font data as a C header file."""
    lines = build_header_lines(font_name, data, size=size, cmd_line=cmd_line)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines) + "\n")

    glyph_props = [glyph for glyph, _ in data["glyphs"]]
    glyph_data_size = sum(len(bitmap) for _, bitmap in data["glyphs"])
    print(f"Created: {output_path} ({glyph_data_size} bytes bitmap, {len(glyph_props)} glyphs)")


def pack_kern_class_entries(entries):
    return b"".join(struct.pack("<HB", code_point, class_id) for code_point, class_id in entries)


def pack_kern_matrix(values):
    if not values:
        return b""
    return struct.pack(f"<{len(values)}b", *values)


def pack_ligature_pairs(pairs):
    return b"".join(struct.pack("<II", packed_pair, ligature_cp) for packed_pair, ligature_cp in pairs)


def write_epdfont(output_path, data):
    """
    Write font data as a binary .epdfont file.

    Version 2 layout:
      Header (16 bytes)
      Metrics (18 bytes)
      Table counts (12 bytes)
      Intervals
      Glyphs
      Kerning left class entries (3 bytes each)
      Kerning right class entries (3 bytes each)
      Kerning matrix (int8_t)
      Ligature pairs (8 bytes each)
      Bitmap data
    """

    glyphs = data["glyphs"]
    intervals = data["intervals"]
    metrics = data["metrics"]
    is_2bit = data["is_2bit"]
    tables = data["tables"]

    glyph_props = [glyph for glyph, _ in glyphs]
    bitmap_data = b"".join(bitmap for _, bitmap in glyphs)
    kern_left_bytes = pack_kern_class_entries(tables["kern_left_classes"])
    kern_right_bytes = pack_kern_class_entries(tables["kern_right_classes"])
    kern_matrix_bytes = pack_kern_matrix(tables["kern_matrix"])
    ligature_bytes = pack_ligature_pairs(tables["ligature_pairs"])

    header_size = 16
    metrics_size = 18
    table_counts_size = 12
    intervals_size = len(intervals) * 12
    glyphs_size = len(glyph_props) * GLYPH_BINARY_SIZE
    total_size = (
        header_size
        + metrics_size
        + table_counts_size
        + intervals_size
        + glyphs_size
        + len(kern_left_bytes)
        + len(kern_right_bytes)
        + len(kern_matrix_bytes)
        + len(ligature_bytes)
        + len(bitmap_data)
    )

    buf = bytearray(total_size)
    offset = 0

    struct.pack_into("<I", buf, offset, EPDFONT_MAGIC)
    offset += 4
    struct.pack_into("<H", buf, offset, EPDFONT_VERSION)
    offset += 2
    struct.pack_into("<H", buf, offset, 0x01 if is_2bit else 0x00)
    offset += 2
    offset += 8  # reserved

    buf[offset] = metrics["advance_y"] & 0xFF
    offset += 1
    buf[offset] = 0
    offset += 1
    struct.pack_into("<h", buf, offset, metrics["ascender"])
    offset += 2
    struct.pack_into("<h", buf, offset, metrics["descender"])
    offset += 2
    struct.pack_into("<I", buf, offset, len(intervals))
    offset += 4
    struct.pack_into("<I", buf, offset, len(glyph_props))
    offset += 4
    struct.pack_into("<I", buf, offset, len(bitmap_data))
    offset += 4

    struct.pack_into("<H", buf, offset, len(tables["kern_left_classes"]))
    offset += 2
    struct.pack_into("<H", buf, offset, len(tables["kern_right_classes"]))
    offset += 2
    buf[offset] = tables["kern_left_class_count"] & 0xFF
    offset += 1
    buf[offset] = tables["kern_right_class_count"] & 0xFF
    offset += 1
    struct.pack_into("<H", buf, offset, 0)
    offset += 2
    struct.pack_into("<I", buf, offset, len(tables["ligature_pairs"]))
    offset += 4

    glyph_offset = 0
    for interval_start, interval_end in intervals:
        struct.pack_into("<I", buf, offset, interval_start)
        offset += 4
        struct.pack_into("<I", buf, offset, interval_end)
        offset += 4
        struct.pack_into("<I", buf, offset, glyph_offset)
        offset += 4
        glyph_offset += interval_end - interval_start + 1

    for glyph in glyph_props:
        buf[offset] = glyph.width & 0xFF
        offset += 1
        buf[offset] = glyph.height & 0xFF
        offset += 1
        buf[offset] = glyph.advance_x & 0xFF
        offset += 1
        buf[offset] = 0
        offset += 1
        struct.pack_into("<h", buf, offset, glyph.left)
        offset += 2
        struct.pack_into("<h", buf, offset, glyph.top)
        offset += 2
        struct.pack_into("<H", buf, offset, glyph.data_length)
        offset += 2
        struct.pack_into("<I", buf, offset, glyph.data_offset)
        offset += 4

    buf[offset : offset + len(kern_left_bytes)] = kern_left_bytes
    offset += len(kern_left_bytes)
    buf[offset : offset + len(kern_right_bytes)] = kern_right_bytes
    offset += len(kern_right_bytes)
    buf[offset : offset + len(kern_matrix_bytes)] = kern_matrix_bytes
    offset += len(kern_matrix_bytes)
    buf[offset : offset + len(ligature_bytes)] = ligature_bytes
    offset += len(ligature_bytes)
    buf[offset : offset + len(bitmap_data)] = bitmap_data

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(buf)
    print(f"Created: {output_path} ({total_size} bytes, {len(glyph_props)} glyphs)")


def resolve_intervals(args):
    intervals = list(INTERVALS_BASE)
    if args.thai:
        intervals.extend(INTERVALS_THAI)
    if args.arabic:
        intervals.extend(INTERVALS_ARABIC)
    if args.hebrew:
        intervals.extend(INTERVALS_HEBREW)
    if args.additional_intervals:
        for interval_str in args.additional_intervals:
            parts = [int(number, base=0) for number in interval_str.split(",")]
            intervals.append(tuple(parts))
    return intervals


def emit_header_to_stdout(font_name, size, data, cmd_line):
    for line in build_header_lines(font_name, data, size=size, cmd_line=cmd_line):
        print(line)


def main():
    parser = argparse.ArgumentParser(
        description="Convert TTF/OTF fonts to Papyrix format (.epdfont or C header)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate C header (original behavior)
  %(prog)s my_font 16 font.ttf --2bit

  # Generate binary .epdfont file
  %(prog)s my-font -r Regular.ttf -b Bold.ttf --size-opt 16 --2bit -o /tmp/fonts/

  # Generate all sizes with Thai support
  %(prog)s my-font -r Regular.ttf --all-sizes --thai -o /tmp/fonts/
""",
    )

    parser.add_argument("name", help="Font name (for header) or family name (for binary)")
    parser.add_argument(
        "size",
        nargs="?",
        type=int,
        help="Font size (required for header mode, use --size-opt for binary mode)",
    )
    parser.add_argument(
        "fontstack",
        nargs="*",
        help="Font files for header mode (fallback order)",
    )

    parser.add_argument("-r", "--regular", help="Regular style font file")
    parser.add_argument("-b", "--bold", help="Bold style font file")
    parser.add_argument("-i", "--italic", help="Italic style font file")
    parser.add_argument("-o", "--output", default=".", help="Output directory (default: current)")
    parser.add_argument("-s", "--size-opt", "--size", dest="size_opt", type=int, help="Font size in points")
    parser.add_argument("--2bit", dest="is_2bit", action="store_true", help="Generate 2-bit grayscale output")
    parser.add_argument("--header", action="store_true", help="Output C header instead of binary .epdfont")
    parser.add_argument("--thai", action="store_true", help="Include Thai script (U+0E00-U+0E7F)")
    parser.add_argument("--arabic", action="store_true", help="Include Arabic script")
    parser.add_argument("--hebrew", action="store_true", help="Include Hebrew script")
    parser.add_argument("--all-sizes", action="store_true", help="Generate 12, 14, 16, 18pt sizes")
    parser.add_argument("--weight", type=int, help="Variable font weight (e.g. 400 regular, 700 bold)")
    parser.add_argument("--force-autohint", action="store_true", help="Force FreeType auto-hinter")
    parser.add_argument(
        "--additional-intervals",
        dest="additional_intervals",
        action="append",
        help="Additional code point intervals as min,max (can be repeated)",
    )

    args = parser.parse_args()
    intervals = resolve_intervals(args)

    if args.regular:
        styles = []
        if args.regular:
            styles.append(("regular", args.regular))
        if args.bold:
            styles.append(("bold", args.bold))
        if args.italic:
            styles.append(("italic", args.italic))

        base_size = args.size_opt or args.size or 16
        sizes = [12, 14, 16, 18] if args.all_sizes else [base_size]
        output_base = Path(args.output)
        family_name = args.name

        print(f"Converting font family: {family_name}")
        print(f"Output directory: {output_base}")
        print(f"Mode: {'2-bit' if args.is_2bit else '1-bit'}")
        if args.force_autohint:
            print("Using FreeType auto-hinter")
        print()

        for size in sizes:
            family_dir = output_base / (f"{family_name}-{size}" if args.all_sizes else family_name)
            for style_name, font_path in styles:
                path = Path(font_path)
                if not path.exists():
                    print(f"Warning: Font file not found: {path}", file=sys.stderr)
                    continue

                print(f"Converting: {path.name} ({size}pt {style_name})...")
                data = convert_font(
                    [path],
                    size,
                    intervals,
                    args.is_2bit,
                    weight=args.weight,
                    force_autohint=args.force_autohint,
                )
                if data is None:
                    continue

                if args.header:
                    header_name = f"{family_name.replace('-', '_')}_{style_name}"
                    if args.all_sizes:
                        header_name += f"_{size}"
                    output_file = output_base / f"{header_name}_2b.h"
                    write_header(output_file, header_name, data, size=size, cmd_line=" ".join(sys.argv))
                else:
                    output_file = family_dir / f"{style_name}.epdfont"
                    write_epdfont(output_file, data)

        print()
        print("Done! Copy font folder(s) to /config/fonts/ on your SD card.")
        return

    if args.fontstack:
        if not args.size:
            parser.error("Font size is required in header mode")

        font_paths = [Path(path) for path in args.fontstack]
        for path in font_paths:
            if not path.exists():
                print(f"Error: Font file not found: {path}", file=sys.stderr)
                sys.exit(1)

        data = convert_font(
            font_paths,
            args.size,
            intervals,
            args.is_2bit,
            weight=args.weight,
            force_autohint=args.force_autohint,
        )
        if data is None:
            sys.exit(1)

        emit_header_to_stdout(args.name, args.size, data, " ".join(sys.argv))
        return

    parser.print_help()
    sys.exit(1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""usse2gxp.py PROGRAM.json... : azahar's dumped USSE programs as GXP containers.

azahar's sgxd_flags `dumpusse` writes each emitted program to host0 as azahar-KIND-KEY.json
(KIND vs, fs, vs2 or fs2): its code words and the interface sgxd runs it with. This puts each one
in a GXP container next to it (KIND-KEY.gxp), laid out as psp2cgc 3.50 writes one, so the SDK's
psp2shaderperf can list it.

The layout, field for field: header, the varyings block, the secondary program, the phase table,
the primary program, the literals, the dependent samplers, the containers (14: the resident
uniforms, 19: the data block), the uniform buffer info, the parameters and their names.

  tools/usse2gxp.py DIR/azahar-*.json
"""
import json
import struct
import sys
from pathlib import Path

HEADER_SIZE = 0x9C
F_SIZE = 0x08
F_BINARY_GUID = 0x0C
F_SOURCE_GUID = 0x10
F_PROGRAM_FLAGS = 0x14
F_BUFFER_FLAGS = 0x18
F_TEXUNIT_FLAGS0 = 0x1C
F_PARAMETER_COUNT = 0x24
F_PARAMETERS_OFFSET = 0x28
F_VARYINGS_OFFSET = 0x2C
F_PRIMARY_REG_COUNT = 0x30
F_SECONDARY_REG_COUNT = 0x32
F_TEMP_REG_COUNT1 = 0x34
F_PHASE_COUNT = 0x3A
F_PRIMARY_INSTR_COUNT = 0x3C
F_PRIMARY_OFFSET = 0x40
F_SECONDARY_INSTR_COUNT = 0x44
F_SECONDARY_OFFSET = 0x48
F_SECONDARY_OFFSET_END = 0x4C
F_DATA_BUFFER_COUNT = 0x5C
F_DEFAULT_UNIFORM_BUFFER_COUNT = 0x64
F_LITERAL_BUFFER_DATA_OFFSET = 0x68
F_COMPILER_VERSION = 0x6C
F_LITERALS_COUNT = 0x70
F_LITERALS_OFFSET = 0x74
F_UNIFORM_BUFFER_COUNT = 0x78
F_UNIFORM_BUFFER_OFFSET = 0x7C
F_DEPENDENT_SAMPLER_COUNT = 0x80
F_DEPENDENT_SAMPLER_OFFSET = 0x84
F_TEXTURE_BUFFER_DEPENDENT_SAMPLER_OFFSET = 0x8C
F_CONTAINER_COUNT = 0x90
F_CONTAINER_OFFSET = 0x94
F_SAMPLER_QUERY_INFO_OFFSET = 0x98

# What psp2cgc 3.50 writes: version 1.5, SDK 0x350, its own version word.
MAJOR, MINOR, SDK_VERSION, COMPILER_VERSION = 1, 5, 0x350, 0x33DC0

# program_flags: fragment bit 0; 0x1000 and 0x4 with interpolated inputs; 0x180000 always;
# 0x8 discard used; 0x10 depth written. Vertex: 0x190000 always, 0x2 per-instance mode.
FLAG_FRAGMENT = 1 << 0
FLAG_ITERATORS = 0x1000
FLAG_ITERATORS_PRESENT = 0x4
FLAG_ALWAYS = 0x180000
FLAG_DISCARD_USED = 1 << 3
FLAG_DEPTH_USED = 1 << 4
FLAG_VERTEX_ALWAYS = 0x190000
FLAG_PER_INSTANCE = 1 << 1
# buffer_flags: buffer 14 (the default uniform buffer) in registers, and read from memory.
BUFFER_DEFAULT_IN_REGISTERS = 1 << 28
BUFFER_DEFAULT_IN_MEMORY = 1 << 29
TEXUNIT_DEPENDENT = 2  # texunit_flags: four bits per unit

CONTAINER_DEFAULT_UNIFORMS = 14
CONTAINER_DATA = 19
SAMPLER_WORDS = 4

CATEGORY_ATTRIBUTE, CATEGORY_UNIFORM, CATEGORY_SAMPLER = 0, 1, 2
TYPE_F32, TYPE_F16 = 0, 1
SEMANTIC_SAMPLER = 1

COLOR0, COLOR1 = 0xA, 0xB  # FragmentInput ids


class Writer:
    def __init__(self):
        self.out = bytearray()

    def size(self):
        return len(self.out)

    def pad(self, n):
        self.out += bytes(n)

    def align(self, a):
        while len(self.out) % a:
            self.out.append(0)

    def u8(self, v):
        self.out += struct.pack('<B', v & 0xFF)

    def u16(self, v):
        self.out += struct.pack('<H', v & 0xFFFF)

    def u32(self, v):
        self.out += struct.pack('<I', v & 0xFFFFFFFF)

    def u64(self, v):
        self.out += struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF)

    def patch16(self, at, v):
        struct.pack_into('<H', self.out, at, v & 0xFFFF)

    def patch32(self, at, v):
        struct.pack_into('<I', self.out, at, v & 0xFFFFFFFF)

    def patch_offset(self, field, target):
        self.patch32(field, target - field)


def varying_descriptor(inputs, index):
    """A fragment program's varying descriptor (usse_program.cpp VaryingDescriptor)."""
    v = inputs[index]
    info = 0x0C00000F | (v['input'] << 12) | ((v['components'] - 1) << 22)
    if v.get('half'):
        info |= 0x20000000  # USEFORMAT F16
    elif v['input'] in (COLOR0, COLOR1):
        info |= 0x00100000
    if index + 1 == len(inputs):
        info |= 0x02000000
    return info


def write_gxp(p):
    w = Writer()
    w.out += b'GXP\0'
    w.u8(MAJOR)
    w.u8(MINOR)
    w.u16(SDK_VERSION)
    w.pad(HEADER_SIZE - w.size())

    inputs, uniforms, samplers = p['inputs'], p['uniforms'], p['samplers']
    attributes, literals = p['attributes'], p['literals']
    if p['vertex']:
        flags = FLAG_VERTEX_ALWAYS | (FLAG_PER_INSTANCE if p['per_instance'] else 0)
    else:
        flags = FLAG_FRAGMENT | FLAG_ALWAYS
        if inputs:
            flags |= FLAG_ITERATORS | FLAG_ITERATORS_PRESENT
        if p['uses_discard']:
            flags |= FLAG_DISCARD_USED
        if p.get('writes_depth'):  # dumps from before the emitter stopped writing depth
            flags |= FLAG_DEPTH_USED
    w.patch32(F_BINARY_GUID, 0)
    w.patch32(F_SOURCE_GUID, 0)
    w.patch32(F_PROGRAM_FLAGS, flags)
    has_uniforms = p['uniform_floats'] != 0
    w.patch32(F_BUFFER_FLAGS, (BUFFER_DEFAULT_IN_REGISTERS if has_uniforms else 0) |
              (BUFFER_DEFAULT_IN_MEMORY if p['buffer_pointer'] else 0))
    texunit_flags = 0
    for s in samplers:
        texunit_flags |= TEXUNIT_DEPENDENT << (s['unit'] * 4)
    w.patch32(F_TEXUNIT_FLAGS0, texunit_flags)
    w.patch16(F_PRIMARY_REG_COUNT, p['pa_count'])
    # Secondary attributes: the resident uniforms, then the data block behind them.
    data_base = p['uniform_floats']
    data_head = 1 if p['buffer_pointer'] else 0
    data_size = data_head + len(literals) + len(samplers) * SAMPLER_WORDS
    w.patch16(F_SECONDARY_REG_COUNT, max(data_base + data_size, p['secondary_reg_count']))
    w.patch32(F_DATA_BUFFER_COUNT, data_size)
    w.patch32(F_LITERALS_COUNT, len(literals))
    w.patch32(F_TEMP_REG_COUNT1, p['temp_count'])
    w.patch16(F_PHASE_COUNT, len(p['phase_starts']))
    w.patch32(F_PRIMARY_INSTR_COUNT, len(p['primary']))
    w.patch32(F_SECONDARY_INSTR_COUNT, len(p['secondary']))
    w.patch32(F_DEFAULT_UNIFORM_BUFFER_COUNT, max(p['uniform_floats'], p['buffer_floats']))
    w.patch32(F_COMPILER_VERSION, COMPILER_VERSION)

    # Varyings: 8 unknown zero bytes, the output description, then the descriptors.
    w.patch_offset(F_VARYINGS_OFFSET, w.size())
    if p['vertex']:
        pa_regs = 0
        for a in attributes:
            pa_regs |= ((1 << a['words']) - 1) << a['resource']
        w.u64(pa_regs)
        w.u64(0)  # untyped_pa_regs
        w.u32(p['outselects'])
        w.u32(p['texsize'])
        w.u32(0)  # texcoord_pack_format: all float
        w.u16(0)  # semantic_index_offset
        w.u16(0)  # semantic_instance_offset
    else:
        w.pad(8)
        w.u8(0)         # fragment_output_start
        w.u8(0)         # unk1
        w.u8(TYPE_F16)  # the colour leaves the program as half4 in pa0
        w.u8(4)
        w.u16(len(inputs))
        w.u16(0)  # non-dependent sampler count
        w.u32(4)  # the descriptors follow this field directly
        for i, v in enumerate(inputs):
            w.u32(varying_descriptor(inputs, i))
            w.u32(0)                             # resource_index
            words = (v['components'] + 1) // 2 if v.get('half') else v['components']
            w.u32((words - 1) << 4)              # registers minus one
            w.u32(0)                             # component_info

    # Secondary program, phase table, primary program.
    w.patch_offset(F_SECONDARY_OFFSET, w.size())
    for word in p['secondary']:
        w.u64(int(word, 16))
    w.patch_offset(F_SECONDARY_OFFSET_END, w.size())
    for start in p['phase_starts']:
        w.u32(start)
    w.patch_offset(F_PRIMARY_OFFSET, w.size())
    for word in p['primary']:
        w.u64(int(word, 16))

    # Literals: (SA offset within the data block, raw value), after the samplers' words
    # (usse_program.h LiteralSaOffset).
    w.patch_offset(F_LITERALS_OFFSET, w.size())
    for i, value in enumerate(literals):
        w.u32(data_head + len(samplers) * SAMPLER_WORDS + i)
        w.u32(value)

    # Dependent samplers: four control words per sampler, each entry naming the unit's word
    # (unit * 4 + k) and the word's SA within the data block.
    samplers_at = w.size()
    w.patch_offset(F_DEPENDENT_SAMPLER_OFFSET, samplers_at)
    w.patch_offset(F_LITERAL_BUFFER_DATA_OFFSET, samplers_at)
    w.patch32(F_DEPENDENT_SAMPLER_COUNT, len(samplers) * SAMPLER_WORDS)
    for j, s in enumerate(samplers):
        for k in range(SAMPLER_WORDS):
            w.u16(s['unit'] * SAMPLER_WORDS + k)
            w.u16(data_head + j * SAMPLER_WORDS + k)

    # Containers.
    containers_at = w.size()
    count = 0
    if has_uniforms:
        w.u16(CONTAINER_DEFAULT_UNIFORMS)
        w.u16(0)
        w.u16(0)
        w.u16(p['uniform_floats'])
        count += 1
    if data_size:
        w.u16(CONTAINER_DATA)
        w.u16(0)
        w.u16(data_base)
        w.u16(data_size)
        count += 1
    w.patch32(F_CONTAINER_COUNT, count)
    w.patch_offset(F_CONTAINER_OFFSET, containers_at)
    w.patch_offset(F_TEXTURE_BUFFER_DEPENDENT_SAMPLER_OFFSET, containers_at)

    # Uniform buffer info: where libgxm plants the buffer pointer (reside_buffer,
    # ldst_base_offset, ldst_base_value), right after the containers.
    buffer_info_at = 0
    if p['buffer_pointer']:
        buffer_info_at = w.size()
        w.u16(CONTAINER_DEFAULT_UNIFORMS)
        w.u16(0)
        w.u32(p['ldst_base_value'])
        w.patch32(F_UNIFORM_BUFFER_COUNT, 1)

    # Parameters, then their names.
    params_at = w.size()
    w.patch32(F_PARAMETER_COUNT, len(attributes) + len(uniforms) + len(samplers))
    w.patch_offset(F_PARAMETERS_OFFSET, params_at)
    name_fields = []
    for a in attributes:
        name_fields.append(w.size())
        w.u32(0)
        w.u16(CATEGORY_ATTRIBUTE | (TYPE_F32 << 4) | (4 << 8))
        w.u8(0)
        w.u8(0)
        w.u32(1)
        w.u32(a['resource'])
    for u in uniforms:
        name_fields.append(w.size())
        w.u32(0)
        w.u16(CATEGORY_UNIFORM | (TYPE_F32 << 4) | (u['components'] << 8) |
              (CONTAINER_DEFAULT_UNIFORMS << 12))
        w.u8(0)
        w.u8(0)
        w.u32(u['array_size'])
        w.u32(u['sa'])
    for s in samplers:
        name_fields.append(w.size())
        w.u32(0)
        w.u16(CATEGORY_SAMPLER | (TYPE_F32 << 4) | (4 << 8))
        w.u8(SEMANTIC_SAMPLER)
        w.u8(0)
        w.u32(1)
        w.u32(s['unit'])
    # Sampler query info: one u16 per texture unit, 0x0301 for a dependent 2D read.
    if samplers:
        w.patch_offset(F_SAMPLER_QUERY_INFO_OFFSET, w.size())
        units = [0] * 16
        for s in samplers:
            units[s['unit'] & 15] = 0x0301
        for u in units:
            w.u16(u)
    for field, item in zip(name_fields, attributes + uniforms + samplers):
        w.patch32(field, w.size() - field)
        w.out += item['name'].encode() + b'\0'

    # Without a buffer info table the offset points where the compiler points it.
    w.patch_offset(F_UNIFORM_BUFFER_OFFSET, buffer_info_at if p['buffer_pointer'] else params_at)
    w.patch32(F_SIZE, w.size())
    w.align(4)
    return bytes(w.out)


def main(paths):
    if not paths:
        sys.exit(__doc__)
    for path in map(Path, paths):
        out = path.with_name(path.name.removeprefix('azahar-')).with_suffix('.gxp')
        out.write_bytes(write_gxp(json.loads(path.read_text())))
        print(out)


if __name__ == '__main__':
    main(sys.argv[1:])

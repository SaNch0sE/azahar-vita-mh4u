// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Part of the program disk cache's version (src/video_core/CMakeLists.txt hashes this file with
// the USSE emitters, the shader generator and the PICA headers). gxm_rasterizer.cpp is not hashed
// any more: hashing it started every player's programs.bin over on any edit to the rasterizer
// (2026-11-17: a diagnostic-only change did). What the rasterizer contributes to a program is the
// configuration it hands the emitters, and that is the program's key (fs_hash/vs keys hash the
// configuration after BindPrograms' adjustments). Bump this number when a rasterizer change alters
// what a program would be for the same key: a global switch read by the emitters, a key that
// stops covering an input, a new meaning for an existing configuration bit.
#define GXM_PROGRAM_EPOCH 1

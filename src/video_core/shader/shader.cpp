// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include "common/arch.h"
#include "video_core/shader/shader_interpreter.h"
#if CITRA_HAS_SHADER_JIT
#include "video_core/shader/shader_jit.h"
#endif
#include "video_core/shader/shader.h"

namespace Pica {

std::unique_ptr<ShaderEngine> CreateEngine(bool use_jit) {
#if CITRA_HAS_SHADER_JIT
#ifdef __vita__
    // ux0:data/azahar/shader_interpreter: the interpreter, to tell a JIT fault from others.
    if (std::FILE* f = std::fopen("ux0:data/azahar/shader_interpreter", "rb")) {
        std::fclose(f);
        use_jit = false;
    }
#endif
    if (use_jit) {
        return std::make_unique<Shader::JitEngine>();
    }
#endif

    return std::make_unique<Shader::InterpreterEngine>();
}

} // namespace Pica

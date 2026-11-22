// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// zstd.wasm (wasm/z3ds_zstd.c): compresses whole Z3DS frames.

export class FrameCompressor {
    static async create(module, level, frameSize) {
        const result = await WebAssembly.instantiate(module, {});
        const instance = result instanceof WebAssembly.Instance ? result : result.instance;
        return new FrameCompressor(instance.exports, level, frameSize);
    }

    constructor(exports, level, frameSize) {
        this.api = exports;
        this.frameSize = frameSize;
        this.inPtr = 0;
        this.inCap = 0;
        this.outPtr = 0;
        this.outCap = 0;
        this.check(this.api.z3ds_init(level, frameSize));
    }

    check(code) {
        if (this.api.z3ds_is_error(code)) {
            const heap = new Uint8Array(this.api.memory.buffer);
            const at = this.api.z3ds_error_name(code);
            const message = new TextDecoder().decode(heap.subarray(at, heap.indexOf(0, at)));
            throw new Error(`zstd: ${message}`);
        }
        return code >>> 0;
    }

    alloc(ptr, size) {
        if (ptr) {
            this.api.free(ptr);
        }
        const p = this.api.malloc(size);
        if (!p) {
            throw new Error("zstd: out of memory");
        }
        return p;
    }

    // Compresses input, a whole number of frames, into one zstd frame each. The ROM's last job
    // also closes the stream's last frame: a partial one, or an empty one when the ROM ended on
    // a frame edge.
    compress(input, last) {
        const size = this.frameSize;
        const count = last ? Math.floor(input.length / size) + 1 : input.length / size;
        if (!Number.isInteger(count)) {
            throw new Error("a job must hold whole frames");
        }
        const bound = this.api.z3ds_bound(size) >>> 0;
        if (this.inCap < input.length) {
            this.inPtr = this.alloc(this.inPtr, input.length);
            this.inCap = input.length;
        }
        if (this.outCap < bound * count) {
            this.outPtr = this.alloc(this.outPtr, bound * count);
            this.outCap = bound * count;
        }
        new Uint8Array(this.api.memory.buffer).set(input, this.inPtr);
        const sizes = new Uint32Array(count);
        let out = 0;
        for (let i = 0; i < count; i++) {
            const length = Math.min(size, input.length - i * size);
            sizes[i] = this.check(this.api.z3ds_frame(this.inPtr + i * size, length,
                this.outPtr + out, this.outCap - out));
            out += sizes[i];
        }
        return { data: new Uint8Array(this.api.memory.buffer, this.outPtr, out).slice(), sizes };
    }
}

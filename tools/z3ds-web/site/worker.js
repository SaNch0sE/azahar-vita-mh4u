// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// One compression worker: whole frames in, zstd frames out.

import { FrameCompressor } from "./zstd.js";

let compressor;

self.onmessage = async ({ data }) => {
    try {
        if (data.type === "init") {
            compressor = await FrameCompressor.create(data.module, data.level, data.frameSize);
            self.postMessage({ type: "ready" });
            return;
        }
        const { data: out, sizes } = compressor.compress(new Uint8Array(data.buffer), data.last);
        self.postMessage({ type: "done", id: data.id, buffer: out.buffer, sizes },
            [out.buffer, sizes.buffer]);
    } catch (e) {
        self.postMessage({ type: "error", id: data.id, message: e.message ?? String(e) });
    }
};

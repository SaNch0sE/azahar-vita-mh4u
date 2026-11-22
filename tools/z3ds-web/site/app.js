// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The page: takes ROMs, shows what rom.js found, and compresses them on worker threads into a
// Z3DS file (z3ds.js) written straight to disk where the browser allows it.

import { inspect, RomError } from "./rom.js";
import * as z3ds from "./z3ds.js";

const COMPRESSOR = "Azahar web compressor";
const JOB_SIZE = 4 * 1024 * 1024; // frames handed to a worker at once
const THREADS = Math.max(1, Math.min(navigator.hardwareConcurrency || 4, 8));
const CIA_THREADS = Math.min(THREADS, 4); // 32 MiB frames: keep the workers' memory in check

const canPickFile = "showSaveFilePicker" in window;
const canPickFolder = "showDirectoryPicker" in window;
const canUseStorage = !!navigator.storage?.getDirectory &&
    "createWritable" in (globalThis.FileSystemFileHandle?.prototype ?? {});

const queue = document.getElementById("queue");
const drop = document.getElementById("drop");
const pick = document.getElementById("pick");
const level = document.getElementById("level");
const all = document.getElementById("all");
const template = document.getElementById("card");

const wasm = fetch(new URL("zstd.wasm", import.meta.url))
    .then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error(`zstd.wasm: ${r.status}`))))
    .then((bytes) => WebAssembly.compile(bytes));

const entries = [];
let nextId = 0;
let busy = null;

document.getElementById("threads").textContent =
    `${THREADS} thread${THREADS > 1 ? "s" : ""}`;

// Leftovers of an earlier visit in the page's storage (Firefox, Safari).
if (canUseStorage) {
    navigator.storage.getDirectory().then(async (root) => {
        for await (const name of root.keys()) {
            await root.removeEntry(name).catch(() => {});
        }
    }).catch(() => {});
}

// --- formatting ----------------------------------------------------------------------------

function bytes(n) {
    const units = ["bytes", "KiB", "MiB", "GiB"];
    let i = 0;
    while (n >= 1024 && i < units.length - 1) {
        n /= 1024;
        i++;
    }
    return i ? `${n.toFixed(n < 10 ? 2 : 1)} ${units[i]}` : `${n} bytes`;
}

function duration(seconds) {
    const s = Math.max(0, Math.round(seconds));
    return s < 60 ? `${s} s` : `${Math.floor(s / 60)} min ${String(s % 60).padStart(2, "0")} s`;
}

// --- cards ---------------------------------------------------------------------------------

function source(file) {
    return {
        name: file.name,
        size: file.size,
        read: async (offset, length) =>
            new Uint8Array(await file.slice(offset, offset + length).arrayBuffer()),
    };
}

function addFiles(files) {
    for (const file of files) {
        const el = template.content.firstElementChild.cloneNode(true);
        const entry = { id: nextId++, file, el, state: "inspecting", checks: [] };
        el.querySelector(".act-compress").onclick = () =>
            compress(entry, canPickFile ? () => pickedFile(entry) : () => browserFile(entry));
        el.querySelector(".act-cancel").onclick = () => entry.abort?.abort();
        el.querySelector(".act-remove").onclick = () => remove(entry);
        entries.push(entry);
        queue.append(el);
        render(entry);
        inspect(source(file)).then((rom) => {
            entry.rom = rom;
            entry.checks = rom.checks;
            entry.state = "ready";
        }, (e) => {
            entry.state = "invalid";
            entry.checks = e.checks ?? [];
            entry.error = e instanceof RomError ? e.message :
                `This file couldn't be read (${e.message}). Choose it again.`;
        }).then(() => render(entry));
    }
}

function remove(entry) {
    entry.abort?.abort();
    if (entry.url) {
        URL.revokeObjectURL(entry.url);
    }
    entry.el.remove();
    entries.splice(entries.indexOf(entry), 1);
    updateAll();
}

function render(entry) {
    const { el, rom, state } = entry;
    const info = rom?.info ?? {};
    const smdh = info.smdh;
    el.dataset.state = state;

    el.querySelector(".title").textContent = smdh?.title || entry.file.name;
    const sub = [smdh?.publisher, smdh?.title ? entry.file.name : null].filter(Boolean);
    el.querySelector(".sub").textContent = sub.join(" · ");

    el.classList.toggle("has-icon", !!smdh);
    if (smdh) {
        const canvas = el.querySelector("canvas");
        canvas.getContext("2d").putImageData(
            new ImageData(smdh.icon.rgba, smdh.icon.width, smdh.icon.height), 0, 0);
    }

    const facts = [
        ["Type", rom?.kind],
        ["Product", info.productCode, true],
        ["Title ID", info.titleId, true],
        ["Region", smdh?.region],
        ["Version", info.version],
        ["Size", bytes(entry.file.size)],
    ];
    if (info.new3ds) {
        facts.splice(1, 0, ["System", "New 3DS only"]);
    }
    el.querySelector(".facts").replaceChildren(...facts.filter(([, v]) => v).map(([k, v, mono]) => {
        const div = document.createElement("div");
        const dt = document.createElement("dt");
        const dd = document.createElement("dd");
        dt.textContent = k;
        dd.textContent = v;
        dd.classList.toggle("mono", !!mono);
        div.append(dt, dd);
        return div;
    }));

    el.querySelector(".checks").replaceChildren(...entry.checks.map((c) => {
        const li = document.createElement("li");
        li.className = c.state;
        const p = document.createElement("p");
        const b = document.createElement("b");
        b.textContent = c.label;
        p.append(b);
        if (c.state === "ok" && c.detail) {
            const span = document.createElement("span");
            span.textContent = ` — ${c.detail}`;
            p.append(span);
        }
        li.append(p);
        return li;
    }));

    const error = el.querySelector(".error");
    error.hidden = !(state === "invalid" || state === "failed");
    error.textContent = entry.error ?? "";

    el.querySelector(".progress").hidden = state !== "compressing";
    const result = el.querySelector(".result");
    result.hidden = state !== "done";
    if (state === "done") {
        const r = entry.result;
        result.replaceChildren(
            r.saved ? `Saved ${r.name}` : `${r.name} is ready`,
            Object.assign(document.createElement("span"), {
                textContent: ` · ${bytes(r.size)}, ${Math.round((r.size / r.original) * 100)}% ` +
                    `of the original · ${duration(r.seconds)}`,
            }));
    }

    el.querySelector(".out").textContent = rom && state !== "done" ? `→ ${rom.outputName}` : "";
    const compressButton = el.querySelector(".act-compress");
    compressButton.hidden = !(state === "ready" || state === "failed");
    compressButton.textContent = state === "failed" ? "Try again" : "Compress";
    el.querySelector(".act-cancel").hidden = state !== "compressing";
    el.querySelector(".act-remove").hidden = state === "compressing";
    const download = el.querySelector(".act-download");
    download.hidden = !(state === "done" && entry.url);
    if (entry.url) {
        download.href = entry.url;
        download.download = entry.result.name;
        download.textContent = "Download";
    }
    updateAll();
}

function updateAll() {
    const ready = entries.filter((e) => e.state === "ready");
    all.hidden = ready.length < 2;
    all.disabled = !!busy;
    level.disabled = !!busy;
    for (const e of entries) {
        e.el.querySelector(".act-compress").disabled = !!busy;
    }
}

function progress(entry, done, total, compressed, started) {
    const now = performance.now();
    if (done < total && now - (entry.shown ?? 0) < 100) {
        return;
    }
    entry.shown = now;
    const seconds = (now - started) / 1000;
    const rate = done / Math.max(seconds, 0.001);
    entry.el.querySelector(".bar i").style.width = `${(done / total) * 100}%`;
    entry.el.querySelector(".stat").textContent =
        `${bytes(done)} of ${bytes(total)} · ${(rate / 1e6).toFixed(0)} MB/s · ` +
        `${Math.round((compressed / Math.max(done, 1)) * 100)}% of the original so far` +
        (done < total ? ` · ${duration((total - done) / rate)} left` : "");
}

// --- where the file goes -------------------------------------------------------------------

function writer(writable, finish) {
    return {
        write: (data) => writable.write(data),
        async finish(header) {
            await writable.write({ type: "write", position: 0, data: header });
            await writable.close();
            return finish();
        },
        abort: () => writable.abort().catch(() => {}),
    };
}

// Chrome and Edge: a file the user picks, written as it is compressed.
function pickedFile(entry) {
    const ext = entry.rom.outputName.slice(entry.rom.outputName.lastIndexOf("."));
    return showSaveFilePicker({
        suggestedName: entry.rom.outputName,
        types: [{ description: "Compressed 3DS ROM", accept: { "application/octet-stream": [ext] } }],
    }).then(async (handle) => writer(await handle.createWritable(),
        () => ({ saved: true, name: handle.name })));
}

// "Compress all" in Chrome and Edge: files in a folder the user picked.
async function folderFile(folder, entry) {
    const name = entry.rom.outputName;
    const dot = name.lastIndexOf(".");
    let candidate = name;
    for (let n = 2; await folder.getFileHandle(candidate).then(() => true, () => false); n++) {
        candidate = `${name.slice(0, dot)} (${n})${name.slice(dot)}`;
    }
    const handle = await folder.getFileHandle(candidate, { create: true });
    return writer(await handle.createWritable(), () => ({ saved: true, name: candidate }));
}

// Elsewhere: the page's storage, then a download; without that, memory.
async function browserFile(entry) {
    const name = entry.rom.outputName;
    if (canUseStorage) {
        const root = await navigator.storage.getDirectory();
        const handle = await root.getFileHandle(`output-${entry.id}`, { create: true });
        return writer(await handle.createWritable(),
            async () => ({ name, url: URL.createObjectURL(await handle.getFile()) }));
    }
    const parts = [];
    return {
        write: async (data) => void parts.push(data),
        async finish(header) {
            parts[0] = header;
            return { name, url: URL.createObjectURL(new Blob(parts)) };
        },
        abort() {
            parts.length = 0;
        },
    };
}

// --- compression ---------------------------------------------------------------------------

class Pool {
    constructor(count, module, compressionLevel, frameSize) {
        this.size = count;
        this.pending = new Map();
        this.nextJob = 0;
        this.workers = [];
        const ready = [];
        for (let i = 0; i < count; i++) {
            const worker = new Worker(new URL("worker.js", import.meta.url), { type: "module" });
            const slot = { worker, load: 0 };
            ready.push(new Promise((resolve, reject) => {
                worker.onmessage = ({ data }) => {
                    if (data.type === "ready") {
                        resolve();
                        return;
                    }
                    if (data.id === undefined) {
                        reject(new Error(data.message));
                        return;
                    }
                    const job = this.pending.get(data.id);
                    this.pending.delete(data.id);
                    slot.load--;
                    if (data.type === "error") {
                        job.reject(new Error(data.message));
                    } else {
                        job.resolve(data);
                    }
                };
                worker.onerror = (e) => {
                    const error = new Error(e.message || "a compression thread stopped");
                    reject(error);
                    this.failAll(error);
                };
            }));
            worker.postMessage({ type: "init", module, level: compressionLevel, frameSize });
            this.workers.push(slot);
        }
        this.ready = Promise.all(ready);
    }

    run(buffer, last) {
        const slot = this.workers.reduce((a, b) => (b.load < a.load ? b : a));
        slot.load++;
        const id = this.nextJob++;
        return new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            slot.worker.postMessage({ type: "job", id, buffer, last }, [buffer]);
        });
    }

    failAll(error) {
        for (const job of this.pending.values()) {
            job.reject(error);
        }
        this.pending.clear();
    }

    close() {
        for (const { worker } of this.workers) {
            worker.terminate();
        }
        this.failAll(new DOMException("Cancelled", "AbortError"));
    }
}

async function compress(entry, open) {
    if (busy) {
        return;
    }
    busy = entry;
    entry.abort = new AbortController();
    const signal = entry.abort.signal;
    let sink;
    let pool;
    try {
        // Opening the file comes first: a save dialog needs the click it was asked from.
        sink = await open();
        entry.state = "compressing";
        entry.error = null;
        render(entry);
        const threads = entry.rom.format === "CIA" ? CIA_THREADS : THREADS;
        pool = new Pool(threads, await wasm, Number(level.value), entry.rom.frameSize);
        signal.addEventListener("abort", () => pool.close());
        await pool.ready;
        entry.result = await run(entry, sink, pool, signal);
        entry.url = entry.result.url;
        entry.state = "done";
        if (entry.url) {
            const a = Object.assign(document.createElement("a"),
                { href: entry.url, download: entry.result.name });
            a.click();
        }
    } catch (e) {
        sink?.abort();
        if (e.name === "AbortError" || signal.aborted) {
            entry.state = "ready";
        } else {
            entry.state = "failed";
            entry.error = e.name === "QuotaExceededError" ?
                "The browser ran out of storage for the compressed file. Free some disk space, " +
                "or use Chrome or Edge, which write straight to disk." :
                `Compression stopped: ${e.message}. Try again.`;
        }
    } finally {
        pool?.close();
        busy = null;
        render(entry);
    }
}

async function run(entry, sink, pool, signal) {
    const { file, rom } = entry;
    const size = file.size;
    const frameSize = rom.frameSize;
    const jobSize = Math.max(frameSize, JOB_SIZE);
    const jobs = Math.ceil(size / jobSize);
    const metadata = z3ds.metadata([
        ["compressor", COMPRESSOR],
        ["date", z3ds.date()],
        ["maxframesize", `${frameSize}`],
        ...rom.metadata,
    ]);
    await sink.write(new Uint8Array(z3ds.HEADER_SIZE)); // the header goes in last
    await sink.write(metadata);

    const compressedSizes = [];
    const decompressedSizes = [];
    const inFlight = [];
    const limit = pool.size * 2 + 2;
    let next = 0;
    let done = 0;
    let compressed = 0;
    const started = performance.now();
    while (next < jobs || inFlight.length) {
        while (next < jobs && inFlight.length < limit) {
            const offset = next * jobSize;
            const length = Math.min(jobSize, size - offset);
            const buffer = await file.slice(offset, offset + length).arrayBuffer();
            signal.throwIfAborted();
            const job = pool.run(buffer, next === jobs - 1);
            job.catch(() => {}); // seen when its turn to be written comes
            inFlight.push({ job, length });
            next++;
        }
        const { job, length } = inFlight.shift();
        const { buffer, sizes } = await job;
        signal.throwIfAborted();
        await sink.write(new Uint8Array(buffer));
        let left = length;
        for (const s of sizes) {
            const d = Math.min(frameSize, left);
            compressedSizes.push(s);
            decompressedSizes.push(d);
            left -= d;
        }
        done += length;
        compressed += buffer.byteLength;
        progress(entry, done, size, compressed, started);
    }
    const table = z3ds.seekTable(compressedSizes, decompressedSizes);
    await sink.write(table);
    compressed += table.length;
    const out = await sink.finish(
        z3ds.header(rom.underlyingMagic, metadata.length, compressed, size));
    return {
        ...out,
        size: z3ds.HEADER_SIZE + metadata.length + compressed,
        original: size,
        seconds: (performance.now() - started) / 1000,
    };
}

all.onclick = async () => {
    let folder = null;
    if (canPickFolder) {
        try {
            folder = await showDirectoryPicker({ id: "z3ds", mode: "readwrite" });
        } catch {
            return;
        }
    }
    for (const entry of entries.filter((e) => e.state === "ready")) {
        if (entry.state === "ready") {
            await compress(entry, folder ? () => folderFile(folder, entry) : () => browserFile(entry));
        }
    }
};

// --- intake --------------------------------------------------------------------------------

pick.onchange = () => {
    addFiles(pick.files);
    pick.value = "";
};
for (const type of ["dragenter", "dragover"]) {
    document.addEventListener(type, (e) => {
        if (e.dataTransfer?.types.includes("Files")) {
            e.preventDefault();
            drop.classList.add("over");
        }
    });
}
document.addEventListener("dragleave", (e) => {
    if (!e.relatedTarget) {
        drop.classList.remove("over");
    }
});
document.addEventListener("drop", (e) => {
    e.preventDefault();
    drop.classList.remove("over");
    addFiles(e.dataTransfer.files);
});
window.addEventListener("beforeunload", (e) => {
    if (busy) {
        e.preventDefault();
    }
});

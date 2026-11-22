// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

#include "session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <algorithm>
#include <cstdlib>
#include <format>
#include <map>
#include <set>
#include "pica.h"
#include "replay.h"

namespace frame_replay {

namespace {

const char* OpKind(u32 k) {
    static const char* n[] = {"command list", "register write", "fill", "transfer", "DMA"};
    return k < 5 ? n[k] : "op";
}

QString Us(u32 a, u32 b) {
    return b >= a && a ? QString("%1 us").arg(b - a) : QString("-");
}

QString Hex(u32 v) {
    return QString("0x%1").arg(v, 8, 16, QChar('0'));
}

} // namespace

QString PlainText(const QString& html) {
    // Rows on lines and cells in columns: enough for the summary's and the state's HTML.
    QString t = html;
    t.replace(QRegularExpression("<br\\s*/?>|</p>|</tr>|</h[1-6]>|</li>|</div>|</table>",
                                 QRegularExpression::CaseInsensitiveOption), "\n");
    t.replace(QRegularExpression("<h[1-6][^>]*>", QRegularExpression::CaseInsensitiveOption), "\n");
    t.replace(QRegularExpression("</t[dh]>", QRegularExpression::CaseInsensitiveOption), "\t");
    t.remove(QRegularExpression("<[^>]*>"));
    t.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&#39;", "'")
        .replace("&nbsp;", " ").replace("&amp;", "&");
    t.replace(QRegularExpression("\t\n"), "\n");
    t.replace(QRegularExpression("\n{3,}"), "\n\n");
    return t.trimmed() + "\n";
}

bool Session::Open(const QString& path, std::string* error) {
    auto c = std::make_unique<Capture>();
    if (!c->Load(path.toStdString(), error)) {
        return false;
    }
    cap = std::move(c);
    // The replay player's results beside the capture, when player.sh ran.
    std::string err;
    LoadReplays(cap.get(), &err, "player");
    return true;
}

QString Session::SummaryText() const {
    if (!cap) {
        return {};
    }
    const u32 a = cap->t0, b = cap->t1;
    QString h = QString("<h3>%1</h3>").arg(QFileInfo(QString::fromStdString(cap->path)).fileName());
    h += "<table>";
    for (const auto& [k, v] : cap->info) {
        h += QString("<tr><td>%1</td><td>%2</td></tr>")
                 .arg(QString::fromStdString(k), QString::fromStdString(v).toHtmlEscaped());
    }
    h += "</table>";
    h += QString("<p>Range %1 ms</p>").arg((b - a) / 1000.0, 0, 'f', 3);
    // Frames: vblank to vblank, presents, and the GPU's busy time.
    const int vbl = cap->TagId("vblank"), pres = cap->TagId("presented");
    std::vector<u32> vb, pr;
    for (const Event& e : cap->events) {
        if (e.time < a || e.time > b) {
            continue;
        }
        if (int(e.tag) == vbl) {
            vb.push_back(e.time);
        } else if (int(e.tag) == pres) {
            pr.push_back(e.time);
        }
    }
    auto intervals = [](const std::vector<u32>& t) {
        if (t.size() < 2) {
            return QString("-");
        }
        double sum = 0, mx = 0;
        for (size_t i = 1; i < t.size(); i++) {
            const double d = (t[i] - t[i - 1]) / 1000.0;
            sum += d;
            mx = std::max(mx, d);
        }
        return QString("%1 intervals, mean %2 ms (%3 fps), longest %4 ms")
            .arg(t.size() - 1)
            .arg(sum / (t.size() - 1), 0, 'f', 2)
            .arg(1000.0 * (t.size() - 1) / sum, 0, 'f', 1)
            .arg(mx, 0, 'f', 2);
    };
    h += QString("<h4>Frames</h4>vblanks: %1<br>presents: %2<br>").arg(intervals(vb), intervals(pr));
    const int ta = cap->TagId("gpu_ta"), ta_end = cap->TagId("gpu_ta_end");
    const int d3 = cap->TagId("gpu_3d"), d3_end = cap->TagId("gpu_3d_end");
    u64 ta_us = 0, d3_us = 0;
    u32 open_ta = 0, open_3d = 0, n3d = 0;
    for (const Event& e : cap->events) {
        if (e.time < a || e.time > b) {
            continue;
        }
        const int t = int(e.tag);
        if (t == ta) {
            open_ta = e.arg;
        } else if (t == ta_end && open_ta && e.arg >= open_ta) {
            ta_us += e.arg - open_ta;
            open_ta = 0;
        } else if (t == d3) {
            open_3d = e.arg;
        } else if (t == d3_end && open_3d && e.arg >= open_3d) {
            d3_us += e.arg - open_3d;
            open_3d = 0;
            n3d++;
        }
    }
    h += QString("<h4>GPU</h4>TA busy %1 ms (%2%), 3D busy %3 ms (%4%) over %5 stamped renders<br>")
             .arg(ta_us / 1000.0, 0, 'f', 2)
             .arg(100.0 * ta_us / std::max(1u, b - a), 0, 'f', 1)
             .arg(d3_us / 1000.0, 0, 'f', 2)
             .arg(100.0 * d3_us / std::max(1u, b - a), 0, 'f', 1)
             .arg(n3d);
    h += "<p>The stamps are sampled: a list whose stamps were overwritten before the render thread "
         "read them is missing from these sums.</p>";
    return PlainText(h);
}

QString Session::FrameText() const {
    const Frame& f = cap->frame;
    if (!f.present) {
        return "No frame in this capture\n";
    }
    size_t mem = 0;
    for (const auto& [id, b] : f.blobs) {
        mem += b.second;
    }
    QString t = QString("frame %1, %2: %3 ops, %4 draws, %5 passes, %6 KiB of memory\n")
                    .arg(f.number)
                    .arg(Us(f.start, f.end))
                    .arg(f.ops.size())
                    .arg(f.draws.size())
                    .arg(f.passes.size())
                    .arg(mem >> 10);
    if (f.has_rasterizer_state) {
        t += QString("renderer state at the start: %1 surfaces with pixels, %2 depth stores, %3 "
                     "clears\n")
                 .arg(f.state_surfaces).arg(f.state_depths).arg(f.state_clears);
    }
    if (!f.results.empty()) {
      t += QString("%1 results (passes' surfaces, transfers' outputs): "
                   "frame-replay check\n")
               .arg(f.results.size());
    }
    // The emulation thread's stream, with each queued op's render thread work under it.
    for (const StreamItem& s : f.stream) {
        switch (s.kind) {
        case StreamItem::Gsp: {
            const GspCommand& c = f.gsp[s.index];
            t += QString("  +%1 us  GSP %2\n").arg(c.time - f.start, 7).arg(pica::DescribeGsp(c));
            break;
        }
        case StreamItem::Reg: {
            const RegWrite& w = f.regs[s.index];
            t += QString("  +%1 us  write %2 = %3\n")
                     .arg(w.time - f.start, 7)
                     .arg(w.lcd ? QString("LCD %1").arg(w.index, 0, 16) : pica::RegName(w.index))
                     .arg(Hex(w.value));
            break;
        }
        case StreamItem::DmaOp: {
            const Dma& d = f.dmas[s.index];
            QStringList runs;
            for (const auto& r : d.runs) {
                runs << QString("%1 -> %2 (%3)").arg(r[0], 0, 16).arg(r[1], 0, 16).arg(r[2]);
            }
            t += QString("  +%1 us  DMA %2\n").arg(d.time - f.start, 7).arg(runs.join("; "));
            break;
        }
        case StreamItem::OpQueued: {
            const auto oit = f.ops.find(u32(s.index));
            if (oit == f.ops.end()) {
                break;
            }
            const Op& op = oit->second;
            t += QString("  +%1 us  op %2: %3, ran %4 +%5 us; %6 draws, %7 passes\n")
                     .arg(op.queued - f.start, 7)
                     .arg(op.seq)
                     .arg(OpKind(op.kind))
                     .arg(Us(op.begin, op.end))
                     .arg(op.begin ? op.begin - f.start : 0)
                     .arg(op.draws.size())
                     .arg(op.passes.size());
            // Draws and pass kicks in time order.
            std::vector<std::pair<u32, QString>> kids;
            for (size_t di : op.draws) {
                const Draw& d = f.draws[di];
                const pica::TextureUnit t0 = pica::Unit(d.regs, 0);
                kids.push_back({d.time, QString("      +%1 us  draw %2 (%3.%4): %5 vertices%6, %7 regs changed\n")
                                            .arg(d.time - f.start, 7)
                                            .arg(di)
                                            .arg(d.pass)
                                            .arg(d.draw)
                                            .arg(d.vertices)
                                            .arg(t0.enabled ? QString(", tex0 %1x%2 %3").arg(t0.width).arg(t0.height).arg(pica::FormatName(t0.format)) : QString())
                                            .arg(d.changed.size())});
            }
            for (size_t pi : op.passes) {
                const Pass& p = f.passes[pi];
                kids.push_back({p.time, QString("      +%1 us  pass %2 kicked: target %3 %4x%5, %6 draws%7%8\n")
                                            .arg(p.time - f.start, 7)
                                            .arg(p.pass)
                                            .arg(Hex(p.target))
                                            .arg(p.width)
                                            .arg(p.height)
                                            .arg(p.draws)
                                            .arg(p.sync ? ", synchronous" : "")
                                            .arg(p.timed ? QString("; TA %1 us, 3D %2 us").arg(p.ta_us).arg(p.d3_us) : QString())});
            }
            std::stable_sort(kids.begin(), kids.end(),
                             [](const auto& x, const auto& y) { return x.first < y.first; });
            for (const auto& [time, line] : kids) {
                t += line;
            }
            break;
        }
        }
    }
    // The passes the recording timed (PTIM), the longest 3D first.
    std::vector<const Pass*> timed;
    u64 ta_sum = 0, d3_sum = 0;
    for (const Pass& p : f.passes) {
        if (p.timed) {
            timed.push_back(&p);
            ta_sum += p.ta_us;
            d3_sum += p.d3_us;
        }
    }
    if (!timed.empty()) {
        std::stable_sort(timed.begin(), timed.end(),
                         [](const Pass* x, const Pass* y) { return x->d3_us > y->d3_us; });
        t += QString("passes by 3D time (%1 timed: TA %2 us, 3D %3 us in all):\n")
                 .arg(timed.size()).arg(ta_sum).arg(d3_sum);
        for (size_t i = 0; i < timed.size() && i < 20; i++) {
            const Pass& p = *timed[i];
            t += QString("  pass %1 (op %2): target %3 %4x%5, %6 draws; TA %7 us, 3D %8 us\n")
                     .arg(p.pass).arg(p.seq).arg(Hex(p.target)).arg(p.width).arg(p.height)
                     .arg(p.draws).arg(p.ta_us).arg(p.d3_us);
        }
    }
    return t;
}

QString Session::DrawShaders(size_t index) const {
    const Frame& f = cap->frame;
    if (index >= f.draws.size()) {
        return {};
    }
    const Draw& d = f.draws[index];
    QString sh;
    if (d.vs_program >= 0) {
        const u32 entry = d.regs[InternalBase + 0x2BA] & 0xFFFF;
        sh += QString("; vertex shader, entry %1\n").arg(entry, 0, 16);
        // The code before the entry too: the subroutines a call reaches live there (KH3D's
        // skinning calls 0x1b from its entry at 0x23).
        if (entry > 0) {
            sh += "; before the entry\n";
            sh += pica::Disassemble(f.programs[d.vs_program], 0, entry);
            sh += "; the entry\n";
        }
        sh += pica::Disassemble(f.programs[d.vs_program], entry);
        sh += "\n; float uniforms (changed in this frame or non-zero)\n";
        for (u32 i = 0; i < 96; i++) {
            const auto& v = d.vs.f[i];
            if (v[0] || v[1] || v[2] || v[3]) {
                sh += QString("c%1 = (%2, %3, %4, %5)\n").arg(i).arg(v[0]).arg(v[1]).arg(v[2]).arg(v[3]);
            }
        }
        sh += QString("bools %1, ints %2 %3 %4 %5\n")
                  .arg(d.vs.b, 4, 16, QChar('0'))
                  .arg(d.vs.i[0], 8, 16, QChar('0'))
                  .arg(d.vs.i[1], 8, 16, QChar('0'))
                  .arg(d.vs.i[2], 8, 16, QChar('0'))
                  .arg(d.vs.i[3], 8, 16, QChar('0'));
    }
    if (d.gs_program >= 0 && (d.regs[InternalBase + 0x229] & 3) == 2) {
        const u32 entry = d.regs[InternalBase + 0x28A] & 0xFFFF;
        sh += QString("\n; geometry shader, entry %1\n").arg(entry, 0, 16);
        sh += pica::Disassemble(f.programs[d.gs_program], entry);
    }
    return sh;
}

QString Session::DrawText(size_t index) const {
    const Frame& f = cap->frame;
    if (index >= f.draws.size()) {
        return QString("no draw %1 (the frame has %2)\n").arg(index).arg(f.draws.size());
    }
    const Draw& d = f.draws[index];
    QString t = QString("draw %1 (pass %2, draw %3), op %4\n\n").arg(index).arg(d.pass).arg(d.draw).arg(d.seq);
    t += PlainText(pica::DescribeDraw(*cap, d)) + "\n\n; registers (non-zero; * changed by this draw)\n";
    const std::set<u32> changed(d.changed.begin(), d.changed.end());
    for (u32 i = InternalBase; i < d.regs.size() && i < InternalBase + 0x300; i++) {
        if (d.regs[i] == 0 && !changed.count(i)) {
            continue;
        }
        t += QString("%1 %2 %3\n").arg(changed.count(i) ? "*" : " ").arg(pica::RegName(i), -40).arg(Hex(d.regs[i]));
    }
    t += "\n" + DrawShaders(index);
    return t;
}

std::vector<DrawImage> Session::DrawImages(size_t index) const {
    std::vector<DrawImage> out;
    const Frame& f = cap->frame;
    if (index >= f.draws.size()) {
        return out;
    }
    const Draw& d = f.draws[index];
    const auto op = f.ops.find(d.seq);
    for (u32 u = 0; u < 3; u++) {
        const pica::TextureUnit t = pica::Unit(d.regs, u);
        if (!t.enabled) {
            continue;
        }
        QImage img;
        if (op != f.ops.end()) {
            for (size_t li : op->second.loads) {
                const MemLoad& m = f.loads[li];
                if (m.kind == u32(MemKind::Texture) && m.paddr == t.address) {
                    size_t n = 0;
                    const u8* p = cap->Blob(m.blob, &n);
                    img = pica::DecodeTexture(p, n, t.width, t.height, t.format);
                    break;
                }
            }
        }
        out.push_back({QString("texture unit %1: %2 %3x%4 %5").arg(u).arg(Hex(t.address))
                           .arg(t.width).arg(t.height).arg(pica::FormatName(t.format)), img});
    }
    if (op != f.ops.end()) {
        const u32 dim = d.regs[InternalBase + 0x11E];
        const u32 w = dim & 0x7FF, h = ((dim >> 12) & 0x3FF) + 1;
        const u32 fmt = (d.regs[InternalBase + 0x117] >> 16) & 7;
        const u32 addr = (d.regs[InternalBase + 0x11D] & 0x0FFFFFFF) * 8;
        for (size_t li : op->second.loads) {
            const MemLoad& m = f.loads[li];
            if (m.kind == u32(MemKind::Colour) && m.paddr == addr) {
                size_t n = 0;
                const u8* p = cap->Blob(m.blob, &n);
                out.push_back({QString("colour buffer at the pass's start (guest memory): %1 %2x%3")
                                   .arg(Hex(addr)).arg(w).arg(h),
                               pica::DecodeColour(p, n, w, h, fmt)});
            }
        }
    }
    return out;
}

QString Session::OpLoadsText(u32 seq) const {
    const Frame& f = cap->frame;
    const auto it = f.ops.find(seq);
    if (it == f.ops.end()) {
        return QString("no op %1 in the frame\n").arg(seq);
    }
    QString t = QString("op %1 (%2): %3 memory loads\n").arg(seq).arg(OpKind(it->second.kind)).arg(it->second.loads.size());
    for (size_t li : it->second.loads) {
        const MemLoad& m = f.loads[li];
        t += QString("  %1 %2 %3 bytes, blob %4\n").arg(MemKindName(m.kind), -10).arg(Hex(m.paddr)).arg(m.size, 8).arg(m.blob);
    }
    return t;
}

QString Session::ReplayText(std::optional<size_t> draw) const {
    const ReplayResults* r = Replays(cap.get());
    if (!r) {
      return "No replay results (CAPTURE.player.bin).\n";
    }
    QString t = QString("Replay %1 (the replay player); %2 draws\n");
    t = t.arg(QString::fromStdString(r->path)).arg(r->draws);
    if (draw) {
        const auto it = r->costs.find(u32(*draw));
        if (it == r->costs.end()) {
            t += QString("draw %1: no cost (request \"costs\")\n").arg(*draw);
        } else {
            const auto& c = it->second;
            u32 before = 0;
            if (*draw > 0) {
                const auto p = r->costs.find(u32(*draw - 1));
                if (p != r->costs.end() && p->second.pass == c.pass) {
                    before = p->second.d3_us;
                }
            }
            t += QString("draw %1: its pass's TA %2 us, 3D %3 us with the draws up to it; the draw "
                         "adds %4 us of 3D\n")
                     .arg(*draw).arg(c.ta_us).arg(c.d3_us).arg(int(c.d3_us) - int(before));
        }
        if (const auto im = r->images.find(u32(*draw)); im != r->images.end()) {
            t += QString("target %1 after the draw: %2x%3%4\n")
                     .arg(Hex(im->second.addr)).arg(im->second.width).arg(im->second.height)
                     .arg(im->second.rgba.empty() ? " (no surface)" : "");
        }
        return t;
    }
    u32 run = ~0u;
    for (const auto& p : r->passes) {
        if (p.run != run) {
            run = p.run;
            t += QString("run %1 passes (TA / 3D us):").arg(run);
        }
        t += QString(" [%1] %2/%3").arg(p.pass).arg(p.ta_us).arg(p.d3_us);
        if (&p == &r->passes.back() || (&p + 1)->run != run) {
            t += "\n";
        }
    }
    for (const auto& o : r->ops) {
        t += QString("run %1 op %2: %3 us\n").arg(o.run).arg(o.seq).arg(o.us);
    }
    if (!r->runs.empty()) {
        // The `profile` runs: medians and bests, then each run.
        const auto stat = [&r](u32 ReplayResults::Run::*field) {
            std::vector<u32> v;
            for (const auto& x : r->runs) {
                v.push_back(x.*field);
            }
            std::sort(v.begin(), v.end());
            return QString("median %1 us, best %2, worst %3").arg(v[v.size() / 2]).arg(v.front()).arg(v.back());
        };
        t += QString("profile, %1 runs (the frame pipelined as the game runs it):\n").arg(r->runs.size());
        t += "  to the GPU's idle:  " + stat(&ReplayResults::Run::idle_us) + "\n";
        t += "  handed over:        " + stat(&ReplayResults::Run::submit_us) + "\n";
        t += "  memory loads:       " + stat(&ReplayResults::Run::loads_us) + "\n";
        t += "  present:            " + stat(&ReplayResults::Run::present_us) + "\n";
        t += "  run   idle us  handed over  loads  present\n";
        for (const auto& x : r->runs) {
            t += QString("  %1 %2 %3 %4 %5\n").arg(x.index, 4).arg(x.idle_us, 8).arg(x.submit_us, 12)
                     .arg(x.loads_us, 6).arg(x.present_us, 8);
        }
    }
    if (!r->costs.empty()) {
        t += "costs (draw: pass, TA us, 3D us up to it):\n";
        for (const auto& [d, c] : r->costs) {
            t += QString("  %1: %2 %3 %4\n").arg(d, 4).arg(c.pass).arg(c.ta_us, 7).arg(c.d3_us, 7);
        }
    }
    if (!r->images.empty()) {
        t += QString("%1 targets after draws (--images DIR saves them)\n").arg(r->images.size());
    }
    for (const auto& s : r->text) {
        t += QString::fromStdString(s) + "\n";
    }
    return t;
}

namespace {

// A result's pixels as RGBA8: kind 0 is a surface's A8B8G8R8 already; kind 1 is a display
// transfer's linear output in its PICA colour format (FORMAT.md, PRES).
std::vector<u8> ResultRgba(u32 kind, u32 format, u32 width, u32 height, const u8* p, size_t n) {
    const size_t pixels = size_t(width) * height;
    std::vector<u8> out(pixels * 4, 0);
    static constexpr u32 Bytes[5] = {4, 3, 2, 2, 2};
    const u32 bpp = kind == 0 ? 4 : format < 5 ? Bytes[format] : 0;
    if (bpp == 0 || n < pixels * bpp) {
        return {};
    }
    for (size_t i = 0; i < pixels; i++) {
        const u8* s = p + i * bpp;
        u8* d = &out[i * 4];
        const u32 v = s[0] | s[1] << 8;
        const auto bits = [v](u32 at, u32 width) {
            const u32 x = (v >> at) & ((1u << width) - 1);
            return u8((x * 255 + ((1u << width) - 1) / 2) / ((1u << width) - 1));
        };
        if (kind == 0) {
            std::copy(s, s + 4, d);
            continue;
        }
        switch (format) {
        case 0: // RGBA8: A, B, G, R in memory
            d[0] = s[3], d[1] = s[2], d[2] = s[1], d[3] = s[0];
            break;
        case 1: // RGB8: B, G, R
            d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
            break;
        case 2: // RGB565
            d[0] = bits(11, 5), d[1] = bits(5, 6), d[2] = bits(0, 5), d[3] = 255;
            break;
        case 3: // RGB5A1
            d[0] = bits(11, 5), d[1] = bits(6, 5), d[2] = bits(1, 5), d[3] = bits(0, 1);
            break;
        default: // RGBA4
            d[0] = bits(12, 4), d[1] = bits(8, 4), d[2] = bits(4, 4), d[3] = bits(0, 4);
            break;
        }
    }
    return out;
}

bool SavePng(const std::vector<u8>& rgba, u32 width, u32 height, const QString& path) {
    const QImage q(rgba.data(), int(width), int(height), int(width * 4), QImage::Format_RGBA8888);
    return q.copy().save(path);
}

} // namespace

QString Session::ResultsText(const QString& images) const {
    const Frame& f = cap->frame;
    if (f.results.empty()) {
        return "The capture has no results (PRES): record it with azahar's capture version 3.\n";
    }
    QString t = QString("recording %1 results (PRES)\n").arg(f.results.size());
    t += "   #  op  kind      address     size      format\n";
    for (const Result& x : f.results) {
        t += QString("%1 %2  %3  %4  %5x%6  %7")
                 .arg(x.index, 4)
                 .arg(x.seq, 3)
                 .arg(x.kind == 0 ? "pass    " : "transfer")
                 .arg(Hex(x.addr), 10)
                 .arg(x.width, 4)
                 .arg(x.height, -4)
                 .arg(x.format);
        size_t n = 0;
        const u8* p = cap->Blob(x.blob, &n);
        const auto a = ResultRgba(x.kind, x.format, x.width, x.height, p, p ? n : 0);
        if (a.empty()) {
            t += "  (no pixels)\n";
            continue;
        }
        if (!images.isEmpty()) {
            const QString png = QString("%1/result-%2-%3-%4.png")
                                    .arg(images)
                                    .arg(x.index, 3, 10, QChar('0'))
                                    .arg(x.kind == 0 ? "pass" : "transfer")
                                    .arg(Hex(x.addr));
            t += SavePng(a, x.width, x.height, png) ? "  -> " + png : "  (not saved)";
        }
        t += "\n";
    }
    return t;
}

QString Session::CheckText(const QString& images) const {
    const Frame& f = cap->frame;
    if (f.results.empty()) {
        return "The capture has no results (PRES): record it with azahar's capture version 3.\n";
    }
    const ReplayResults* r = Replays(cap.get());
    if (!r || r->results.empty()) {
      return "No replay results (RPRS): player.sh with the request results\n";
    }
    if (!images.isEmpty()) {
        QDir().mkpath(images);
    }
    // The k-th result for a kind and address on one side is the k-th on the other: a pass the
    // replay splits or merges shifts only its own target's list.
    std::map<std::pair<u32, u32>, std::vector<const ReplayResults::Result*>> replayed;
    for (const auto& x : r->results) {
        replayed[{x.kind, x.addr}].push_back(&x);
    }
    std::map<std::pair<u32, u32>, size_t> taken;
    const QString where = "Replay";
    QString t = QString("recording %1 results (PRES), %2 %3 (RPRS, %4)%5\n")
                    .arg(f.results.size())
                    .arg(where)
                    .arg(r->results.size())
                    .arg(QString::fromStdString(r->path))
                    .arg(f.has_rasterizer_state ? ""
                                                : "; the capture has no renderer state (RSTA)");
    t += "   #  op  kind      address     size      differing pixels     max  replay #\n";
    u32 same = 0, compared = 0;
    for (const Result& x : f.results) {
        const auto key = std::make_pair(x.kind, x.addr);
        const auto& list = replayed[key];
        const size_t k = taken[key]++;
        QString line = QString("%1 %2  %3  %4  %5x%6")
                           .arg(x.index, 4)
                           .arg(x.seq, 3)
                           .arg(x.kind == 0 ? "pass    " : "transfer")
                           .arg(Hex(x.addr), 10)
                           .arg(x.width, 4)
                           .arg(x.height, -4);
        if (k >= list.size()) {
            t += line + "  not in the replay\n";
            continue;
        }
        const ReplayResults::Result& y = *list[k];
        size_t n = 0;
        const u8* p = cap->Blob(x.blob, &n);
        const auto a = ResultRgba(x.kind, x.format, x.width, x.height, p, p ? n : 0);
        const auto b = ResultRgba(y.kind, y.format, y.width, y.height, y.bytes.data(),
                                  y.bytes.size());
        if (a.empty() || b.empty() || x.width != y.width || x.height != y.height ||
            x.format != y.format) {
            t += line + QString("  cannot compare (replay %1x%2 format %3, %4 bytes)\n")
                            .arg(y.width).arg(y.height).arg(y.format).arg(y.bytes.size());
            continue;
        }
        const size_t pixels = size_t(x.width) * x.height;
        size_t differ = 0;
        int max = 0;
        std::vector<u8> diff(pixels * 4);
        for (size_t i = 0; i < pixels; i++) {
            int m = 0;
            for (int c = 0; c < 4; c++) {
                m = std::max(m, std::abs(int(a[i * 4 + c]) - int(b[i * 4 + c])));
            }
            differ += m != 0;
            max = std::max(max, m);
            // Differing pixels red by how far apart; the rest the console's picture, dimmed.
            u8* d = &diff[i * 4];
            if (m != 0) {
                d[0] = u8(std::min(255, 96 + m * 4)), d[1] = 0, d[2] = 0;
            } else {
                d[0] = d[1] = d[2] = u8((a[i * 4] + a[i * 4 + 1] + a[i * 4 + 2]) / 12);
            }
            d[3] = 255;
        }
        compared++;
        same += differ == 0;
        t += line + QString("  %1 (%2%)  %3  %4\n")
                        .arg(differ, 8)
                        .arg(100.0 * double(differ) / double(pixels), 6, 'f', 2)
                        .arg(max, 5)
                        .arg(y.index, 4);
        if (!images.isEmpty()) {
            const QString base = QString("%1/check-%2").arg(images).arg(x.index, 3, 10, QChar('0'));
            SavePng(a, x.width, x.height, base + "-recorded.png");
            SavePng(b, y.width, y.height, base + "-replay.png");
            SavePng(diff, x.width, x.height, base + "-diff.png");
        }
    }
    for (const auto& [key, list] : replayed) {
        const size_t k = taken[key];
        for (size_t i = k; i < list.size(); i++) {
            t += QString("   replay #%1: %2 %3 %4x%5 has no recorded result\n")
                     .arg(list[i]->index)
                     .arg(key.first == 0 ? "pass" : "transfer")
                     .arg(Hex(key.second))
                     .arg(list[i]->width)
                     .arg(list[i]->height);
        }
    }
    t += QString("%1 of %2 compared results are identical\n").arg(same).arg(compared);
    if (!images.isEmpty()) {
        t += "saved check-N-recorded.png, -replay.png and -diff.png in " + images + "\n";
    }
    return t;
}

std::string Session::TraceJson() const {
    // Chrome trace events (ui.perfetto.dev, chrome://tracing): the GPU's TA and 3D
    // spans, sgxd's pass kicks and the vblanks on tracks of their own, and each thread's paired events as spans, the others as instants.
    // Times are the capture's clock (microseconds).
    std::string out = "{\"traceEvents\":[\n";
    bool first = true;
    auto put = [&](const std::string& e) {
        out += first ? "" : ",\n";
        out += e;
        first = false;
    };
    auto name_track = [&](u32 tid, const std::string& name) {
        put(std::format("{{\"ph\":\"M\",\"pid\":1,\"tid\":{},\"name\":\"thread_name\","
                        "\"args\":{{\"name\":\"{}\"}}}}",
                        tid, name));
    };
    auto span = [&](u32 tid, const std::string& name, u32 begin, u32 end) {
        put(std::format("{{\"ph\":\"X\",\"pid\":1,\"tid\":{},\"name\":\"{}\",\"ts\":{},"
                        "\"dur\":{}}}",
                        tid, name, begin, end - begin));
    };
    auto instant = [&](u32 tid, const std::string& name, u32 time) {
        put(std::format("{{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":{},\"name\":\"{}\","
                        "\"ts\":{}}}",
                        tid, name, time));
    };
    put("{\"ph\":\"M\",\"pid\":1,\"name\":\"process_name\",\"args\":{\"name\":\"azahar\"}}");
    // Track IDs below the app's thread IDs (kernel UIDs, large).
    constexpr u32 TrackTa = 1, Track3d = 2, TrackPasses = 3, TrackVblank = 4;
    name_track(TrackTa, "GPU TA");
    name_track(Track3d, "GPU 3D");
    name_track(TrackPasses, "sgxd passes");
    name_track(TrackVblank, "vblanks");
    const int ta = cap->TagId("gpu_ta"), ta_end = cap->TagId("gpu_ta_end");
    const int d3 = cap->TagId("gpu_3d"), d3_end = cap->TagId("gpu_3d_end");
    const int vbl = cap->TagId("vblank"), pass = cap->TagId("pass");
    static const std::map<std::string, std::string> pairs = {
        {"op", "done"}, {"present", "presented"}, {"sync", "sync_end"},
        {"sgx_present", "sgx_present_end"}};
    std::map<int, int> end_of; // end tag -> begin tag
    for (const auto& [b, e] : pairs) {
        const int bi = cap->TagId(b), ei = cap->TagId(e);
        if (bi >= 0 && ei >= 0) {
            end_of[ei] = bi;
        }
    }
    std::map<std::pair<u32, int>, std::pair<u32, u32>> open; // (tid, begin tag) -> (time, arg)
    std::set<u32> named;
    u32 open_ta = 0, open_3d = 0;
    for (const Event& e : cap->events) {
        const int t = int(e.tag);
        if (t == ta) {
            open_ta = e.arg;
        } else if (t == ta_end) {
            if (open_ta && e.arg >= open_ta) {
                span(TrackTa, "TA", open_ta, e.arg);
            }
            open_ta = 0;
        } else if (t == d3) {
            open_3d = e.arg;
        } else if (t == d3_end) {
            if (open_3d && e.arg >= open_3d) {
                span(Track3d, "3D", open_3d, e.arg);
            }
            open_3d = 0;
        } else if (t == vbl) {
            instant(TrackVblank, "vblank", e.time);
        } else if (t == pass) {
            instant(TrackPasses, std::format("pass {} kicked", e.arg), e.time);
        } else {
            if (named.insert(e.tid).second) {
                const auto it = cap->threads.find(e.tid);
                name_track(e.tid, it != cap->threads.end() ? it->second
                                                           : std::format("thread {:#x}", e.tid));
            }
            const std::string& name = t < int(cap->tags.size()) ? cap->tags[t] : std::string("?");
            if (pairs.count(name)) {
                open[{e.tid, t}] = {e.time, e.arg};
                continue;
            }
            if (const auto ei = end_of.find(t); ei != end_of.end()) {
                if (const auto o = open.find({e.tid, ei->second}); o != open.end()) {
                    const std::string& begin = cap->tags[ei->second];
                    span(e.tid, begin == "op" ? OpKind(o->second.second) : begin,
                         o->second.first, e.time);
                    open.erase(o);
                }
                continue;
            }
            instant(e.tid, std::format("{} {}", name, e.arg), e.time);
        }
    }
    out += "\n]}\n";
    return out;
}

} // namespace frame_replay

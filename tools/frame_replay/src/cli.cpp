// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// frame-replay: an sgxd frame capture of azahar's diagnostics build, and its replay on the console's
// replay player, as text (session.h); the GPU timeline as a Chrome trace for Perfetto.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <cstdio>
#include <optional>
#include "replay.h"
#include "session.h"

using namespace frame_replay;

namespace {

const char* const Usage = R"(usage: frame-replay COMMAND CAPTURE [options]

CAPTURE is a capture the diagnostics build wrote (sgxd_flags vprof; record.sh); the replay
player's results load with it from CAPTURE.player.bin.

  summary CAPTURE                 its info, vblank and present intervals, the GPU's busy time
  trace CAPTURE OUT.json          the GPU's TA and 3D spans, sgxd's passes, the vblanks and the
                                  threads' events as a Chrome trace (ui.perfetto.dev)
  frame CAPTURE                   the recorded frame: GSP commands, writes, DMAs, ops, draws, passes
  draw CAPTURE N [--images DIR]   draw N: state, registers, shaders; its textures and target as PNG
  op CAPTURE SEQ                  op SEQ's memory loads
  blob CAPTURE ID OUT             a blob's bytes (a load's contents, op names them) into OUT
  replay CAPTURE [--draw N] [--images DIR]
                                  the replay's results: pass times, profile runs, costs, targets
  check CAPTURE [--images DIR]    the recording's pass and transfer results against the replay's
                                  (request "results"): differing pixels, PNGs, a diff
  results CAPTURE [--images DIR]  the recording's pass and transfer results alone (no replay)
)";

void Out(const QString& s) {
    const QByteArray b = s.toUtf8();
    std::fwrite(b.data(), 1, b.size(), stdout);
    if (!s.endsWith('\n')) {
        std::fputc('\n', stdout);
    }
}

int Fail(const QString& s) {
    std::fprintf(stderr, "frame-replay: %s\n", s.toUtf8().constData());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList raw = app.arguments().mid(1);
    if (raw.isEmpty() || raw[0] == "-h" || raw[0] == "--help" || raw[0] == "help") {
        Out(Usage);
        return raw.isEmpty() ? 2 : 0;
    }
    const QString cmd = raw[0];
    QStringList pos;
    std::map<QString, QString> opt;
    for (int i = 1; i < raw.size(); i++) {
        if (raw[i] == "--images" || raw[i] == "--draw") {
            if (i + 1 >= raw.size()) {
                return Fail(raw[i] + " needs a value");
            }
            opt[raw[i]] = raw[i + 1];
            i++;
        } else if (raw[i].startsWith("--")) {
            return Fail("unknown option " + raw[i]);
        } else {
            pos << raw[i];
        }
    }
    if (pos.isEmpty()) {
        return Fail("which capture? (frame-replay help)");
    }
    Session s;
    std::string err;
    if (!s.Open(pos[0], &err)) {
        return Fail(pos[0] + ": " + QString::fromStdString(err));
    }
    const QString images = opt.count("--images") ? opt["--images"] : QString();
    if (!images.isEmpty()) {
        QDir().mkpath(images);
    }

    if (cmd == "summary") {
        Out(s.SummaryText());
        return 0;
    }
    if (cmd == "trace") {
        if (pos.size() < 2) {
            return Fail("trace CAPTURE OUT.json");
        }
        QFile f(pos[1]);
        const std::string json = s.TraceJson();
        if (!f.open(QIODevice::WriteOnly) || f.write(json.data(), qint64(json.size())) < 0) {
            return Fail("cannot write " + pos[1]);
        }
        Out("trace: " + pos[1] + " (ui.perfetto.dev)");
        return 0;
    }
    if (cmd == "frame") {
        Out(s.FrameText());
        return 0;
    }
    if (cmd == "draw") {
        if (pos.size() < 2) {
            return Fail("which draw? (frame-replay frame lists them)");
        }
        const size_t n = pos[1].toULong();
        Out(s.DrawText(n));
        int i = 0;
        for (const DrawImage& im : s.DrawImages(n)) {
            QString line = "picture: " + im.title;
            if (!images.isEmpty()) {
                const QString png = QString("%1/draw%2-%3.png").arg(images).arg(n).arg(i++);
                line += !im.image.isNull() && im.image.save(png) ? " -> " + png : " (not decoded)";
            }
            Out(line);
        }
        Out(s.ReplayText(n));
        return 0;
    }
    if (cmd == "op") {
        if (pos.size() < 2) {
            return Fail("which op? (frame-replay frame lists them)");
        }
        Out(s.OpLoadsText(pos[1].toUInt()));
        return 0;
    }
    if (cmd == "blob") {
        if (pos.size() < 3) {
            return Fail("blob CAPTURE ID OUT (op lists an op's blobs)");
        }
        size_t n = 0;
        const u8* const p = s.cap->Blob(pos[1].toUInt(), &n);
        QFile f(pos[2]);
        if (p == nullptr || !f.open(QIODevice::WriteOnly) ||
            f.write(reinterpret_cast<const char*>(p), qint64(n)) != qint64(n)) {
            return Fail("no blob " + pos[1] + " or cannot write " + pos[2]);
        }
        Out(QString("blob %1: %2 bytes -> %3").arg(pos[1]).arg(n).arg(pos[2]));
        return 0;
    }
    if (cmd == "replay") {
        std::optional<size_t> draw;
        if (opt.count("--draw")) {
            draw = opt["--draw"].toULong();
        }
        Out(s.ReplayText(draw));
        const ReplayResults* r = Replays(s.cap.get());
        if (r && !images.isEmpty()) {
            for (const auto& [d, im] : r->images) {
                if ((draw && d != *draw) || im.rgba.empty()) {
                    continue;
                }
                const QImage q(im.rgba.data(), int(im.width), int(im.height), int(im.width * 4),
                               QImage::Format_RGBA8888);
                const QString png = QString("%1/replay-draw%2.png").arg(images).arg(d);
                Out(q.copy().save(png) ? "saved " + png : "cannot save " + png);
            }
        }
        return 0;
    }
    if (cmd == "check") {
        Out(s.CheckText(images));
        return 0;
    }
    if (cmd == "results") {
        Out(s.ResultsText(images));
        return 0;
    }
    return Fail("unknown command " + cmd + " (frame-replay help)");
}

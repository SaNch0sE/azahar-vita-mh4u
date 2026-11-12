#!/bin/sh
# Host builds of the GXM backend's USSE emitters, for configurations dumped on the console
# with the usse_dump switch: fragment programs as ux0:data/azahar/usse/us_<hash>.fq, tier-2
# vertex requests as vr_<key>.bin.
#   ./fs_emit us_<hash>.fq [level]   ./vs_emit vr_<key>.bin [level]
# USSE_IR_DUMP=1 prints the IR after every pass and, last, with registers placed; GXM_FLAGS
# takes the console's switch words (for example "usse_vsnoloadsrc usse_stop6").
# check_load_sources.py <dump> checks LoadSources' reader rewrites lane by lane.
set -e
A=$(cd "$(dirname "$0")/../.." && pwd)
U=$A/src/video_core/renderer_gxm/usse
FLAGS="-std=gnu++20 -O0 -g -w -I$A/src -I$A/externals/fmt/include -I$A/externals/boost -I$A/externals/xxHash -I$A/externals/nihstro/include -DFMT_HEADER_ONLY"
D=$(dirname "$0")
g++ $FLAGS $D/fs_emit.cpp $U/fs_usse_gen.cpp $U/usse_ir.cpp $U/usse_encoder.cpp $U/gxp_writer.cpp -o fs_emit
g++ $FLAGS $D/vs_emit.cpp $D/stub_flags.cpp $D/stub_log.cpp $U/vs_usse_gen.cpp $U/usse_ir.cpp $U/usse_encoder.cpp $U/gxp_writer.cpp -o vs_emit

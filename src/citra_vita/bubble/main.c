// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// A pinned title's LiveArea bubble (livearea.cpp): it starts Azahar with the title's path, kept
// in its own title.txt, and ends. Azahar takes the path from the launch URI's param.

#include <stdio.h>
#include <string.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

int main(void) {
    char path[900] = {0};
    const SceUID fd = sceIoOpen("app0:title.txt", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        const int got = sceIoRead(fd, path, sizeof(path) - 1);
        sceIoClose(fd);
        path[got > 0 ? got : 0] = '\0';
    }
    char uri[1024];
    snprintf(uri, sizeof(uri), "psgm:play?titleid=%s&param=%s", AZAHAR_VITA_TITLEID, path);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelExitProcess(0);
    return 0;
}

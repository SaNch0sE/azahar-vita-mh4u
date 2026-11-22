// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"

namespace Core {
class System;
} // namespace Core

namespace Kernel {

class SVC;

class SVCContext {
public:
    SVCContext(Core::System& system);
    ~SVCContext();
    void CallSVC(u32 immediate);
    /// CallSVC without the perf stats' clock: the Vita native core's upcalls, which run the svc
    /// in the guest's world, where reading the clock is a syscall.
    void CallSVCForUpcall(u32 immediate);
    /// The second emulation thread's svcs it answers itself (emu_host.h): the ones that touch
    /// only its own cores' threads and events, which need no lock (Event's lists are each
    /// host's). Every other svc it hands to host 0 (System::SecondHostSvc).
    [[nodiscard]] bool LocalToSecondHost(u32 immediate);
    /// CallSVC for those, with no HLE lock.
    void CallSVCLocal(u32 immediate);

private:
    std::unique_ptr<SVC> impl;
};

class SVC_SyncCallback;
class SVC_IPCCallback;

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::SVC_SyncCallback)
BOOST_CLASS_EXPORT_KEY(Kernel::SVC_IPCCallback)

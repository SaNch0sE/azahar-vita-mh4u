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

private:
    std::unique_ptr<SVC> impl;
};

class SVC_SyncCallback;
class SVC_IPCCallback;

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::SVC_SyncCallback)
BOOST_CLASS_EXPORT_KEY(Kernel::SVC_IPCCallback)

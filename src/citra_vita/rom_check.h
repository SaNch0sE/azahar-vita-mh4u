// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <string>

/**
 * Whether a game's file is whole: every part of it against the SHA-256 checksums the 3DS formats
 * carry, read through Z3DS where the file is compressed. A cartridge dump (NCSD: .3ds, .cci) is
 * its partitions; each NCCH (the game, its manual, its Download Play child, the update data; a
 * .cxi or an installed title's .app alone) its extended header, logo, ExeFS header and each ExeFS
 * file, and its RomFS's hash tree (IVFC) from the header's master hashes down to every data
 * block. A CIA is its contents against its TMD's records. A part past the file's end is a copy cut
 * short. The checksums are of the decrypted data: an encrypted file is not checked, and neither
 * is a format without checksums (3DSX, ELF).
 */
namespace VitaFrontend::RomCheck {

enum class Outcome {
    Intact,
    Damaged,
    CutShort,
    Encrypted,
    NoChecksums,
    Unreadable,
};

struct Result {
    Outcome outcome;
    /// Damaged and CutShort: the part, in English for the menus to translate ("the game", "the
    /// manual", ...).
    const char* part = nullptr;
    /// What exactly differed, for the log.
    std::string detail;
};

/// Reads `path` whole and checks it. `progress` gets the share done (0 to 1) every tenth of a
/// second or so, on the calling thread.
Result Check(const std::string& path, const std::function<void(float)>& progress);

} // namespace VitaFrontend::RomCheck

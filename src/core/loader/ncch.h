// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include "common/common_types.h"
#include "common/swap.h"
#include "core/file_sys/ncch_container.h"
#include "core/loader/loader.h"

namespace Loader {

/// Loads an NCCH file (e.g. from a CCI, or the first NCCH in a CXI)
class AppLoader_NCCH final : public AppLoader {
public:
    AppLoader_NCCH(Core::System& system_, FileUtil::IOFile&& file, const std::string& filepath)
        : AppLoader(system_, std::move(file)), base_ncch(filepath), overlay_ncch(&base_ncch),
          filepath(filepath) {
        filetype = IdentifyType(this->file.get());
        this->file.reset();
    }

    /**
     * Returns the type of the file
     * @param file FileUtil::IOFile open file
     * @return FileType found, or FileType::Error if this loader doesn't know it
     */
    static FileType IdentifyType(FileUtil::IOFileBase* file);

    FileType GetFileType() override {
        return filetype;
    }

    [[nodiscard]] std::span<const u32> GetPreferredRegions() const override {
        return preferred_regions;
    }

    ResultStatus Load(std::shared_ptr<Kernel::Process>& process) override;

    void ReapplyCodePatch() override;

    [[nodiscard]] std::vector<bool> TitlePatchesIn() const override {
        return title_patches_in;
    }

    [[nodiscard]] std::string TitlePatchesWarning() const override {
        return title_patches_warning;
    }

    [[nodiscard]] std::string ProductSummary() const override {
        return product_summary;
    }

    std::pair<std::optional<u32>, ResultStatus> LoadCoreVersion() override;

    /**
     * Loads the Exheader and returns the system mode for this application.
     * @returns A pair with the optional system mode, and and the status.
     */
    std::pair<std::optional<Kernel::MemoryMode>, ResultStatus> LoadKernelMemoryMode() override;

    std::pair<std::optional<Kernel::New3dsHwCapabilities>, ResultStatus> LoadNew3dsHwCapabilities()
        override;

    bool IsN3DSExclusive() override;

    ResultStatus IsExecutable(bool& out_executable) override;

    ResultStatus ReadCode(std::vector<u8>& buffer) override;

    ResultStatus ReadIcon(std::vector<u8>& buffer) override;

    ResultStatus ReadBanner(std::vector<u8>& buffer) override;

    ResultStatus ReadLogo(std::vector<u8>& buffer) override;

    ResultStatus ReadProgramId(u64& out_program_id) override;

    ResultStatus ReadExtdataId(u64& out_extdata_id) override;

    ResultStatus ReadRomFS(std::shared_ptr<FileSys::RomFSReader>& romfs_file) override;

    ResultStatus ReadUpdateRomFS(std::shared_ptr<FileSys::RomFSReader>& romfs_file) override;

    ResultStatus DumpRomFS(const std::string& target_path) override;

    ResultStatus DumpUpdateRomFS(const std::string& target_path) override;

    ResultStatus ReadTitle(std::string& title) override;

    CompressFileInfo GetCompressFileInfo() override;

    bool IsFileCompressed() override;

    std::string GetFilePath() override {
        return filepath;
    }

private:
    /**
     * Loads .code section into memory for booting
     * @param process The newly created process
     * @return ResultStatus result of function
     */
    ResultStatus LoadExec(std::shared_ptr<Kernel::Process>& process);

    /// Reads the region lockout info in the SMDH and send it to CFG service
    /// If an SMDH is not present, the program ID is compared against a list
    /// of known system titles to determine the region.
    void ParseRegionLockoutInfo(u64 program_id);

    /// Detects whether the NCCH contains GBA Virtual Console.
    bool IsGbaVirtualConsole(std::span<const u8> code);

    /// Keeps the runs of bytes a code patch changed in .code's read-only segments.
    void NoteCodePatch(std::span<const u8> original, std::span<const u8> patched, VAddr base);

    /// A run of bytes a code patch changed, from the text segment's address.
    struct CodePatchRun {
        u32 offset;
        std::vector<u8> bytes;
    };
    std::vector<CodePatchRun> code_patch_runs;
    VAddr code_patch_base = 0;
    /// The loaded title's (ReapplyCodePatch finds its process by it).
    u64 loaded_program_id = 0;
    /// ApplyTitlePatches's, at the load or the last state's load.
    std::vector<bool> title_patches_in;
    std::string title_patches_warning;
    std::string product_summary;

    FileSys::NCCHContainer base_ncch;
    FileSys::NCCHContainer update_ncch;
    FileSys::NCCHContainer* overlay_ncch;

    std::vector<u32> preferred_regions;

    std::string filepath;
    FileType filetype;
};

} // namespace Loader

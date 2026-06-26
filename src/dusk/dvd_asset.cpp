#include "dusk/dvd_asset.hpp"
#include "dusk/logging.h"
#include "dusk/endian.h"
#include "dolphin/dvd.h"
#include "DynamicLink.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRDvdRipper.h"

#include <cstring>

#ifdef DUSK_TPHD
#include <zlib.h>
#include <fstream>
#endif

namespace dusk {

static const u8* s_dolData = nullptr; // pointer to dol data
static size_t    s_dolSize = 0;

struct DolHeader {
    BE(u32) textOffset[7];
    BE(u32) dataOffset[11];
    BE(u32) textAddr[7];
    BE(u32) dataAddr[11];
    BE(u32) textSize[7];
    BE(u32) dataSize[11];
};

struct DolSection {
    u32 fileOffset;
    u32 vaddr;
    u32 size;
};

static DolSection s_dolSections[18]; // 7 text + 11 data
static int        s_dolSectionCount = 0;

static bool EnsureDolParsed() {
    if (s_dolData) return true;

    s32 sz = 0;
    const u8* p = DVDGetDOLLocation(&sz);
    if (!p || sz < 256) {
        DuskLog.fatal("dvd_asset: DVDGetDOLLocation failed (size={})", sz);
        return false;
    }
    
    s_dolData = p;
    s_dolSize = sz;

    const auto* hdr = (const DolHeader*)s_dolData;
    s_dolSectionCount = 0;

    for (int i = 0; i < 7;  i++) {
        u32 off = hdr->textOffset[i];
        u32 addr = hdr->textAddr[i];
        u32 sz = hdr->textSize[i];
        if (sz > 0 && off > 0) {
            s_dolSections[s_dolSectionCount++] = {off, addr, sz};
        }
    }

    for (int i = 0; i < 11; i++) {
        u32 off = hdr->dataOffset[i];
        u32 addr = hdr->dataAddr[i];
        u32 sz = hdr->dataSize[i];
        if (sz > 0 && off > 0) {
            s_dolSections[s_dolSectionCount++] = {off, addr, sz};
        }
    }

    return true;
}

static s32 DolVaToFileOffset(u32 va) {
    if (!EnsureDolParsed()) return -1;
    for (int i = 0; i < s_dolSectionCount; i++) {
        const auto& sec = s_dolSections[i];
        if (va >= sec.vaddr && va < sec.vaddr + sec.size) {
            return static_cast<s32>(sec.fileOffset + (va - sec.vaddr));
        }
    }
    DuskLog.fatal("dvd_asset: VA 0x{:08X} not found in any DOL section", va);
    return -1;
}

static u32 GetOffsetForVersion(std::initializer_list<OffsetVersion> version) {
    const auto gameVersion = dusk::version::getGameVersion();
    for (auto elem : version) {
        if (elem.mGameVersion == gameVersion) {
            return elem.mOffset;
        }
    }

    DuskLog.fatal("Unable to find offset for this game version!");
}

bool LoadDolAsset(void* dst, std::initializer_list<OffsetVersion> virtualAddress, s32 size) {
    s32 fileOffset = DolVaToFileOffset(GetOffsetForVersion(virtualAddress));
    if (fileOffset < 0) {
        return false;
    }

    if (size <= 0 || (size_t)(fileOffset + size) > s_dolSize) {
        DuskLog.fatal("dvd_asset: DOL read out of range (offset={:#x} size={:#x} dolSize={})", fileOffset, size, s_dolSize);
        return false;
    }

    std::memcpy(dst, s_dolData + fileOffset, size);
    return true;
}

bool LoadRelAsset(void* dst, const char* dvdPath, std::initializer_list<OffsetVersion> offset, s32 size) {
    auto resOffset = GetOffsetForVersion(offset);
    void* p = JKRDvdRipper::loadToMainRAM(dvdPath, (u8*)dst, EXPAND_SWITCH_UNKNOWN1, (u32)size, nullptr, JKRDvdRipper::ALLOC_DIRECTION_FORWARD, resOffset, nullptr, nullptr);
    if (!p) DuskLog.fatal("dvd_asset: failed to load {} (offset={:#x} size={:#x})", dvdPath, resOffset, size);
    return p != nullptr;
}

bool LoadArchivedRelAsset(void* dst, u32 memType, const char* relFileName, std::initializer_list<OffsetVersion> offset, s32 size) {
    auto resOffset = GetOffsetForVersion(offset);

    // On TARGET_PC, cDyl_InitCallback skips DynamicModuleControl::initialize() due to static linking
    // Mount RELS.arc on first use so sArchive is available
    static bool s_mountAttempted = false;
    if (!DynamicModuleControl::sArchive && !s_mountAttempted) {
        s_mountAttempted = true; DynamicModuleControl::initialize();
    }

    if (!DynamicModuleControl::sArchive) {
        DuskLog.fatal("dvd_asset: RELS archive not mounted"); return false;
    }

    const u8* rel = static_cast<const u8*>(DynamicModuleControl::sArchive->getResource(memType, relFileName));
    if (!rel) {
        DuskLog.fatal("dvd_asset: {} not found in RELS archive", relFileName); return false;
    }

    std::memcpy(dst, rel + resOffset, size);
    return true;
}

#ifdef DUSK_TPHD
enum struct SectionType : uint32_t // sh_type
{
    // https://en.wikipedia.org/wiki/Executable_and_Linkable_Format
    SHT_NULL = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHT_HASH = 5,
    SHT_DYNAMIC = 6,
    SHT_NOTE = 7,
    SHT_NOBITS = 8,
    SHT_REL = 9,
    SHT_SHLIB = 10,
    SHT_DYNSYM = 11,
    SHT_INIT_ARRAY = 14,
    SHT_FINI_ARRAY = 15,
    SHT_PREINIT_ARRAY = 16,
    SHT_GROUP = 17,
    SHT_SYMTAB_SHNDX = 18,

    // https://refspecs.linuxfoundation.org/LSB_2.1.0/LSB-Core-generic/LSB-Core-generic/elftypes.html
    SHT_LOPROC = 0x70000000,
    SHT_HIPROC = 0x7fffffff,
    SHT_LOUSER = 0x80000000,
    SHT_HIUSER = 0xffffffff,

    // from https://gist.github.com/exjam/b4290ad23828cbc04db4 and looking at the rpx itself
    SHT_RPL_EXPORTS = 0x80000001,
    SHT_RPL_IMPORTS = 0x80000002,
    SHT_RPL_CRCS = 0x80000003,
    SHT_RPL_FILEINFO = 0x80000004
};

enum struct SectionFlags : uint32_t {
    // https://en.wikipedia.org/wiki/Executable_and_Linkable_Format
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,

    // from https://gist.github.com/exjam/b4290ad23828cbc04db4 and looking at the rpx itself
    SHF_DEFLATED = 0x08000000
};

struct Elf32_Ehdr {
    uint8_t e_ident[0x10];
    BE(u16) e_type;
    BE(u16) e_machine;
    BE(u32) e_version;
    BE(u32) e_entry;
    BE(u32) e_phoff;
    BE(u32) e_shoff;
    BE(u32) e_flags;
    BE(u16) e_ehsize;
    BE(u16) e_phentsize;
    BE(u16) e_phnum;
    BE(u16) e_shentsize;
    BE(u16) e_shnum;
    BE(u16) e_shstrndx;
};

struct Elf32_Shdr {
    BE(u32) sh_name;
    BE(u32) sh_type; // SectionType
    BE(u32) sh_flags;
    BE(u32) sh_addr;
    BE(u32) sh_offset;
    BE(u32) sh_size;
    BE(u32) sh_link;
    BE(u32) sh_info;
    BE(u32) sh_addralign;
    BE(u32) sh_entsize;
};

struct RPXSection {
    u32 vaddr;
    std::string data;
};

static std::vector<RPXSection> g_rpxSections;

static bool EnsureRPXParsed() {
    if (!g_rpxSections.empty()) return true;

    std::ifstream rpx(dusk::tphd_content_path().parent_path() / "code" / "Zelda.rpx", std::ios::binary);
    if (!rpx.is_open()) {
        DuskLog.fatal("dvd_asset: Failed to open Zelda.rpx");
        return false;
    }

    Elf32_Ehdr ehdr;
    rpx.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));

    for (int i = 0; i < ehdr.e_shnum; i++) {
        rpx.seekg(ehdr.e_shoff + ehdr.e_shentsize * i, std::ios::beg);

        Elf32_Shdr shdr;
        rpx.read(reinterpret_cast<char*>(&shdr), sizeof(shdr));

        if(shdr.sh_addr != 0 && shdr.sh_offset != 0 && (shdr.sh_type & ((int)SectionType::SHT_NULL | (int)SectionType::SHT_NOBITS)) == 0) {
            rpx.seekg(shdr.sh_offset, std::ios::beg);

            RPXSection& section = g_rpxSections.emplace_back();
            section.vaddr = shdr.sh_addr;

            if(shdr.sh_flags & (int)SectionFlags::SHF_DEFLATED) {
                BE(u32) size;
                std::string inData(shdr.sh_size - 4, '\0');
                rpx.read(reinterpret_cast<char*>(&size), sizeof(size));
                rpx.read(inData.data(), inData.size());

                section.data.resize(size);

                unsigned long outSize = section.data.size();
                uncompress(reinterpret_cast<unsigned char*>(section.data.data()), &outSize, reinterpret_cast<unsigned char*>(inData.data()), inData.size());
            }
            else {
                section.data.resize(shdr.sh_size);
                rpx.read(section.data.data(), section.data.size());
            }
        }
    }

    return true;
}

bool LoadRPXAsset(void* dst, uint32_t virtualAddress, s32 size) {
    if(!EnsureRPXParsed()) return false;

    const auto& it = std::ranges::find_if(g_rpxSections, [virtualAddress, size](const RPXSection& section) { return section.vaddr <= virtualAddress && virtualAddress + size <= section.vaddr + section.data.size(); });
    if(it == g_rpxSections.end()) {
        return false;
    }

    it->data.copy(static_cast<char*>(dst), size, virtualAddress - it->vaddr);

    return true;
}
#endif

}  // namespace dusk

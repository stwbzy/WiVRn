#include "installed_contract.h"
#include "compat/elf_build_id.h"

#include <openssl/evp.h>
#include <elf.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
class FileImage {
    std::vector<unsigned char> data;
    std::vector<Elf64_Phdr> segments;
public:
    explicit FileImage(const char* path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Cannot open ELF");
        auto size = input.tellg();
        if (size < static_cast<std::streamoff>(sizeof(Elf64_Ehdr)) || size > 512 * 1024 * 1024)
            throw std::runtime_error("ELF size outside bounds");
        data.resize(static_cast<size_t>(size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(data.data()), size))
            throw std::runtime_error("Truncated ELF read");
        Elf64_Ehdr header;
        std::memcpy(&header, data.data(), sizeof(header));
        if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) ||
            header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
            header.e_machine != EM_X86_64 || header.e_type != ET_DYN ||
            header.e_phentsize != sizeof(Elf64_Phdr))
            throw std::runtime_error("Expected little-endian x86_64 shared ELF");
        if (header.e_phoff > data.size() ||
            header.e_phnum > (data.size() - header.e_phoff) / sizeof(Elf64_Phdr))
            throw std::runtime_error("Program headers outside file");
        segments.resize(header.e_phnum);
        std::memcpy(segments.data(), data.data() + header.e_phoff,
                    segments.size() * sizeof(Elf64_Phdr));
    }
    std::string Sha256() const {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int count = 0;
        if (!EVP_Digest(data.data(), data.size(), digest.data(), &count, EVP_sha256(), nullptr))
            throw std::runtime_error("SHA-256 failed");
        constexpr char hex[] = "0123456789abcdef";
        std::string value;
        for (unsigned int i = 0; i < count; ++i) {
            value += hex[digest[i] >> 4]; value += hex[digest[i] & 15];
        }
        return value;
    }
    bool Range(uint64_t rva, size_t size, uint32_t type, uint32_t flags) const {
        for (const auto& p : segments)
            if (p.p_type == type && (p.p_flags & flags) == flags && rva >= p.p_vaddr &&
                rva - p.p_vaddr <= p.p_memsz && size <= p.p_memsz - (rva - p.p_vaddr))
                return true;
        return false;
    }
    const unsigned char* Code(uint64_t rva, size_t size) const {
        for (const auto& p : segments) {
            if (p.p_type != PT_LOAD || (p.p_flags & (PF_R | PF_X)) != (PF_R | PF_X) ||
                rva < p.p_vaddr || rva - p.p_vaddr > p.p_filesz ||
                size > p.p_filesz - (rva - p.p_vaddr)) continue;
            const auto delta = rva - p.p_vaddr;
            if (p.p_offset > data.size() || delta > data.size() - p.p_offset ||
                size > data.size() - p.p_offset - delta)
                throw std::runtime_error("Code segment outside file");
            return data.data() + p.p_offset + delta;
        }
        throw std::runtime_error("Code RVA outside executable file segment");
    }
};
void Require(bool valid, const char* name, std::ostream* output) {
    if (output) *output << "check=" << name << " valid=" << valid << '\n';
    if (!valid) throw std::runtime_error(std::string("Contract rejected: ") + name);
}
}

std::string wivrn::roblox::VerifyInstalledStaticPayload(const std::string& path, std::ostream* output) {
    try {
        using namespace mocktail::vr::internal;
        const auto& c = wivrn::roblox::installed_contract;
        auto id = mocktail::compat::ReadElfBuildId(path);
        Require(id && id.build_id == c.build_id, "build_id", output);
        FileImage image(path.c_str());
        auto hash = image.Sha256();
        if (output) *output << "sha256=" << hash << '\n';
        Require(hash == wivrn::roblox::installed_sha256, "payload_hash", output);
        Require(image.Range(c.device.vtable_rva, 0xd8, PT_GNU_RELRO, 0), "vtable_relro", output);
        Require(image.Range(c.device.emulator_flag_storage_rva, 1, PT_LOAD, PF_R | PF_W) &&
                image.Range(c.hand_pitch_storage_rva, 4, PT_LOAD, PF_R | PF_W), "flag_storage", output);
        Require(HasExpectedVrStateGetterContract(image.Code(c.device.state_getter_rva, kStateGetterContractSize),
                kStateGetterContractSize, c), "state_getter", output);
        Require(HasExpectedVrEyeGetterContract(image.Code(c.device.eye_getter_rva, kEyeGetterContractSize),
                kEyeGetterContractSize), "eye_getter", output);
        Require(HasExpectedVrConstructorContract(image.Code(c.device.constructor_rva, kConstructorContractSize),
                kConstructorContractSize, c.device), "constructor", output);
        Require(HasExpectedVrEyeInitializerContract(image.Code(c.device.eye_initializer_rva, kInitializerContractSize),
                kInitializerContractSize, c.device), "eye_initializer", output);
        Require(HasExpectedVrPointerContract(image.Code(c.pointer_frame_rva, 0x1b2), 0x1b2, c), "world_pointer", output);
        Require(*image.Code(c.haptic_sink_rva, 1) == 0xc3, "haptic_sink", output);
        image.Code(c.state_memcpy_rva, 1);
        image.Code(c.workspace_getter_rva, 1);
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

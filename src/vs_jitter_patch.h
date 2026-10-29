#pragma once

// DLAA jitter: a patched copy of a vertex shader whose clip-space position gets a per-draw offset
// (research/dlaa.md, "Method: patch the vertex shaders' position output"). Every write to the
// position output goes to a temp register the shader never uses instead, and the shader ends with
//   mad oPos.xy, rT.wwww, cJ.xyyy, rT.xyyy
//   mov oPos.zw, rT
// so oPos.xy += J.xy * oPos.w, J being a constant register the shader cannot reach. The game's own
// constants are untouched. Shader model 1-3 bytecode (the D3D9 token format); anything the walker
// does not understand leaves the shader unpatched. No Windows dependency: tests/vs_patch_test.cpp
// runs the same code over the shaders in the game's packages.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vsjitter {

// J is c255, the last of the 256 float constants D3D9 hardware (and DXVK) give every vertex shader
// model; vs_1_x only guarantees 96, and a device that refuses the patched copy gets the original.
// tests/vs_jitter_test.cpp checks that no vertex shader in the game's packages can reach it
// (research/dlaa.md, "The free constant register").
constexpr int kJitterRegister = 255;

enum Result
{
    kPatched,
    kNotVertexShader,
    kUnsupportedVersion,
    kMalformed,           // no end token within the limit, or a parameter token where an instruction was expected
    kUnknownOpcode,       // vs_1_x instruction outside the table below (no length field before vs_2_0)
    kNoPosition,          // never writes the position output
    kPartialPosition,     // writes only some of oPos.xyzw
    kRelativeOutput,      // o[aL] could be the position
    kNoFreeTemp,
    kJitterRegisterUsed,  // reads cJ directly, defines it, or a constant table range covers it
    kUnknownRelative,     // c[a0 + n] with no constant table range for n: its reach is unknown
    kExtendedConstants,   // constant register types past c255 (software vertex processing)
    kResultCount
};

inline const char* ResultName(Result result)
{
    static const char* const names[kResultCount] = {"patched", "not a vertex shader", "unsupported version", "malformed",
        "unknown opcode", "no position output", "partial position write", "relative output", "no free temp register",
        "jitter register in use", "relative constant out of any table range", "extended constant registers"};
    return result < kResultCount ? names[result] : "?";
}

struct Info
{
    Result result = kMalformed;
    std::uint32_t version = 0;     // the version token
    int temp = -1;                 // rT
    int jitter_register = -1;      // cJ
    int max_constant = -1;         // highest float constant the shader can reach (direct, def, table ranges)
    int position_writes = 0;
    bool constant_table = false;
    std::size_t tokens = 0;        // the original's length in tokens, end token included
};

// Register types (D3DSHADER_PARAM_REGISTER_TYPE) and opcodes (D3DSHADER_INSTRUCTION_OPCODE_TYPE).
enum : std::uint32_t
{
    kRegTemp = 0, kRegInput = 1, kRegConst = 2, kRegAddr = 3, kRegRastOut = 4, kRegOutput = 6,
    kRegConst2 = 11, kRegConst3 = 12, kRegConst4 = 13,
};
enum : std::uint32_t
{
    kOpMov = 1, kOpMad = 4, kOpCall = 25, kOpCallNz = 26, kOpLoop = 27, kOpRet = 28, kOpEndLoop = 29,
    kOpLabel = 30, kOpDcl = 31, kOpRep = 38, kOpEndRep = 39, kOpIf = 40, kOpIfC = 41, kOpEndIf = 43,
    kOpDefB = 47, kOpDefI = 48, kOpDef = 81, kOpComment = 0xFFFE, kEndToken = 0x0000FFFF,
};

inline std::uint32_t RegisterType(std::uint32_t p) { return ((p >> 28) & 7) | ((p >> 8) & 0x18); }
inline std::uint32_t RegisterNumber(std::uint32_t p) { return p & 0x7FF; }
inline bool IsRelative(std::uint32_t p) { return (p & 0x2000) != 0; }

inline std::uint32_t Param(std::uint32_t type, std::uint32_t number, std::uint32_t mask_or_swizzle)
{
    return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | (number & 0x7FF) | (mask_or_swizzle << 16);
}

// Parameter tokens of a vs_1_x instruction, which has no length field. 0: not in the table.
inline int Sm1Length(std::uint32_t opcode)
{
    switch (opcode) {
    case 0: return 0;                                                  // nop
    case 1: case 6: case 7: case 14: case 15: case 16: case 19:        // mov rcp rsq exp log lit frc
    case 78: case 79: case kOpDcl:                                     // expp logp dcl
        return 2;
    case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: // add sub mul dp3 dp4 min max slt
    case 13: case 17: case 20: case 21: case 22: case 23: case 24:     // sge dst m4x4 m4x3 m3x4 m3x3 m3x2
        return 3;
    case kOpMad: return 4;
    case kOpDef: return 5;
    default: return -1;
    }
}

// Length in tokens of the shader at `code`, end token included, reading at most `limit` tokens.
// Comment blocks can hold an end-token value, so they are skipped by their length. 0: no end.
inline std::size_t TokenCount(const std::uint32_t* code, std::size_t limit)
{
    std::size_t i = 1;
    while (i < limit) {
        const std::uint32_t t = code[i];
        if (t == kEndToken) {
            return i + 1;
        }
        i += (t & 0xFFFF) == kOpComment ? 1 + ((t >> 16) & 0x7FFF) : 1;
    }
    return 0;
}

struct TableRange
{
    int first;
    int count;
};

// Float4 ranges of a constant table comment ('CTAB', D3DXSHADER_CONSTANTTABLE).
inline void ReadConstantTable(const std::uint32_t* comment, std::uint32_t words, std::vector<TableRange>* ranges)
{
    if (words < 8 || comment[0] != 0x42415443u) {   // 'CTAB'
        return;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(comment + 1);
    const std::size_t size = (words - 1) * 4;
    auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(base[at] | (base[at + 1] << 8) | (base[at + 2] << 16) | (base[at + 3] << 24)); };
    auto u16 = [&](std::size_t at) { return static_cast<std::uint32_t>(base[at] | (base[at + 1] << 8)); };
    const std::uint32_t count = u32(12);
    const std::uint32_t info = u32(16);
    for (std::uint32_t c = 0; c < count; ++c) {
        const std::size_t at = static_cast<std::size_t>(info) + 20u * c;
        if (at + 20 > size) {
            break;
        }
        if (u16(at + 4) == 2) {   // D3DXRS_FLOAT4
            ranges->push_back({static_cast<int>(u16(at + 6)), static_cast<int>(u16(at + 8))});
        }
    }
}

// Patches `code` (TokenCount tokens) into `out`. On any result but kPatched, `out` is left empty.
inline Info Patch(const std::uint32_t* code, std::size_t count, std::vector<std::uint32_t>* out)
{
    Info info;
    out->clear();
    info.tokens = count;
    if (count < 2 || code[count - 1] != kEndToken) {
        return info;
    }
    const std::uint32_t version = code[0];
    info.version = version;
    if ((version & 0xFFFF0000u) != 0xFFFE0000u) {
        info.result = kNotVertexShader;
        return info;
    }
    const std::uint32_t major = (version >> 8) & 0xFF;
    if (major < 1 || major > 3) {
        info.result = kUnsupportedVersion;
        return info;
    }
    const int max_temps = major == 3 ? 32 : 12;
    info.jitter_register = kJitterRegister;

    bool temp_used[32] = {};
    bool const_used[256] = {};
    std::vector<int> relative_bases;
    std::vector<TableRange> ranges;
    std::vector<std::size_t> position_params;   // token indices to redirect into rT
    std::uint32_t position_type = major == 3 ? kRegOutput : kRegRastOut;
    int position_number = major == 3 ? -1 : 0;
    std::uint32_t position_mask = 0;
    std::size_t insert_at = count - 1;   // before the end token, or before main's ret
    bool main_ended = false;
    int depth = 0;

    std::size_t i = 1;
    while (i < count - 1) {
        const std::uint32_t t = code[i];
        const std::uint32_t opcode = t & 0xFFFF;
        if (opcode == kOpComment) {
            const std::uint32_t words = (t >> 16) & 0x7FFF;
            if (i + 1 + words > count - 1) {
                return info;
            }
            ReadConstantTable(code + i + 1, words, &ranges);
            i += 1 + words;
            continue;
        }
        if ((t & 0x80000000u) != 0) {
            return info;
        }
        int length = 0;
        if (major >= 2) {
            length = static_cast<int>((t >> 24) & 0xF);
        } else {
            length = Sm1Length(opcode);
            if (length < 0) {
                info.result = kUnknownOpcode;
                return info;
            }
        }
        if (i + 1 + length > count - 1) {
            return info;
        }
        const std::uint32_t* p = code + i + 1;

        if (opcode == kOpIf || opcode == kOpIfC || opcode == kOpLoop || opcode == kOpRep) {
            ++depth;
        } else if (opcode == kOpEndIf || opcode == kOpEndLoop || opcode == kOpEndRep) {
            --depth;
        } else if (opcode == kOpRet && depth == 0 && !main_ended) {
            // The main function ends at its first top-level ret; subroutines follow.
            main_ended = true;
            insert_at = i;
        }

        if (opcode == kOpDcl) {
            if (length != 2) {
                return info;
            }
            // vs_3_0 output declarations: position 0 names the output register to redirect.
            const std::uint32_t usage = p[0] & 0x1F;
            const std::uint32_t usage_index = (p[0] >> 16) & 0xF;
            if (major == 3 && RegisterType(p[1]) == kRegOutput && usage == 0 && usage_index == 0) {
                if (position_number >= 0 || ((p[1] >> 16) & 0xF) != 0xF) {
                    info.result = kPartialPosition;
                    return info;
                }
                position_number = static_cast<int>(RegisterNumber(p[1]));
            }
            i += 1 + length;
            continue;
        }
        if (opcode == kOpDef || opcode == kOpDefI || opcode == kOpDefB) {
            if (length < 2) {
                return info;
            }
            if (opcode == kOpDef && RegisterType(p[0]) == kRegConst && RegisterNumber(p[0]) < 256) {
                const_used[RegisterNumber(p[0])] = true;
            }
            i += 1 + length;
            continue;
        }

        for (int k = 0; k < length; ++k) {
            const std::uint32_t param = p[k];
            if ((param & 0x80000000u) == 0) {
                return info;
            }
            const std::uint32_t type = RegisterType(param);
            const std::uint32_t number = RegisterNumber(param);
            if (type == kRegConst2 || type == kRegConst3 || type == kRegConst4) {
                info.result = kExtendedConstants;
                return info;
            }
            if (type == kRegTemp && number < 32) {
                temp_used[number] = true;
            } else if (type == kRegConst) {
                if (IsRelative(param)) {
                    relative_bases.push_back(static_cast<int>(number));
                } else if (number < 256) {
                    const_used[number] = true;
                }
            } else if (type == position_type) {
                if (major == 3 && IsRelative(param)) {
                    info.result = kRelativeOutput;
                    return info;
                }
                if (static_cast<int>(number) == position_number) {
                    position_params.push_back(i + 1 + k);
                    position_mask |= (param >> 16) & 0xF;
                }
            }
            // From vs_2_0 a relative operand is followed by its address register token.
            if (major >= 2 && IsRelative(param) && k + 1 < length) {
                ++k;
            }
        }
        i += 1 + length;
    }

    for (const TableRange& range : ranges) {
        if (range.count > 0) {
            info.max_constant = std::max(info.max_constant, range.first + range.count - 1);
        }
        info.constant_table = true;
    }
    for (int c = 255; c >= 0; --c) {
        if (const_used[c]) {
            info.max_constant = std::max(info.max_constant, c);
            break;
        }
    }

    if (position_params.empty()) {
        info.result = kNoPosition;
        return info;
    }
    if (position_mask != 0xF) {
        info.result = kPartialPosition;
        return info;
    }
    info.position_writes = static_cast<int>(position_params.size());

    const int j = info.jitter_register;
    if (const_used[j]) {
        info.result = kJitterRegisterUsed;
        return info;
    }
    for (const TableRange& range : ranges) {
        if (j >= range.first && j < range.first + range.count) {
            info.result = kJitterRegisterUsed;
            return info;
        }
    }
    // c[a0 + n] stays inside the table range that holds n (an array such as the bone palette).
    for (int base : relative_bases) {
        bool covered = false;
        for (const TableRange& range : ranges) {
            covered = covered || (base >= range.first && base < range.first + range.count);
        }
        if (!covered) {
            info.result = kUnknownRelative;
            return info;
        }
    }

    for (int r = 0; r < max_temps; ++r) {
        if (!temp_used[r]) {
            info.temp = r;
            break;
        }
    }
    if (info.temp < 0) {
        info.result = kNoFreeTemp;
        return info;
    }

    const std::uint32_t rt = static_cast<std::uint32_t>(info.temp);
    const std::uint32_t pos_number = static_cast<std::uint32_t>(position_number);
    std::vector<std::uint32_t> body(code, code + count);
    for (std::size_t index : position_params) {
        body[index] = (body[index] & ~(0x70001800u | 0x7FFu)) | Param(kRegTemp, rt, 0);
    }
    // vs_1_x has no length field (the bits are reserved, 0).
    const std::uint32_t mad = major >= 2 ? kOpMad | (4u << 24) : kOpMad;
    const std::uint32_t mov = major >= 2 ? kOpMov | (2u << 24) : kOpMov;
    out->reserve(count + 8);
    out->assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(insert_at));
    out->push_back(mad);
    out->push_back(Param(position_type, pos_number, 0x3));                      // oPos.xy
    out->push_back(Param(kRegTemp, rt, 0xFF));                                  // rT.wwww
    out->push_back(Param(kRegConst, static_cast<std::uint32_t>(j), 0x54));      // cJ.xyyy
    out->push_back(Param(kRegTemp, rt, 0x54));                                  // rT.xyyy
    out->push_back(mov);
    out->push_back(Param(position_type, pos_number, 0xC));                      // oPos.zw
    out->push_back(Param(kRegTemp, rt, 0xE4));                                  // rT.xyzw
    out->insert(out->end(), body.begin() + static_cast<std::ptrdiff_t>(insert_at), body.end());
    info.result = kPatched;
    return info;
}

} // namespace vsjitter

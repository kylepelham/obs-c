/**
 * @file graphics_offsets.h
 * @author Kyle Pelham (bonezone2001@gmail.com)
 * @brief The graphics offsets executable executor and parser.
 * 
 * @copyright Copyright (c) 2024
*/

#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <windows.h>
#include <array>
#include <cstdio>

#include <fmt/core.h>
#include "obsc/utils.h"
#include "obsc/toml11.h"
#include "obsc/constants.h"
#include "obsc/obsc_export.hpp"

namespace obsc {

#pragma pack(push, 8)

struct D3D8 {
	uint32_t present;
};

struct D3D9 {
	uint32_t present;
	uint32_t present_ex;
	uint32_t present_swap;
	uint32_t d3d9_clsoff;
	uint32_t is_d3d9ex_clsoff;
};

struct D3D12 {
	uint32_t execute_command_lists;
};

struct DXGI {
	uint32_t present;
	uint32_t resize;

	uint32_t present1;
};

struct DXGI2 {
	uint32_t release;
};

struct DDraw {
	uint32_t surface_create;
	uint32_t surface_restore;
	uint32_t surface_release;
	uint32_t surface_unlock;
	uint32_t surface_blt;
	uint32_t surface_flip;
	uint32_t surface_set_palette;
	uint32_t palette_set_entries;
};

struct ShmemData {
	volatile int last_tex;
	uint32_t tex1_offset;
	uint32_t tex2_offset;
};

struct ShtexData {
	uint32_t tex_handle;
};

enum class OBSC_EXPORT CaptureType : uint32_t {
    Memory,
    Texture
};

struct GraphicsOffsets {
	struct D3D8 d3d8;
	struct D3D9 d3d9;
	struct DXGI dxgi;
	struct DDraw ddraw;
	struct DXGI2 dxgi2;
	struct D3D12 d3d12;
};

#pragma pack(pop)

// Function to load graphic offsets
inline GraphicsOffsets loadGraphicsOffsets(uint32_t pid, bool is32Bit)
{
    // Switch binaries based on the bitness. Resolve next to the EXE (not the
    // cwd) so the bot can be launched from any working directory.
    std::string graphicsOffsetName = resolveBesideExe(is32Bit ? FILE_GRAPHICS_OFFSETS_32_NAME : FILE_GRAPHICS_OFFSETS_64_NAME);

    if (!std::filesystem::exists(graphicsOffsetName))
        throw std::runtime_error(fmt::format("The graphics offsets exe does not exist: {}", graphicsOffsetName));

    // Execute the binary. Quote the path -- it may contain spaces.
    std::string output = runProcessAndCaptureOutput("\"" + graphicsOffsetName + "\"");

    // Parse the output using a TOML parsing library
    auto parsedToml = toml::parse_str(output);
    
    GraphicsOffsets offsets{};

    offsets.d3d8.present = toml::find<uint32_t>(parsedToml, "d3d8", "present");
    offsets.d3d9.present = toml::find<uint32_t>(parsedToml, "d3d9", "present");
    offsets.d3d9.present_ex = toml::find<uint32_t>(parsedToml, "d3d9", "present_ex");
    offsets.d3d9.present_swap = toml::find<uint32_t>(parsedToml, "d3d9", "present_swap");
    offsets.d3d9.d3d9_clsoff = toml::find<uint32_t>(parsedToml, "d3d9", "d3d9_clsoff");
    offsets.d3d9.is_d3d9ex_clsoff = toml::find<uint32_t>(parsedToml, "d3d9", "is_d3d9ex_clsoff");

    offsets.dxgi.present = toml::find<uint32_t>(parsedToml, "dxgi", "present");
    offsets.dxgi.present1 = toml::find<uint32_t>(parsedToml, "dxgi", "present1");
    offsets.dxgi.resize = toml::find<uint32_t>(parsedToml, "dxgi", "resize");
    offsets.dxgi2.release = toml::find<uint32_t>(parsedToml, "dxgi", "release");

    return offsets;
}

} // namespace obsc
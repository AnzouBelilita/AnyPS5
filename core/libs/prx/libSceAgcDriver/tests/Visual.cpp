#define SDL_MAIN_HANDLED
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
void FreeTrackedMemory(std::uint16_t* memory) {
    if (memory != nullptr) VirtualFree(memory, 0, MEM_RELEASE);
}
#endif

using AgcDriver::Graphics::Require;
constexpr std::uint32_t Width = 640;
constexpr std::uint32_t Height = 480;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

ShaderRecompiler::RecompileResult LoadShader(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(file.is_open(), "cannot open test SPIR-V: " + path.string());
    const auto size = file.tellg();
    Require(size >= 20 && size <= 1024 * 1024 && size % 4 == 0, "invalid test SPIR-V size");
    ShaderRecompiler::RecompileResult result;
    result.spirv.resize(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    Require(static_cast<bool>(file.read(reinterpret_cast<char*>(result.spirv.data()), size)), "cannot read test SPIR-V");
    return result;
}

void RenderSoftwareTriangle() {
    constexpr float ax = 64.0f, ay = 408.0f;
    constexpr float bx = 576.0f, by = 408.0f;
    constexpr float cx = 320.0f, cy = 48.0f;
    const auto edge = [](float x0, float y0, float x1, float y1, float x, float y) {
        return (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
    };
    const float area = edge(ax, ay, bx, by, cx, cy);
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float wa = edge(bx, by, cx, cy, px, py) / area;
            const float wb = edge(cx, cy, ax, ay, px, py) / area;
            const float wc = edge(ax, ay, bx, by, px, py) / area;
            const auto offset = (static_cast<std::size_t>(y) * Width + x) * 4;
            if (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f) {
                Pixels[offset] = static_cast<std::byte>(static_cast<unsigned char>(255.0f * wa + 26.0f * wb + 38.0f * wc));
                Pixels[offset + 1] = static_cast<std::byte>(static_cast<unsigned char>(38.0f * wa + 255.0f * wb + 77.0f * wc));
                Pixels[offset + 2] = static_cast<std::byte>(static_cast<unsigned char>(26.0f * wa + 51.0f * wb + 255.0f * wc));
            } else {
                Pixels[offset] = std::byte{16};
                Pixels[offset + 1] = std::byte{24};
                Pixels[offset + 2] = std::byte{40};
            }
            Pixels[offset + 3] = std::byte{255};
        }
    }
}

void Run(SDL_Window* window, const std::filesystem::path& directory, bool verifyOnly, bool windowOnly) {
    unsigned count = 0;
    Require(SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr) == SDL_TRUE, SDL_GetError());
    std::vector<const char*> extensions(count);
    Require(SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data()) == SDL_TRUE, SDL_GetError());
    const AgcDriver::PresentationWindow presentation{window, extensions, [](void* context, VkInstance instance) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        Require(SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(context), instance, &surface) == SDL_TRUE, SDL_GetError());
        return surface;
    }, [](void* context, std::uint32_t* width, std::uint32_t* height) {
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(context), &drawableWidth, &drawableHeight);
        *width = drawableWidth > 0 ? static_cast<std::uint32_t>(drawableWidth) : 0;
        *height = drawableHeight > 0 ? static_cast<std::uint32_t>(drawableHeight) : 0;
    }, Width, Height};
    AgcDriver::VulkanDevice device(&presentation);
    auto vertex = LoadShader(directory / "Triangle.vert.spv");
    auto fragment = LoadShader(directory / "Triangle.frag.spv");
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderRecompiler::ShaderStage::Vertex, &vertex, 0},
        {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}
    }};
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        Pixels[i] = std::byte{16};
        Pixels[i + 1] = std::byte{24};
        Pixels[i + 2] = std::byte{40};
        Pixels[i + 3] = std::byte{255};
    }
    AgcDriver::Graphics::State state{};
    state.stages.path = AgcDriver::Graphics::ShaderPath::Vertex;
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size()};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    if (windowOnly) {
        RenderSoftwareTriangle();
    } else {
#ifdef _WIN32
        auto indices = std::unique_ptr<std::uint16_t[], decltype(&FreeTrackedMemory)>(
                static_cast<std::uint16_t*>(VirtualAlloc(nullptr, 3 * sizeof(std::uint16_t), MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE)),
                &FreeTrackedMemory);
        Require(indices != nullptr, "cannot allocate write-watched test indices");
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
#else
        const std::array<std::uint16_t, 3> indices{0, 1, 2};
#endif
        const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(
#ifdef _WIN32
                indices.get()
#else
                indices.data()
#endif
        ), 3, 2, 1, 0};
        device.Draw(state, draw, shaders);
        device.WaitIdle();
        const std::vector<std::byte> indexedPixels(Pixels.begin(), Pixels.end());
        for (std::size_t i = 0; i < Pixels.size(); i += 4) {
            Pixels[i] = std::byte{16};
            Pixels[i + 1] = std::byte{24};
            Pixels[i + 2] = std::byte{40};
            Pixels[i + 3] = std::byte{255};
        }
        const AgcDriver::Pm4::DrawParameters autoDraw{0, 3, 0, 1, 0, false};
        device.Draw(state, autoDraw, shaders);
        device.WaitIdle();
        Require(std::equal(Pixels.begin(), Pixels.end(), indexedPixels.begin()), "GPU readback: auto draw differs from indexed triangle");
        const auto center = (Height / 2 * Width + Width / 2) * 4;
        Require(std::to_integer<unsigned>(Pixels[center]) > 30 && std::to_integer<unsigned>(Pixels[center + 1]) > 30 && std::to_integer<unsigned>(Pixels[center + 2]) > 30, "GPU readback: triangle center was not rendered");
        Require(Pixels[0] == std::byte{16} && Pixels[1] == std::byte{24} && Pixels[2] == std::byte{40}, "GPU readback: background changed");
    }
    device.PresentPixels(Width, Height, Pixels);
    if (windowOnly) {
        std::cout << "Vulkan presentation active; software triangle displayed (AGC GPU draw verification skipped). Close the window or press Escape.\n" << std::flush;
    } else {
        std::cout << "SPIR-V triangle rendered, GPU readback verified, frame queued for presentation. Close the window or press Escape.\n" << std::flush;
    }
    if (!verifyOnly) {
        bool running = true;
        while (running) {
            SDL_Event event{};
            if (SDL_WaitEventTimeout(&event, 100)) {
                if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) device.PresentPixels(Width, Height, Pixels);
            }
        }
    }
    device.WaitIdle();
}

}

int main(int argc, char** argv) {
    try {
        const bool verifyOnly = argc == 2 && std::string_view(argv[1]) == "--verify";
        const bool windowOnly = argc == 2 && std::string_view(argv[1]) == "--window-only";
        Require(argc == 1 || verifyOnly || windowOnly, "usage: agc_driver_visual_test [--verify|--window-only]");
        SDL_SetMainReady();
        Require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        {
            const auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(SDL_CreateWindow("AnyPS5 AGC - SPIR-V triangle test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, Width, Height, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN), SDL_DestroyWindow);
            Require(window != nullptr, SDL_GetError());
            const auto base = std::unique_ptr<char, decltype(&SDL_free)>(SDL_GetBasePath(), SDL_free);
            Require(base != nullptr, SDL_GetError());
            Run(window.get(), std::filesystem::path(base.get()), verifyOnly, windowOnly);
        }
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}

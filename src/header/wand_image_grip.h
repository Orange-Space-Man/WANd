#pragma once
#include <wincodec.h>
#include <fstream>
#include <vector>
#include <limits>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace wand_image {
struct Grip { int x, y; };
inline bool locate(const std::vector<unsigned char>& rgba, int width, int height, Grip& grip) {
    if (width <= 0 || height <= 0 || rgba.size() != size_t(width) * height * 4) return false;
    int left = width, right = -1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (rgba[(size_t(y) * width + x) * 4 + 3] >= 128) {
                left = (std::min)(left, x); right = (std::max)(right, x);
            }
    if (right < left) return false;
    const int target = left + (right - left + 2) / 4;
    int best = (std::numeric_limits<int>::max)();
    for (int x = left; x <= right; ++x) {
        int sum = 0, count = 0;
        for (int y = 0; y < height; ++y)
            if (rgba[(size_t(y) * width + x) * 4 + 3] >= 128) { sum += y; ++count; }
        if (!count) continue;
        const int center = sum / count;
        for (int y = 0; y < height; ++y) {
            if (rgba[(size_t(y) * width + x) * 4 + 3] < 128) continue;
            const int score = abs(x - target) * height + abs(y - center);
            if (score < best) { best = score; grip = {x, y}; }
        }
    }
    return true;
}
inline std::vector<unsigned char> read(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file) {
        const auto size = file.tellg();
        if (size <= 0 || size > 16777216) return {};
        std::vector<unsigned char> data(static_cast<size_t>(size));
        file.seekg(0); file.read(reinterpret_cast<char*>(data.data()), data.size());
        return file ? data : std::vector<unsigned char>{};
    }
    std::ifstream archive("data/data.wak", std::ios::binary);
    auto number = [&]() { uint32_t value = 0; archive.read(reinterpret_cast<char*>(&value), 4); return value; };
    number(); const auto count = number(); number(); number();
    if (count > 1000000) return {};
    for (uint32_t i = 0; archive && i < count; ++i) {
        const auto offset = number(), size = number(), length = number();
        if (length > 4096) return {};
        std::string name(length, '\0'); archive.read(name.data(), length);
        if (name != path) continue;
        if (size > 16777216) return {};
        std::vector<unsigned char> data(size); archive.seekg(offset);
        archive.read(reinterpret_cast<char*>(data.data()), size);
        return archive ? data : std::vector<unsigned char>{};
    }
    return {};
}
inline bool decode(const std::string& path, int cropX, int cropY, int& width, int& height, Grip& grip) {
    auto bytes = read(path);
    if (bytes.empty()) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr; IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr; IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool success = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        UINT w = 0, h = 0; if (FAILED(frame->GetSize(&w, &h))) break;
        if (width == 0) width = w;
        if (height == 0) height = h;
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || cropX < 0 || cropY < 0 || uint64_t(cropX) + width > w || uint64_t(cropY) + height > h) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
        WICRect rect{cropX, cropY, width, height};
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        if (FAILED(converter->CopyPixels(&rect, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) break;
        success = locate(pixels, width, height, grip);
    } while (false);
    if (converter) converter->Release(); if (frame) frame->Release();
    if (decoder) decoder->Release(); if (stream) stream->Release(); if (factory) factory->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return success;
}
}

#include "outfits.h"
#include <windows.h>
#include "wand_image_grip.h"
#include <array>
#include <filesystem>
#include <string>
#include <cstring>
#include <fstream>
#include <atomic>
#include <regex>
#include "monitor.h"
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

namespace {
    constexpr outfits::Color colors[]{ {"Purple",155,111,154}, {"Red",190,70,65}, {"Blue",75,126,205}, {"Yellow",211,180,65}, {"Green",83,160,100}, {"White",190,191,201} };
    unsigned selection = 0, options = 0, unlockMask = 0;
    bool preferencesLoaded = false;
    std::wstring settingsPath = L"mods/WANd/character.ini";
    DWORD unlockChecked = 0;
    std::array<outfits::Portrait, 3> accessories;
    constexpr unsigned accessoryBits[]{outfits::crown, outfits::amulet, outfits::gem};
    constexpr const char* accessoryTags[]{"player_hat2", "player_amulet", "player_amulet_gem"};
    constexpr const char* unlockFlags[]{"secret_hat", "secret_amulet", "secret_amulet_gem"};
    void savePreferences() {
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(settingsPath).parent_path(), error);
        WritePrivateProfileStringW(L"Character", L"Appearance", std::to_wstring(selection | options).c_str(), std::filesystem::absolute(settingsPath).c_str());
    }
    struct Assets { std::string body, arm, cape; bool ready = false; outfits::Portrait head, bodyPreview; };
    std::array<Assets, outfits::count> assets;
    std::atomic<unsigned> loadState{0};
    struct Binding { int entity = 0, sprite = 0, arm = 0; unsigned outfit = ~0U; std::array<int, 3> cosmetics{}; std::string animation; };
    Binding bindings[2];
    DWORD lastAttempt[2]{};

    bool writeText(const std::string& path, const std::string& value) {
        std::ofstream file(path, std::ios::binary);
        file.write(value.data(), value.size());
        file.close();
        return bool(file);
    }

    bool makeSprite(IWICImagingFactory* factory, const char* source, const std::string& target, unsigned id) {
        const std::string base = std::string("data/enemies_gfx/") + source;
        auto png = wand_image::read(base + ".png");
        auto xmlBytes = wand_image::read(base + ".xml");
        if (png.empty() || xmlBytes.empty()) return false;
        IWICStream* input = nullptr; IWICStream* output = nullptr;
        IWICBitmapDecoder* decoder = nullptr; IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr; IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* encoded = nullptr;
        bool ok = false;
        do {
            if (FAILED(factory->CreateStream(&input)) || FAILED(input->InitializeFromMemory(png.data(), static_cast<DWORD>(png.size())))) break;
            if (FAILED(factory->CreateDecoderFromStream(input, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) || FAILED(decoder->GetFrame(0, &frame))) break;
            UINT width = 0, height = 0;
            if (FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 4096 || height > 4096) break;
            if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
            std::vector<unsigned char> pixels(size_t(width) * height * 4);
            if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) break;
            for (size_t i = 0; i < pixels.size(); i += 4) outfits::recolor(pixels.data() + i, id);
            if (std::strcmp(source, "player") == 0) {
                std::string xml(xmlBytes.begin(), xmlBytes.end());
                std::smatch stand;
                if (!std::regex_search(xml, stand, std::regex("<RectAnimation\\b[^>]*name=\"stand\"[^>]*>"))) break;
                const auto animation = stand.str();
                auto attr = [&](const char* name) {
                    std::smatch v;
                    return std::regex_search(animation, v, std::regex(std::string(name) + "=\"([0-9]+)\"")) ? unsigned(std::stoul(v[1])) : 0U;
                };
                const auto x = attr("pos_x"), y = attr("pos_y"), w = attr("frame_width"), h = attr("frame_height");
                if (!w || !h || x + w > width || y + h > height) break;
                auto& head = assets[id].head;
                auto& body = assets[id].bodyPreview;
                body = {w, h, std::vector<unsigned char>(size_t(w) * h * 4)};
                for (unsigned row = 0; row < h; ++row) std::memcpy(body.rgba.data() + size_t(row) * w * 4, pixels.data() + (size_t(y + row) * width + x) * 4, w * 4);
                head = {w, (std::min)(10U, h), {}};
                head.rgba.assign(body.rgba.begin(), body.rgba.begin() + size_t(w) * head.height * 4);
            }
            for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
            if (FAILED(factory->CreateStream(&output)) || FAILED(output->InitializeFromFilename(std::filesystem::path(target + ".png").c_str(), GENERIC_WRITE))) break;
            if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(output, WICBitmapEncoderNoCache))) break;
            if (FAILED(encoder->CreateNewFrame(&encoded, nullptr)) || FAILED(encoded->Initialize(nullptr)) || FAILED(encoded->SetSize(width, height))) break;
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (FAILED(encoded->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) break;
            if (FAILED(encoded->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data())) || FAILED(encoded->Commit()) || FAILED(encoder->Commit())) break;
            std::string xml(xmlBytes.begin(), xmlBytes.end());
            const auto start = xml.find("filename=\"");
            const auto end = start == std::string::npos ? start : xml.find('"', start + 10);
            if (start == std::string::npos || end == std::string::npos) break;
            xml.replace(start + 10, end - start - 10, target + ".png");
            ok = writeText(target + ".xml", xml);
        } while (false);
        if (encoded) encoded->Release(); if (encoder) encoder->Release(); if (output) output->Release();
        if (converter) converter->Release(); if (frame) frame->Release(); if (decoder) decoder->Release(); if (input) input->Release();
        return ok;
    }

    bool prepare(unsigned id, const std::string& directory = "mods/WANd/generated") {
        auto& a = assets[id];
        if (a.ready) return true;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        const auto prefix = directory + "/outfit_v2_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(id);
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        if (ok) ok = makeSprite(factory, "player", prefix + "_body", id) && makeSprite(factory, "player_arm", prefix + "_arm", id);
        if (factory) factory->Release();
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (!ok) return false;
        const auto& c = colors[id];
        const auto edge = 0xFF000000U | unsigned(c.r) << 16 | unsigned(c.g) << 8 | c.b;
        const auto fill = 0xFF000000U | unsigned(c.r * 127 / 155) << 16 | unsigned(c.g * 84 / 111) << 8 | unsigned(c.b * 118 / 154);
        char edgeHex[11]{}, fillHex[11]{};
        sprintf_s(edgeHex, "0x%08X", edge); sprintf_s(fillHex, "0x%08X", fill);
        const auto cape = std::string("<Entity name=\"cape\"><Base file=\"data/entities/verlet_chains/cape/cape.xml\"><VerletPhysicsComponent cloth_color=\"") + fillHex + "\" cloth_color_edge=\"" + edgeHex + "\" /></Base></Entity>";
        if (!writeText(prefix + "_cape.xml", cape)) return false;
        a.body = id ? prefix + "_body.xml" : "data/enemies_gfx/player.xml";
        a.arm = id ? prefix + "_arm.xml" : "data/enemies_gfx/player_arm.xml";
        a.cape = id ? prefix + "_cape.xml" : "data/entities/verlet_chains/cape/cape.xml";
        a.ready = true;
        return true;
    }


    bool loadAccessoryPreviews() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        for (unsigned i = 0; ok && i < 3; ++i) {
            auto bytes = wand_image::read(std::string("data/enemies_gfx/") + accessoryTags[i] + ".png");
            IWICStream* stream = nullptr; IWICBitmapDecoder* decoder = nullptr;
            IWICBitmapFrameDecode* frame = nullptr; IWICFormatConverter* converter = nullptr;
            ok = !bytes.empty() && SUCCEEDED(factory->CreateStream(&stream))
                && SUCCEEDED(stream->InitializeFromMemory(bytes.data(), DWORD(bytes.size())))
                && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            if (ok) {
                auto& p = accessories[i];
                p = {12, 19, std::vector<unsigned char>(12 * 19 * 4)};
                WICRect rect{0, 1, 12, 19};
                ok = SUCCEEDED(converter->CopyPixels(&rect, 48, UINT(p.rgba.size()), p.rgba.data()));
            }
            if (converter) converter->Release(); if (frame) frame->Release();
            if (decoder) decoder->Release(); if (stream) stream->Release();
        }
        if (factory) factory->Release();
        if (SUCCEEDED(initialized)) CoUninitialize();
        return ok;
    }

    bool setEnabled(lua51::lua_State* s, int entity, int comp, bool enabled) {
        if (!comp) return !enabled;
        const int top = lua51::getTop(s);
        lua51::getGlobal(s, "EntitySetComponentIsEnabled"); lua51::pushNumber(s, entity);
        lua51::pushNumber(s, comp); lua51::pushBoolean(s, enabled);
        const bool ok = lua51::pcall(s, 3, 0, 0) == 0; lua51::setTop(s, top); return ok;
    }

    int addAccessory(lua51::lua_State* s, int entity, unsigned index) {
        const int top = lua51::getTop(s);
        lua51::getGlobal(s, "EntityAddComponent2"); lua51::pushNumber(s, entity);
        lua51::pushString(s, "SpriteComponent"); lua51::createTable(s, 0, 7);
        const std::string tag = std::string("character,") + accessoryTags[index];
        const std::string path = std::string("data/enemies_gfx/") + accessoryTags[index] + ".xml";
        lua51::pushString(s, tag.c_str()); lua51::setField(s, -2, "_tags");
        lua51::pushString(s, path.c_str()); lua51::setField(s, -2, "image_file");
        lua51::pushString(s, "stand"); lua51::setField(s, -2, "rect_animation");
        lua51::pushNumber(s, 6); lua51::setField(s, -2, "offset_x");
        lua51::pushNumber(s, 14); lua51::setField(s, -2, "offset_y");
        lua51::pushNumber(s, index == 1 ? .59 : .58); lua51::setField(s, -2, "z_index");
        const bool ok = lua51::pcall(s, 3, 1, 0) == 0;
        const int result = ok && lua51::type(s, -1) == lua51::typeNumber ? int(lua51::toNumber(s, -1)) : 0;
        lua51::setTop(s, top); return result;
    }

    DWORD WINAPI loadAssets(LPVOID) {
        bool ok = false;
        try { ok = loadAccessoryPreviews(); for (unsigned id = 0; ok && id < outfits::count; ++id) ok = prepare(id); } catch (...) { ok = false; }
        loadState.store(ok ? 2 : 3, std::memory_order_release);
        if (!ok) monitor::write("log", "Outfit assets failed to load; generation will not retry during gameplay");
        return 0;
    }

    int component(lua51::lua_State* s, int entity, const char* tag) {
        const int top = lua51::getTop(s);
        lua51::getGlobal(s, "EntityGetFirstComponentIncludingDisabled"); lua51::pushNumber(s, entity); lua51::pushString(s, "SpriteComponent");
        int args = 2; if (tag) { lua51::pushString(s, tag); ++args; }
        const bool ok = lua51::pcall(s, args, 1, 0) == 0;
        const int value = ok && lua51::type(s, -1) == lua51::typeNumber ? int(lua51::toNumber(s, -1)) : 0;
        lua51::setTop(s, top); return value;
    }

    bool setSprite(lua51::lua_State* s, int entity, int componentId, const std::string& path) {
        const int top = lua51::getTop(s);
        lua51::getGlobal(s, "ComponentSetValue2"); lua51::pushNumber(s, componentId); lua51::pushString(s, "image_file"); lua51::pushString(s, path.c_str());
        bool ok = lua51::pcall(s, 3, 0, 0) == 0;
        lua51::setTop(s, top);
        if (ok) {
            lua51::getGlobal(s, "EntityRefreshSprite"); lua51::pushNumber(s, entity); lua51::pushNumber(s, componentId);
            ok = lua51::pcall(s, 2, 0, 0) == 0;
            lua51::setTop(s, top);
        }
        return ok;
    }
}

const outfits::Color& outfits::color(unsigned id) { return colors[id < count ? id : 0]; }
unsigned outfits::selected() { return selection; }
void outfits::select(unsigned id) { if (id < count && selection != id) { selection = id; savePreferences(); } }

unsigned outfits::appearance() { return selection | (options & (hideCape | unlockMask)); }
bool outfits::unlocked(unsigned accessory) { return (unlockMask & accessory) == accessory; }
bool outfits::enabled(unsigned option) { return (appearance() & option) != 0; }
void outfits::toggle(unsigned option) {
    if (option != hideCape && option != crown && option != amulet && option != gem) return;
    if (option != hideCape && !unlocked(option)) return;
    options ^= option; savePreferences();
}
void outfits::refreshUnlocks(lua51::lua_State* s) {
    if (!s) {
        if (!preferencesLoaded) {
            const unsigned saved = GetPrivateProfileIntW(L"Character", L"Appearance", 0, std::filesystem::absolute(settingsPath).c_str());
            selection = (saved & 255) < count ? saved & 255 : 0;
            options = saved & (hideCape | crown | amulet | gem);
            preferencesLoaded = true;
        }
        PWSTR folder = nullptr;
        std::filesystem::path root;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &folder))) {
            root = std::filesystem::path(folder) / L"Nolla_Games_Noita";
            CoTaskMemFree(folder);
        }
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 1 < argc; ++i)
            if (std::wcscmp(argv[i], L"-save_path") == 0) root = argv[++i];
        if (argv) LocalFree(argv);
        unlockMask = 0;
        if (!root.empty()) {
            std::error_code error;
            for (unsigned i = 0; i < 3; ++i)
                if (std::filesystem::is_regular_file(root / "save00/persistent/flags" / unlockFlags[i], error)) unlockMask |= accessoryBits[i];
        }
        return;
    }
    const DWORD now = GetTickCount();
    if (unlockChecked && now - unlockChecked < 1000) return;
    unlockChecked = now;
    const int top = lua51::getTop(s);
    unsigned mask = 0;
    for (unsigned i = 0; i < 3; ++i) {
        lua51::getGlobal(s, "HasFlagPersistent"); lua51::pushString(s, unlockFlags[i]);
        const bool ok = lua51::pcall(s, 1, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeBoolean;
        if (!ok) { lua51::setTop(s, top); return; }
        if (lua51::toBoolean(s, -1)) mask |= accessoryBits[i];
        lua51::setTop(s, top);
    }
    unlockMask = mask;
}
outfits::Portrait outfits::preview(unsigned value) {
    const unsigned id = (value & 255) < count ? value & 255 : 0;
    Portrait result{24, 27, std::vector<unsigned char>(24 * 27 * 4)};
    if (!ready()) return result;
    auto pixel = [&](unsigned x, unsigned y, const unsigned char* p) {
        if (x >= result.width || y >= result.height || !p[3]) return;
        auto* dst = result.rgba.data() + (y * result.width + x) * 4;
        for (int c = 0; c < 3; ++c) dst[c] = (p[c] * p[3] + dst[c] * (255 - p[3])) / 255;
        dst[3] = static_cast<unsigned char>(p[3] + dst[3] * (255 - p[3]) / 255);
    };
    if (!(value & hideCape)) {
        const auto& c = colors[id]; unsigned char fill[]{static_cast<unsigned char>(c.r * 127 / 155), static_cast<unsigned char>(c.g * 84 / 111), static_cast<unsigned char>(c.b * 118 / 154), 255};
        for (unsigned y = 9; y < 23; ++y) for (unsigned x = 10; x < 15 + (y - 9) / 3; ++x) pixel(x, y, fill);
    }
    auto layer = [&](const Portrait& p) {
        for (unsigned y = 0; y < p.height; ++y) for (unsigned x = 0; x < p.width; ++x)
            pixel(x + 6, y + 4, p.rgba.data() + (y * p.width + x) * 4);
    };
    layer(assets[id].bodyPreview);
    for (unsigned i = 0; i < 3; ++i) if (value & accessoryBits[i]) layer(accessories[i]);
    return result;
}
void outfits::animate(lua51::lua_State* s, int entity, const char* animation) {
    auto& binding = bindings[1];
    if (binding.entity != entity || !animation || binding.animation == animation) return;
    const int top = lua51::getTop(s);
    for (int comp : binding.cosmetics) if (comp) {
        lua51::getGlobal(s, "ComponentSetValue2"); lua51::pushNumber(s, comp);
        lua51::pushString(s, "rect_animation"); lua51::pushString(s, animation);
        lua51::pcall(s, 3, 0, 0); lua51::setTop(s, top);
    }
    binding.animation = animation;
}

bool outfits::ready() { return loadState.load(std::memory_order_acquire) == 2; }
bool outfits::failed() { return loadState.load(std::memory_order_acquire) == 3; }
const outfits::Portrait* outfits::portrait(unsigned id) { return ready() && id < count ? &assets[id].head : nullptr; }
void outfits::forget(bool remote) { bindings[remote ? 1 : 0] = {}; lastAttempt[remote ? 1 : 0] = 0; }
void outfits::initialize() {
    unsigned expected = 0;
    if (!loadState.compare_exchange_strong(expected, 1)) return;
    const HANDLE thread = CreateThread(nullptr, 0, loadAssets, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread); else loadState.store(3, std::memory_order_release);
}

void outfits::recolor(unsigned char* p, unsigned id) {
    if (!id || id >= count || !p[3] || p[0] * 10 <= p[1] * 11 || p[2] * 10 <= p[1] * 11 || std::abs(int(p[0]) - p[2]) > 40) return;
    const float shade = (std::max)(p[0], p[2]) / 155.0f;
    const auto& c = colors[id];
    p[0] = static_cast<unsigned char>((std::min)(255.0f, c.r * shade));
    p[1] = static_cast<unsigned char>((std::min)(255.0f, c.g * shade));
    p[2] = static_cast<unsigned char>((std::min)(255.0f, c.b * shade));
}

void outfits::apply(lua51::lua_State* s, int entity, unsigned id, bool remote) {
    if (!s || !entity) return;
    unsigned value = id & (255 | hideCape | crown | amulet | gem);
    id = value & 255;
    if (id >= count) { id = 0; value &= ~255U; }
    if (!remote) value &= 255 | hideCape | unlockMask;
    auto& binding = bindings[remote ? 1 : 0];
    if (binding.entity == entity && binding.outfit == value) return;
    initialize();
    if (!ready()) return;
    auto& attempted = lastAttempt[remote ? 1 : 0];
    const DWORD now = GetTickCount();
    if (attempted && now - attempted < 1000) return;
    attempted = now;
    const int top = lua51::getTop(s);
    const int sprite = component(s, entity, "character");
    if (!sprite) return;
    int arm = 0, cape = 0;
    lua51::getGlobal(s, "EntityGetAllChildren"); lua51::pushNumber(s, entity);
    if (lua51::pcall(s, 1, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeTable) {
        for (int i = 1; i <= 64; ++i) {
            lua51::rawGetIndex(s, -1, i);
            if (lua51::type(s, -1) != lua51::typeNumber) { lua51::setTop(s, top + 1); break; }
            const int child = int(lua51::toNumber(s, -1)); lua51::setTop(s, top + 1);
            lua51::getGlobal(s, "EntityGetName"); lua51::pushNumber(s, child);
            if (lua51::pcall(s, 1, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeString) {
                const char* name = lua51::toString(s, -1);
                if (std::strcmp(name, "arm_r") == 0) arm = child;
                if (std::strcmp(name, "cape") == 0) cape = child;
            }
            lua51::setTop(s, top + 1);
        }
    }
    lua51::setTop(s, top);
    const int armSprite = arm ? component(s, arm, nullptr) : 0;
    if (!armSprite) return;
    const bool fresh = binding.entity != entity;
    const bool colorChanged = fresh ? id != 0 : (binding.outfit & 255) != id;
    const auto& a = assets[id];
    if (colorChanged && (!setSprite(s, entity, sprite, a.body) || !setSprite(s, arm, armSprite, a.arm))) return;
    std::array<int, 3> cosmetics{};
    for (unsigned i = 0; i < 3; ++i) {
        const bool show = (value & accessoryBits[i]) != 0;
        cosmetics[i] = component(s, entity, accessoryTags[i]);
        if (!cosmetics[i] && show) cosmetics[i] = addAccessory(s, entity, i);
        if (!setEnabled(s, entity, cosmetics[i], show)) return;
    }
    if (value & hideCape) {
        if (cape) {
            lua51::getGlobal(s, "EntityKill"); lua51::pushNumber(s, cape);
            const bool ok = lua51::pcall(s, 1, 0, 0) == 0; lua51::setTop(s, top);
            if (!ok) return;
        }
    } else if (!cape || colorChanged) {
        float x = 0, y = 0;
        lua51::getGlobal(s, "EntityGetTransform"); lua51::pushNumber(s, cape ? cape : entity);
        if (lua51::pcall(s, 1, 2, 0) == 0) { x = float(lua51::toNumber(s, -2)); y = float(lua51::toNumber(s, -1)); }
        lua51::setTop(s, top);
        lua51::getGlobal(s, "EntityLoad"); lua51::pushString(s, a.cape.c_str()); lua51::pushNumber(s, x); lua51::pushNumber(s, y);
        int replacement = 0;
        if (lua51::pcall(s, 3, 1, 0) == 0 && lua51::type(s, -1) == lua51::typeNumber) replacement = int(lua51::toNumber(s, -1));
        lua51::setTop(s, top);
        if (!replacement) return;
        lua51::getGlobal(s, "EntitySetName"); lua51::pushNumber(s, replacement); lua51::pushString(s, "cape"); lua51::pcall(s, 2, 0, 0); lua51::setTop(s, top);
        lua51::getGlobal(s, "EntityAddChild"); lua51::pushNumber(s, entity); lua51::pushNumber(s, replacement);
        const bool attached = lua51::pcall(s, 2, 0, 0) == 0; lua51::setTop(s, top);
        if (!attached || cape) { lua51::getGlobal(s, "EntityKill"); lua51::pushNumber(s, attached ? cape : replacement); lua51::pcall(s, 1, 0, 0); lua51::setTop(s, top); }
        if (!attached) return;
    }
    binding = {entity, sprite, arm, value, cosmetics, {}};
}

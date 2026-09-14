#include "multiplayer_menu.h"
#include "lan_discovery.h"
#include "game_start.h"
#include "memory.h"
#include "monitor.h"
#include "network.h"
#include "noita.h"
#include <windows.h>
#include <gl/GL.h>
#include "wand_image_grip.h"
#include "outfits.h"
#include <intrin.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#pragma comment(lib, "opengl32.lib")

namespace {
    struct NativeString { char buffer[16]; unsigned size; unsigned capacity; };
    struct NativeButtonResult { unsigned char bytes[0x4C]{}; };
    static_assert(sizeof(NativeButtonResult) == 76);
    using Button = void*(__thiscall*)(void*, void*, unsigned, unsigned, const NativeString*, unsigned, unsigned, float, float, void*, void*, float, float);
    using PollEvent = int(__cdecl*)(void*);
    Button originalButton = nullptr;
    PollEvent originalPoll = nullptr;
    bool visible = false, mouseHeld = false, enterHeld = false, escapeHeld = false;
    bool upHeld = false, downHeld = false, click = false, enter = false;
    bool leftHeld = false, rightHeld = false, tabHeld = false, keyboardFocus = false;
    std::array<GLuint, outfits::count> portraits{};
    bool editingCharacter = false;
    GLuint characterTexture = 0;
    unsigned characterTextureAppearance = ~0U;
    int focus = 0, buttonCount = 0, previousCount = 0;
    float mouseX = -1, mouseY = -1;
    DWORD menuFrame = 0;
    std::string message;
    size_t page = 0;
    struct Glyph { int offsetX, offsetY, height, width, x, y, advance; };
    std::array<Glyph, 256> glyphs{};
    GLuint font = 0;
    UINT fontWidth = 0, fontHeight = 0;
    bool fontAttempted = false;

    bool frontMenu() {
        return noita::game() && *reinterpret_cast<unsigned char*>(reinterpret_cast<unsigned char*>(noita::game()) + 0xE0761B) != 0;
    }

    bool edge(int key, bool& held) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool pressed = down && !held;
        held = down;
        return pressed;
    }

    int __cdecl pollEvent(void* event) {
        for (;;) {
            const int result = originalPoll(event);
            if (!result || !event || !visible || !frontMenu()) return result;
            const unsigned type = *static_cast<unsigned*>(event);
            if (type < 0x300 || type > 0x6FF) return result;
        }
    }

    void* __fastcall buttonHook(void* gui, void*, void* result, unsigned id, unsigned a3, const NativeString* label,
        unsigned flags, unsigned a6, float a7, float a8, void* a9, void* a10, float a11, float a12) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - reinterpret_cast<std::uintptr_t>(noita::game());
        const bool main = frontMenu() && caller >= 0x2E3000 && caller < 0x2E5000;
        if (main) menuFrame = GetTickCount();
        if (main && visible) { std::memset(result, 0, sizeof(NativeButtonResult)); return result; }
        if (main && caller == 0x2E3F36 && id == 0x3669) {
            const NativeString multiplayer{{'M','u','l','t','i','p','l','a','y','e','r',0}, 11, 15};
            NativeButtonResult extra{};
            originalButton(gui, &extra, 0x57414E44, a3, &multiplayer, flags, a6, a7, a8, a9, a10, a11, a12);
            if (extra.bytes[0]) {
                outfits::initialize();
                outfits::refreshUnlocks();
                editingCharacter = false;
                visible = true;
                mouseHeld = enterHeld = true;
                focus = 0;
                message.clear();
                if (!lan_discovery::open(network::status() == network::Status::hosting)) message = "LAN discovery unavailable (UDP 27889).";
                std::memset(result, 0, sizeof(NativeButtonResult));
                return result;
            }
        }
        return originalButton(gui, result, id, a3, label, flags, a6, a7, a8, a9, a10, a11, a12);
    }

    bool loadFont() {
        auto xmlBytes = wand_image::read("data/fonts/font_pixel.xml");
        auto bytes = wand_image::read("data/fonts/font_pixel.png");
        if (xmlBytes.empty() || bytes.empty()) return false;
        std::istringstream xml(std::string(xmlBytes.begin(), xmlBytes.end()));
        std::string line;
        while (std::getline(xml, line)) {
            unsigned id = 0;
            Glyph glyph{};
            if (sscanf_s(line.c_str(), " <QuadChar id=\"%u\" offset_x=\"%d\" offset_y=\"%d\" rect_h=\"%d\" rect_w=\"%d\" rect_x=\"%d\" rect_y=\"%d\" width=\"%d\"",
                &id, &glyph.offsetX, &glyph.offsetY, &glyph.height, &glyph.width, &glyph.x, &glyph.y, &glyph.advance) == 8 && id < glyphs.size()) glyphs[id] = glyph;
        }
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        std::vector<unsigned char> pixels;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) break;
            if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())))) break;
            if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) || FAILED(decoder->GetFrame(0, &frame))) break;
            if (FAILED(frame->GetSize(&fontWidth, &fontHeight)) || fontWidth > 4096 || fontHeight > 4096) break;
            if (FAILED(factory->CreateFormatConverter(&converter))) break;
            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
            pixels.resize(size_t(fontWidth) * fontHeight * 4);
            if (FAILED(converter->CopyPixels(nullptr, fontWidth * 4, static_cast<UINT>(pixels.size()), pixels.data()))) pixels.clear();
        } while (false);
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (pixels.empty() || glyphs['A'].advance == 0) return false;
        glGenTextures(1, &font);
        glBindTexture(GL_TEXTURE_2D, font);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fontWidth, fontHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return font != 0;
    }

    void rectangle(float x, float y, float w, float h, float r, float g, float b, float a = 1) {
        glDisable(GL_TEXTURE_2D);
        glColor4f(r, g, b, a);
        glBegin(GL_QUADS);
        glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
        glEnd();
    }

    void panel(float x, float y, float w, float h) {
        rectangle(x, y, w, h, .56f, .51f, .38f);
        rectangle(x + 2, y + 2, w - 4, h - 4, .045f, .043f, .045f, .97f);
    }

    void text(float x, float y, const std::string& value, bool highlighted = false, float opacity = 1) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, font);
        glColor4f(1, highlighted ? .85f : 1, highlighted ? .4f : 1, opacity);
        glBegin(GL_QUADS);
        for (unsigned char c : value) {
            const auto& g = glyphs[c];
            const float x0 = x + g.offsetX, y0 = y + g.offsetY;
            const float u0 = float(g.x) / fontWidth, v0 = float(g.y) / fontHeight;
            const float u1 = float(g.x + g.width) / fontWidth, v1 = float(g.y + g.height) / fontHeight;
            glTexCoord2f(u0, v0); glVertex2f(x0, y0);
            glTexCoord2f(u1, v0); glVertex2f(x0 + g.width, y0);
            glTexCoord2f(u1, v1); glVertex2f(x0 + g.width, y0 + g.height);
            glTexCoord2f(u0, v1); glVertex2f(x0, y0 + g.height);
            x += g.advance;
        }
        glEnd();
    }

    bool button(float x, float y, float w, const std::string& label, bool enabled = true) {
        const int index = buttonCount++;
        const bool hovered = mouseX >= x && mouseX < x + w && mouseY >= y && mouseY < y + 23;
        if (enabled && (hovered || (keyboardFocus && focus == index))) rectangle(x, y, w, 23, .19f, .18f, .22f);
        text(x + 5, y + 6, label, enabled && (hovered || (keyboardFocus && focus == index)), enabled ? 1.f : .35f);
        if (enabled && ((click && hovered) || (enter && focus == index))) { click = enter = false; focus = index; return true; }
        return false;
    }

    void leave() {
        editingCharacter = false;
        network::stop();
        lan_discovery::close();
        visible = false;
    }

    float textWidth(const std::string& value) {
        float width = 0;
        for (unsigned char c : value) width += glyphs[c].advance;
        return width;
    }

    std::string fitText(std::string value, float width) {
        if (textWidth(value) <= width) return value;
        while (!value.empty() && textWidth(value + "...") > width) value.pop_back();
        return value + "...";
    }

    void portrait(float x, float y, unsigned id, float scale) {
        const auto* head = outfits::portrait(id);
        if (!head) return;
        if (!portraits[id]) {
            glGenTextures(1, &portraits[id]);
            glBindTexture(GL_TEXTURE_2D, portraits[id]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, head->width, head->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, head->rgba.data());
        }
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, portraits[id]);
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(1, 0); glVertex2f(x + head->width * scale, y);
        glTexCoord2f(1, 1); glVertex2f(x + head->width * scale, y + head->height * scale);
        glTexCoord2f(0, 1); glVertex2f(x, y + head->height * scale);
        glEnd();
    }

    void outfitGrid(float x, float y) {
        text(x, y, "Outfit color");
        text(x, y + 17, "Body, sleeves and cape", false, .5f);
        for (unsigned id = 0; id < outfits::count; ++id) {
            const float cx = x + (id % 2) * 95, cy = y + 39 + (id / 2) * 67;
            const int index = buttonCount++;
            const bool hover = mouseX >= cx && mouseX < cx + 88 && mouseY >= cy && mouseY < cy + 60;
            const bool selected = outfits::selected() == id;
            const bool focused = hover || (keyboardFocus && focus == index);
            const auto& c = outfits::color(id);
            rectangle(cx, cy, 88, 60, selected ? c.r / 255.f : .24f, selected ? c.g / 255.f : .22f, selected ? c.b / 255.f : .28f);
            rectangle(cx + 1, cy + 1, 86, 58, focused ? .18f : .09f, focused ? .16f : .075f, focused ? .21f : .105f);
            if (selected) rectangle(cx + 1, cy + 57, 86, 2, c.r / 255.f, c.g / 255.f, c.b / 255.f);
            portrait(cx + 26, cy + 5, id, 3);
            text(cx + (88 - textWidth(c.name)) / 2, cy + 42, c.name, selected || focused);
            if (selected) text(cx + 76, cy + 3, "*", true);
            if (outfits::ready() && ((click && hover) || (enter && focus == index))) {
                outfits::select(id); focus = index; click = enter = false;
            }
        }
        if (!outfits::ready()) text(x, y + 244, outfits::failed() ? "Outfits could not load" : "Loading outfits...", outfits::failed(), .7f);
    }


    void characterPreview(float x, float y, float scale) {
        if (!outfits::ready()) return;
        const unsigned value = outfits::appearance();
        if (!characterTexture) glGenTextures(1, &characterTexture);
        glBindTexture(GL_TEXTURE_2D, characterTexture);
        if (characterTextureAppearance != value) {
            const auto pixels = outfits::preview(value);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixels.width, pixels.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.rgba.data());
            characterTextureAppearance = value;
        }
        glEnable(GL_TEXTURE_2D); glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x, y);
        glTexCoord2f(1, 0); glVertex2f(x + 24 * scale, y);
        glTexCoord2f(1, 1); glVertex2f(x + 24 * scale, y + 27 * scale);
        glTexCoord2f(0, 1); glVertex2f(x, y + 27 * scale);
        glEnd();
    }

    void characterEditor(float width, float height) {
        const float area = (std::min)(600.f, width - 32), left = (width - area) / 2;
        const float top = 44, bottom = height - 43;
        const float previewWidth = 140, colorsX = left + 151, controlsX = left + 357;
        text(left, 19, "Character editor");
        panel(left, top, previewWidth, bottom - top);
        panel(colorsX, top, 196, bottom - top);
        panel(controlsX, top, area - 357, bottom - top);
        text(left + 12, top + 13, "Your character");
        characterPreview(left + 10, top + 48, 5);
        text(left + 12, top + 201, outfits::color(outfits::selected()).name, true);
        text(left + 12, top + 222, "Shared with players", false, .5f);
        outfitGrid(colorsX + 6, top + 12);
        const float x = controlsX + 10, w = area - 377;
        text(x, top + 13, "Accessories");
        text(x, top + 31, "Your earned cosmetics", false, .5f);
        const unsigned flags[]{outfits::hideCape, outfits::crown, outfits::amulet, outfits::gem};
        const char* labels[]{"Cape", "Crown", "Amulet", "Amulet gem"};
        for (unsigned i = 0; i < 4; ++i) {
            const bool allowed = i == 0 || outfits::unlocked(flags[i]);
            const bool on = i == 0 ? !outfits::enabled(flags[i]) : outfits::enabled(flags[i]);
            const float y = top + 54 + i * 45;
            rectangle(x, y + 40, w, 1, .22f, .20f, .26f);
            const std::string label = std::string(on ? "[x] " : "[ ] ") + labels[i];
            if (button(x, y, w, label, allowed && outfits::ready())) outfits::toggle(flags[i]);
            text(x + 5, y + 24, allowed ? (i == 0 ? "Show or remove cape" : "Unlocked") : "Locked - earn in Noita", false, allowed ? .45f : .3f);
        }
        text(x, bottom - 25, "Changes saved automatically", false, .45f);
        if (button(left, bottom + 7, 170, "< Done editing")) {
            editingCharacter = false; focus = 0; keyboardFocus = false;
        }
    }

    void lobby(float width, float height) {
        if (editingCharacter) { characterEditor(width, height); return; }
        const float area = (std::min)(600.f, width - 32), left = (width - area) / 2;
        const float top = 44, split = left + 214, right = left + area, bottom = height - 43;
        text(left + 1, 19, "Multiplayer");
        text(right - textWidth("LOCAL NETWORK"), 21, "LOCAL NETWORK", false, .5f);
        panel(left, top, 204, bottom - top);
        panel(split, top, right - split, bottom - top);
        text(left + 12, top + 13, "Your character");
        characterPreview(left + 42, top + 34, 5);
        text(left + 12, top + 183, outfits::color(outfits::selected()).name, true);
        text(left + 12, top + 203, outfits::enabled(outfits::hideCape) ? "Cape hidden" : "Cape visible", false, .5f);
        if (button(left + 10, bottom - 35, 184, "Edit character")) {
            editingCharacter = true; focus = 0; keyboardFocus = false; outfits::refreshUnlocks();
        }
        const auto state = network::status();
        const float x = split + 12, w = right - x - 12;
        if (state == network::Status::stopped) {
            text(x, top + 13, "LAN games");
            if (button(right - 73, top + 7, 62, "Refresh")) {
                page = 0;
                message = lan_discovery::open(false) ? "" : "LAN discovery could not start.";
            }
            rectangle(x, top + 35, w, 1, .24f, .22f, .28f);
            const auto& entries = lan_discovery::sessions();
            if (entries.empty()) {
                text(x + 4, top + 64, "No games found", false, .85f);
                text(x + 4, top + 86, fitText("Host a game or wait for a friend.", w - 8), false, .5f);
            }
            const size_t perPage = 3;
            if (page * perPage >= entries.size()) page = 0;
            for (size_t i = page * perPage; i < entries.size() && i < (page + 1) * perPage; ++i) {
                const auto& session = entries[i];
                const float y = top + 45 + float(i % perPage) * 49;
                if (button(x, y, w, fitText("Join " + session.name, w - 10))) {
                    message = network::join(session.address.c_str(), session.port) ? "" : "Could not join this session.";
                    break;
                }
                text(x + 5, y + 24, session.address + "  /  1 of 2 players", false, .5f);
            }
            if (entries.size() > perPage && button(x, bottom - 66, w, "More games >")) page = (page + 1) % ((entries.size() + perPage - 1) / perPage);
            rectangle(x, bottom - 39, w, 1, .24f, .22f, .28f);
            if (button(x, bottom - 30, w, "Host a new game")) {
                if (!network::host(lan_discovery::gamePort)) message = "Could not host. Another game may be using this port.";
                else if (!lan_discovery::open(true)) message = "Hosting, but LAN discovery is unavailable.";
                else message.clear();
            }
        } else if (state == network::Status::joining) {
            text(x, top + 13, "Joining game");
            text(x, top + 64, "Connecting to host...", false, .7f);
            if (button(x, bottom - 30, w, "Cancel")) {
                network::stop(); lan_discovery::open(false); message.clear();
            }
        } else {
            text(x, top + 13, network::isHost() ? "Your lobby" : "Game lobby");
            text(x, top + 33, "Normal game", false, .5f);
            portrait(x + 3, top + 64, outfits::selected(), 2);
            text(x + 37, top + 65, network::isHost() ? "You  /  Host" : "You  /  Guest");
            text(x + 37, top + 82, outfits::color(outfits::selected()).name, false, .6f);
            rectangle(x, top + 111, w, 1, .24f, .22f, .28f);
            const bool connected = state == network::Status::connected;
            text(x + 4, top + 128, connected ? (network::isHost() ? "Guest connected" : "Host connected") : "Waiting for a player...", false, connected ? 1.f : .55f);
            text(x + 4, top + 153, connected ? "2 of 2 players" : "1 of 2 players", false, .5f);
            if (network::isHost()) {
                rectangle(x, bottom - 39, w, 1, .24f, .22f, .28f);
                if (button(x, bottom - 30, w, "Start new game", connected && outfits::ready())) message = game_start::requestStart() ? "Starting game..." : "Could not start the game.";
            } else text(x + 4, bottom - 27, fitText("Waiting for the host to start...", w - 8), false, .6f);
        }
        if (button(left, bottom + 7, 100, state == network::Status::stopped ? "< Back" : "< Leave lobby")) leave();
        if (!message.empty()) text(left + 108, bottom + 13, fitText(message, area - 108), true);
    }
}

bool multiplayer_menu::init() {
    auto* base = reinterpret_cast<unsigned char*>(noita::game());
    const unsigned char expected[]{0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68};
    if (!base || std::memcmp(base + 0x4245D0, expected, sizeof(expected)) != 0) return false;
    if (!memory::hook_iat(noita::game(), "SDL2.dll", "SDL_PollEvent", reinterpret_cast<void*>(&pollEvent), reinterpret_cast<void**>(&originalPoll))) return false;
    if (!memory::hook(base + 0x4245D0, reinterpret_cast<void*>(&buttonHook), reinterpret_cast<void**>(&originalButton))) return false;
    monitor::write("log", "Native Multiplayer menu installed");
    return true;
}

void multiplayer_menu::closeForRun() {
    visible = false;
    lan_discovery::close();
}

void multiplayer_menu::draw() {
    if (!visible) return;
    if (!frontMenu()) { closeForRun(); return; }
    if (GetTickCount() - menuFrame > 500) return;
    lan_discovery::update();
    HWND window = WindowFromDC(wglGetCurrentDC());
    if (!window) return;
    const bool active = GetForegroundWindow() == window;
    if (active && edge(VK_ESCAPE, escapeHeld)) {
        if (editingCharacter) { editingCharacter = false; focus = 0; } else leave();
        return;
    }
    click = edge(VK_LBUTTON, mouseHeld) && active;
    enter = edge(VK_RETURN, enterHeld) && active;
    if (click) keyboardFocus = false;
    const int total = (std::max)(1, previousCount);
    if (edge(VK_UP, upHeld) && active) { focus = (focus + total - (editingCharacter && focus < 6 ? 2 : 1)) % total; keyboardFocus = true; }
    if (edge(VK_DOWN, downHeld) && active) { focus = (focus + (focus < 6 ? 2 : 1)) % total; keyboardFocus = true; }
    if (edge(VK_LEFT, leftHeld) && active) { focus = (focus + total - 1) % total; keyboardFocus = true; }
    if ((edge(VK_RIGHT, rightHeld) || edge(VK_TAB, tabHeld)) && active) { focus = (focus + 1) % total; keyboardFocus = true; }
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    RECT client{};
    GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0 || viewport[2] <= 0 || viewport[3] <= 0) return;
    const float aspect = float(viewport[2]) / viewport[3];
    const float width = (std::max)(640.f, 360.f * aspect), height = width / aspect;
    POINT cursor{};
    GetCursorPos(&cursor); ScreenToClient(window, &cursor);
    mouseX = active ? (cursor.x * float(viewport[2]) / client.right - viewport[0]) * width / viewport[2] : -1;
    mouseY = active ? cursor.y * height / client.bottom : -1;
    using UseProgram = void(APIENTRY*)(GLuint);
    using ActiveTexture = void(APIENTRY*)(GLenum);
    static auto useProgram = reinterpret_cast<UseProgram>(wglGetProcAddress("glUseProgram"));
    static auto activeTexture = reinterpret_cast<ActiveTexture>(wglGetProcAddress("glActiveTexture"));
    GLint program = 0, textureUnit = 0, matrixMode = 0;
    glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
    if (useProgram) { glGetIntegerv(0x8B8D, &program); useProgram(0); }
    if (activeTexture) { glGetIntegerv(0x84E0, &textureUnit); activeTexture(0x84C0); }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_LIGHTING); glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_FOG); glDisable(GL_COLOR_LOGIC_OP);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    if (!fontAttempted) { fontAttempted = true; if (!loadFont()) { monitor::write("log", "Multiplayer font could not load"); leave(); } }
    if (font) { buttonCount = 0; lobby(width, height); previousCount = buttonCount; focus %= (std::max)(1, buttonCount); }
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(matrixMode);
    glPopAttrib();
    if (activeTexture) activeTexture(textureUnit);
    if (useProgram) useProgram(program);
}

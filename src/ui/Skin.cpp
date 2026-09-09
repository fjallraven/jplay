#include "Skin.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace jplay {

namespace {

// Indexed by SkinSlice; must stay in the enum's order.
const char* const kSliceFiles[(int)SkinSlice::Count] = {
    "chrome-topbar",
    "chrome-navstrip",
    "chrome-dropdownbar",

    "button-idle",
    "button-hover",
    "button-disabled",
    "uibutton-idle",
    "uibutton-hover",
    "uibutton-on",
    "uibutton-on-hover",
    "nav-button",
    "topbutton-idle",
    "topbutton-hover",
    "topbutton-on",
    "topbutton-on-hover",

    "input-idle",
    "input-focus",
    "picker-idle",
    "picker-open",
    "dbarbox-idle",
    "dbarbox-open",

    "list",
    "menu",
    "menu-title",
    "tooltip",
    "curve-plot",

    "segment-off",
    "segment-on",
    "checkbox-off",
    "checkbox-on",
    "radio",

    "tool-off",
    "tool-on",
    "tab-off",
    "tab-on",

    "slider-track",
    "slider-knob",
    "menuslider-groove",
    "menuslider-knob",

    "scrollbar-track",
    "scrollbar-thumb",
    "scrollbar-thumb-bare",
};

std::filesystem::path skinsRoot() {
    const char* base = SDL_GetBasePath(); // exe dir, trailing sep; do not free
    return std::filesystem::path(base ? base : "") / "skins";
}

SkinColors& mutableColors() {
    static SkinColors c;
    return c;
}

// Read the "colors" block of skin.json over the palette. Anything the file omits
// or spells wrongly keeps its default, so a partial or older manifest degrades to
// the shipped look rather than to black. Returns the number of colours applied,
// or -1 when the file could not be read or parsed at all.
int loadColors(const std::filesystem::path& file) {
    SDL_IOStream* io = SDL_IOFromFile(file.string().c_str(), "rb");
    if (!io) return -1;
    size_t size = 0;
    void* data = SDL_LoadFile_IO(io, &size, true); // closes io
    if (!data) return -1;
    std::string text((const char*)data, size);
    SDL_free(data);

    rapidjson::Document doc;
    if (doc.Parse(text.c_str()).HasParseError() || !doc.IsObject()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Skin: %s is not valid JSON",
                    file.string().c_str());
        return -1;
    }
    auto it = doc.FindMember("colors");
    if (it == doc.MemberEnd() || !it->value.IsObject()) return 0;

    SkinColors& out = mutableColors();
    int applied = 0;
    for (const SkinColorSlot& slot : kSkinColorSlots) {
        auto c = it->value.FindMember(slot.key);
        if (c == it->value.MemberEnd() || !c->value.IsArray() || c->value.Size() < 3)
            continue;
        const auto& a = c->value;
        auto ch = [&](rapidjson::SizeType i, Uint8 fallback) -> Uint8 {
            if (i >= a.Size() || !a[i].IsNumber()) return fallback;
            return (Uint8)std::clamp(a[i].GetInt(), 0, 255);
        };
        SDL_Color col{ ch(0, 0), ch(1, 0), ch(2, 0), ch(3, 255) };
        std::memcpy((char*)&out + slot.off, &col, sizeof(col));
        ++applied;
    }
    return applied;
}

} // namespace

bool Skin::load(SDL_Renderer* r, const std::string& name) {
    unload();
    if (!r || name.empty()) return false;

    const std::filesystem::path dir = skinsRoot() / name;

    int ok = 0;
    for (int i = 0; i < (int)SkinSlice::Count; ++i) {
        const std::filesystem::path p = dir / (std::string(kSliceFiles[i]) + ".9.bmp");
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) continue; // absent: caller falls back
        if (NineSlice_LoadBMP(r, p.string().c_str(), slices_[i])) ++ok;
    }

    // Colours are independent of the art: a skin may ship one, the other or both.
    // Reset first so switching skins can't inherit the previous one's palette.
    mutableColors() = SkinColors{};
    const int cols = loadColors(dir / "skin.json");

    name_ = name;
    loaded_ = ok > 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Skin: %s — %d/%d surfaces, %d/%d colours",
                dir.string().c_str(), ok, (int)SkinSlice::Count, std::max(cols, 0),
                kSkinColorCount);
    return loaded_;
}

void Skin::unload() {
    for (NineSlice& s : slices_) NineSlice_Destroy(s);
    name_.clear();
    loaded_ = false;
}

const NineSlice* Skin::get(SkinSlice s) const {
    const int i = (int)s;
    if (i < 0 || i >= (int)SkinSlice::Count) return nullptr;
    return slices_[i].tex ? &slices_[i] : nullptr;
}

bool Skin::draw(SDL_Renderer* r, SkinSlice s, const SDL_FRect& dst, Uint8 alpha) const {
    const NineSlice* ns = get(s);
    if (!ns) return false;
    if (alpha != 255) {
        SDL_SetTextureAlphaMod(ns->tex, alpha);
        NineSlice_Draw(r, *ns, dst);
        SDL_SetTextureAlphaMod(ns->tex, 255); // shared texture: leave it opaque
        return true;
    }
    NineSlice_Draw(r, *ns, dst);
    return true;
}

Skin& skin() {
    static Skin s;
    return s;
}

const SkinColors& colors() {
    return mutableColors();
}

std::vector<std::string> skinNames() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(skinsRoot(), ec)) {
        if (e.is_directory(ec)) out.push_back(e.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace jplay

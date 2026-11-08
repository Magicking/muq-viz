#include "ObsLayout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace muq {

using nlohmann::json;

namespace {

constexpr int OBS_ALIGN_LEFT = 1 << 0;
constexpr int OBS_ALIGN_RIGHT = 1 << 1;
constexpr int OBS_ALIGN_TOP = 1 << 2;
constexpr int OBS_ALIGN_BOTTOM = 1 << 3;

double Num(const json& obj, const char* key, double fallback)
{
    if (obj.is_object())
    {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_number())
        {
            return it->get<double>();
        }
    }
    return fallback;
}

double Vec(const json& obj, const char* key, const char* axis, double fallback)
{
    if (obj.is_object())
    {
        auto it = obj.find(key);
        if (it != obj.end())
        {
            return Num(*it, axis, fallback);
        }
    }
    return fallback;
}

// Offset of the alignment point from the item's top-left corner, along one axis.
double AlignOffset(int align, int lowBit, int highBit, double size)
{
    if (align & lowBit)
    {
        return 0.0;
    }
    if (align & highBit)
    {
        return size;
    }
    return size / 2.0;
}

} // namespace

Layout ParseObsLayout(const std::string& text, const LayoutOptions& options)
{
    json doc;
    try
    {
        doc = json::parse(text);
    }
    catch (const json::parse_error& e)
    {
        throw std::runtime_error(std::string("invalid JSON: ") + e.what());
    }

    if (!doc.is_object() || !doc.contains("sources") || !doc["sources"].is_array())
    {
        throw std::runtime_error("not an OBS scene collection (missing \"sources\" array)");
    }

    Layout layout;
    const json& sources = doc["sources"];

    std::map<std::string, const json*> byUuid;
    std::map<std::string, const json*> byName;
    for (const auto& src : sources)
    {
        if (src.contains("uuid") && src["uuid"].is_string())
        {
            byUuid[src["uuid"].get<std::string>()] = &src;
        }
        if (src.contains("name") && src["name"].is_string())
        {
            byName[src["name"].get<std::string>()] = &src;
        }
    }

    // Pick the scene.
    std::string sceneName = options.scene;
    if (sceneName.empty() && doc.contains("current_scene") && doc["current_scene"].is_string())
    {
        sceneName = doc["current_scene"].get<std::string>();
    }
    const json* scene = nullptr;
    for (const auto& src : sources)
    {
        if (src.value("id", "") != "scene")
        {
            continue;
        }
        if (sceneName.empty() || src.value("name", "") == sceneName)
        {
            scene = &src;
            break;
        }
    }
    if (!scene)
    {
        throw std::runtime_error("scene \"" + sceneName + "\" not found");
    }
    layout.sceneName = scene->value("name", "");

    const json& settings = (*scene).contains("settings") ? (*scene)["settings"] : json::object();
    if (!settings.contains("items") || !settings["items"].is_array())
    {
        throw std::runtime_error("scene \"" + layout.sceneName + "\" has no items");
    }

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;

    for (const auto& item : settings["items"])
    {
        const std::string name = item.value("name", "");
        if (name.compare(0, options.prefix.size(), options.prefix) != 0)
        {
            continue;
        }
        if (!options.includeHidden && !item.value("visible", true))
        {
            layout.warnings.push_back(name + ": hidden, skipped");
            continue;
        }

        const json* source = nullptr;
        if (item.contains("source_uuid") && item["source_uuid"].is_string())
        {
            auto it = byUuid.find(item["source_uuid"].get<std::string>());
            if (it != byUuid.end())
            {
                source = it->second;
            }
        }
        if (!source)
        {
            auto it = byName.find(name);
            if (it != byName.end())
            {
                source = it->second;
            }
        }

        // Canvas size: CLI override, else the scale reference stored on the item.
        double canvasW = options.canvasWidth > 0 ? options.canvasWidth : Vec(item, "scale_ref", "x", 1920.0);
        double canvasH = options.canvasHeight > 0 ? options.canvasHeight : Vec(item, "scale_ref", "y", 1080.0);

        // Base (unscaled) source size. Sources without explicit size (e.g. color_source
        // defaults) use the canvas size, which is what OBS does for color sources.
        double baseW = canvasW;
        double baseH = canvasH;
        if (source && source->contains("settings"))
        {
            const json& ss = (*source)["settings"];
            baseW = Num(ss, "width", Num(ss, "cx", canvasW));
            baseH = Num(ss, "height", Num(ss, "cy", canvasH));
        }
        else if (!source)
        {
            layout.warnings.push_back(name + ": source not found, assuming canvas size");
        }

        const double cropL = Num(item, "crop_left", 0), cropR = Num(item, "crop_right", 0);
        const double cropT = Num(item, "crop_top", 0), cropB = Num(item, "crop_bottom", 0);
        const double scaleX = std::fabs(Vec(item, "scale", "x", 1.0));
        const double scaleY = std::fabs(Vec(item, "scale", "y", 1.0));

        double w = std::max(0.0, baseW - cropL - cropR) * scaleX;
        double h = std::max(0.0, baseH - cropT - cropB) * scaleY;

        const int boundsType = static_cast<int>(Num(item, "bounds_type", 0));
        if (boundsType != 0)
        {
            w = Vec(item, "bounds", "x", w);
            h = Vec(item, "bounds", "y", h);
        }

        const int align = static_cast<int>(Num(item, "align", OBS_ALIGN_LEFT | OBS_ALIGN_TOP));
        const double px = Vec(item, "pos", "x", 0.0);
        const double py = Vec(item, "pos", "y", 0.0);
        const double x = px - AlignOffset(align, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT, w);
        const double y = py - AlignOffset(align, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM, h);

        if (std::fabs(Num(item, "rot", 0.0)) > 0.01)
        {
            layout.warnings.push_back(name + ": rotation is not supported, ignored");
        }

        Surface s;
        s.name = name;
        s.rect.x = static_cast<int>(std::lround(x));
        s.rect.y = static_cast<int>(std::lround(y));
        s.rect.w = static_cast<int>(std::lround(w));
        s.rect.h = static_cast<int>(std::lround(h));
        if (s.rect.w <= 0 || s.rect.h <= 0)
        {
            layout.warnings.push_back(name + ": empty size, skipped");
            continue;
        }

        minX = std::min(minX, s.rect.x);
        minY = std::min(minY, s.rect.y);
        maxX = std::max(maxX, s.rect.x + s.rect.w);
        maxY = std::max(maxY, s.rect.y + s.rect.h);
        layout.surfaces.push_back(std::move(s));
    }

    if (layout.surfaces.empty())
    {
        throw std::runtime_error("no surface named \"" + options.prefix + "*\" in scene \"" + layout.sceneName + "\"");
    }

    // OBS stores items top-most first; sort by name for stable, readable ordering.
    std::sort(layout.surfaces.begin(), layout.surfaces.end(),
              [](const Surface& a, const Surface& b) { return a.name < b.name; });

    layout.bounds = {minX, minY, maxX - minX, maxY - minY};
    return layout;
}

Layout LoadObsLayout(const std::string& path, const LayoutOptions& options)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot open layout file: " + path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ParseObsLayout(ss.str(), options);
}

} // namespace muq

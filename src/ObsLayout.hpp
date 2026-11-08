#pragma once
// Parser for OBS scene-collection JSON exports: every scene item whose name starts
// with a prefix (default "S") becomes an output surface. Geometry is taken from the
// scene item transform (pos/scale/align/crop/bounds) and the source's base size.

#include <string>
#include <vector>

namespace muq {

struct Rect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Surface
{
    std::string name;
    Rect rect; // in OBS canvas pixels, top-left origin
};

struct Layout
{
    std::string sceneName;
    std::vector<Surface> surfaces;
    Rect bounds; // bounding box of all surfaces (the rendered canvas)
    std::vector<std::string> warnings;
};

struct LayoutOptions
{
    std::string prefix = "S";
    std::string scene;       // empty = current_scene
    int canvasWidth = 0;     // 0 = derive from item scale_ref (fallback 1920x1080)
    int canvasHeight = 0;
    bool includeHidden = false;
};

/// Parses an OBS JSON file. Throws std::runtime_error on invalid input or when no surface is found.
Layout LoadObsLayout(const std::string& path, const LayoutOptions& options = {});

/// Same as LoadObsLayout, but from a JSON string.
Layout ParseObsLayout(const std::string& json, const LayoutOptions& options = {});

} // namespace muq

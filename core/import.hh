// Model import: a glTF 2.0 (.gltf, .glb), FBX or OBJ (with its .mtl) file turned
// into a project .mesh plus the PNG textures its materials use.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

struct ImportedModel {
    std::string mesh;                 // project-relative path of the written .mesh
    std::vector<Material> materials;  // one per submesh, textures project-relative
    std::vector<std::string> textures;  // every PNG written, project-relative
    int triangles = 0;
    std::vector<std::string> warnings;
};

// File extensions importModel reads, lower case with the dot.
bool canImportModel(const std::filesystem::path& file);

// Reads `source` and writes models/<stem>.mesh and models/<stem>_<n>.png into
// `project`, replacing earlier imports of the same name. Every mesh node of
// the file (the default scene for glTF) is baked into one mesh in the file's
// root space, one submesh per material. glTF is read through cgltf, FBX and
// OBJ through ufbx, which first converts their axes and units to glTF's
// (right-handed, Y up, +Z front, metres). From there all three take the same
// route to the left-handed space a .mesh uses: X mirrored, winding reversed
// (again for each mirrored node). UVs end up with a bottom-left origin: glTF's
// are flipped, FBX and OBJ already have it. Base colour textures come from the
// glTF buffers or files; for FBX and OBJ from embedded data, the path in the
// file, then the bare name in the model's folder and its textures/tex/
// materials folders (one not found is a warning, not an error). Textures
// larger than 256 texels on a side are scaled down to fit.
// Throws std::runtime_error with a message for the user.
ImportedModel importModel(const std::filesystem::path& source, const std::filesystem::path& project);

}  // namespace splash

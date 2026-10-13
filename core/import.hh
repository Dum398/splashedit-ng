// Model import: a glTF 2.0 (.gltf, .glb), FBX or OBJ (with its .mtl) file turned
// into a project .mesh plus the PNG textures its materials use.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

// One node of the model's hierarchy. The transform is local to the parent
// node (to the model's root for a top-level node) and already in the .mesh
// space: X mirrored in the position, the rotation (x, y, z, w) taken to
// (x, -y, -z, w), the scale as it is. Composing it down the tree over `mesh`
// gives the node's part of the baked mesh.
struct ImportedNode {
    std::string name;
    int parent = -1;  // index into ImportedModel::nodes, -1 for a top-level node
    Transform transform;
    std::string mesh;                 // project-relative .mesh in the node's own space, empty for none
    std::vector<Material> materials;  // one per submesh of `mesh`
};

struct ImportedModel {
    std::string mesh;                 // project-relative path of the written .mesh
    std::vector<Material> materials;  // one per submesh, textures project-relative
    std::vector<ImportedNode> nodes;  // the node tree, every parent before its children
    std::vector<std::string> textures;  // every PNG written, project-relative
    int triangles = 0;
    std::vector<std::string> warnings;
};

// File extensions importModel reads, lower case with the dot.
bool canImportModel(const std::filesystem::path& file);

// Reads `source` and writes models/<stem>.mesh and models/<stem>_<n>.png into
// `project`, replacing earlier imports of the same name. Every mesh node of
// the file (the default scene for glTF) is baked into one mesh in the file's
// root space, one submesh per material. The node tree is kept too: every node
// with a mesh also gets models/<stem>/<node>.mesh in its own space (names made
// file-safe and unique). glTF is read through cgltf, FBX and
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

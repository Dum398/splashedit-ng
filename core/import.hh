// Model import: a glTF 2.0 (.gltf, .glb), FBX, OBJ (with its .mtl) or .blend
// file turned into a project .mesh plus the PNG textures its materials use.
#pragma once

#include <cstdint>
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
    // `mesh` carries a skeleton. Its space is then the baked mesh's: the
    // transform puts the node at the model's root, as glTF ignores a skinned
    // node's own transform.
    bool skinned = false;
};

struct ImportedModel {
    std::string mesh;                 // project-relative path of the written .mesh
    std::vector<Material> materials;  // one per submesh, textures project-relative
    std::vector<ImportedNode> nodes;  // the node tree, every parent before its children
    std::vector<std::string> textures;  // every PNG written, project-relative
    bool skinned = false;               // the baked mesh carries a skeleton
    std::vector<std::string> clips;     // every .anim written, project-relative, in file order
    int triangles = 0;
    std::vector<std::string> warnings;
};

// How a model is imported, kept per model file in a sidecar beside it (see
// importSettingsPath), the way Unity keeps a model's import settings.
enum class UpAxis : uint8_t { File, YUp, ZUp };
enum class Normals : uint8_t { Import, Recompute };
struct ImportSettings {
    float scale = 1;  // multiplies every position and node translation
    // File: the axes the file declares. YUp, ZUp: the data is taken as
    // right-handed Y up (+Z front) or Z up (-Y front) whatever the file says.
    UpAxis up = UpAxis::File;
    // Recompute: every corner gets the average of the face normals around its
    // position that are within smoothingAngle degrees of its own face's, so a
    // sharper edge stays hard. Import: the file's normals, flat where it has none.
    Normals normals = Normals::Import;
    float smoothingAngle = 60;
    // Objects the editor makes from the model: one object drawing the baked
    // mesh instead of the node tree, and these on each mesh component.
    bool mergeMeshes = false;
    BitDepth bitDepth = MeshComponent{}.bitDepth;
    DynamicLighting lighting = MeshComponent{}.dynamicLighting;
    // Absolute path of the file the model was copied into the project from;
    // empty when it was in the project already.
    std::string source;
    ExtraKeys extra;
};

// models/crate.fbx -> models/crate.fbx.import.json
std::filesystem::path importSettingsPath(const std::filesystem::path& model);
// The settings in `model`'s sidecar, the defaults when it has none. Throws
// std::runtime_error when the sidecar cannot be read.
ImportSettings loadImportSettings(const std::filesystem::path& model);
// Writes `model`'s sidecar. Throws std::runtime_error on failure.
void saveImportSettings(const std::filesystem::path& model, const ImportSettings& settings);

// File extensions importModel reads, lower case with the dot.
bool canImportModel(const std::filesystem::path& file);

// The name importModel writes `file`'s outputs under: models/<name>.mesh.
std::string importName(const std::filesystem::path& file);

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
// A skinned mesh (glTF skin, FBX skin deformer) keeps its skeleton: the joints
// of every skin in one list in tree order, each named after its node (made
// unique). Its geometry stays in root space whatever the skinned node's
// transform, and each vertex keeps its four heaviest influences, normalised.
// Every animation in the file becomes models/<stem>/<clip>.anim, channels by
// joint name in the .mesh space and keys in seconds: glTF keys as they are
// (cubic-spline ones by their values), FBX baked at 30 Hz.
// `settings` then apply in the .mesh space: an up axis other than File turns
// the model (Z up: -90 degrees about X, so +Z becomes +Y), scale multiplies
// every position and node translation, and Recompute replaces the normals.
// The outputs are named after `as` (see importName) when it is given, so a
// model read from where it was dropped is named after its copy in the project.
// A .blend is first exported as a GLB by Blender run in the background (see
// blend.hh), into a temporary folder, and that GLB is imported as above.
// Throws std::runtime_error with a message for the user.
ImportedModel importModel(const std::filesystem::path& source, const std::filesystem::path& project,
                          const ImportSettings& settings = {}, const std::filesystem::path& as = {});

}  // namespace splash

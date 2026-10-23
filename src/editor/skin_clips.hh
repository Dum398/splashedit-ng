// A skin's clips beyond its own model's (#237): animations from other model
// files (Mixamo writes one per file) imported onto its skeleton, and clip
// names given in the editor that survive importing again.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "editor/document.hh"
#include "import.hh"
#include "scene.hh"

namespace editor {

// Clips from model files for the skin of the object at `object`, one undo
// step. Each file is adopted into models/ (with a sidecar naming where it
// came from, so a change there is noticed), and its animations imported onto
// the skeleton of the object's mesh by joint name, with the import settings
// of the model that mesh came from, into models/<model>/clips/<file>/ (see
// splash::importClips), where importing the model again leaves them. They
// join the skin's clips (a file added again replaces its clips in place), up
// to 16, and the file the skin's clipSources. A file sharing no joint with the
// skeleton, or with no animation of it, is refused and the others still added.
struct AddedClips {
    std::string error;                  // nothing could be done (no skin, a mesh from no model)
    std::vector<std::string> refused;   // "<file>: why", per file left out
    std::vector<std::string> warnings;  // "<file>: warning"
    std::vector<std::string> clips;     // the clips added, project-relative
    int left = 0;                       // clips that did not fit in the 16
};
AddedClips addClipSources(Document& doc, const ObjectPath& object, const std::vector<std::filesystem::path>& files);

// Whether a skin in the open scene takes clips from `model` (absolute, or
// relative to the project).
bool isClipSource(const Document& doc, const std::filesystem::path& model);

// The skins taking clips from `model`, as a clip source, get its clips
// imported again, one undo step. Returns how many objects changed; what went
// wrong goes into `problems`.
int refreshClipSources(Document& doc, const std::filesystem::path& model, std::vector<std::string>* problems = nullptr);

// For a reimport of the skin's own model (see applyReimport): the clips of
// every clip source imported again with the model's settings now. Returns
// whether `skin` changed.
bool refreshSkinSources(const std::filesystem::path& project, const std::string& mesh, splash::SkinComponent& skin,
                        std::vector<std::string>* problems = nullptr);

// Gives clip `index` of the skin on `object` the name SkinnedAnim.Play takes,
// one undo step. The name is trimmed and must be 1..24 characters (UTF-16
// units, as the export counts) and differ, in any case, from the skin's
// other clips' names. A clip its model's import wrote (models/<model>/...)
// keeps the name in the model's sidecar (clipNames) as well as in the .anim,
// so importing again keeps it; another clip has it in its .anim only.
// Returns the error, empty when renamed or already so named.
std::string renameSkinClip(Document& doc, const ObjectPath& object, size_t index, const std::string& name);

}  // namespace editor

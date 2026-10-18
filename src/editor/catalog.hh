// What can be added to a scene from the editor: the component types an object
// can carry and the starting objects the Add object picker offers, plus the
// search that ranks them as the user types.
#pragma once

#include <string>
#include <vector>

#include "scene.hh"

namespace editor {

enum class ComponentKind { Mesh, Collider, Script, Light, Player, Navigation, Trigger, Interactable, Audio, Skin, Room, Portal, Walkoff, Agent };

struct ComponentInfo {
    ComponentKind kind;
    const char* label;     // "Trigger box"
    const char* keywords;  // extra words the search matches, space separated
    const char* blurb;     // one line for the picker
};

// Every component type, in picker order.
const std::vector<ComponentInfo>& components();
const ComponentInfo& info(ComponentKind k);

bool hasComponent(const splash::Object& o, ComponentKind k);
// Why `k` cannot be added to `o` right now ("Already added", "Needs a Mesh"),
// or empty when it can.
std::string cannotAdd(const splash::Object& o, ComponentKind k);
// Adds `k` with its defaults. Returns false (and changes nothing) when cannotAdd is set.
bool addComponent(splash::Object& o, ComponentKind k);
// Removes `k`. A mesh takes its skin with it, since a skin cannot exist alone.
bool removeComponent(splash::Object& o, ComponentKind k);

// A starting object for the Add object picker.
struct ObjectPreset {
    const char* label;
    const char* keywords;
    const char* blurb;
    std::vector<ComponentKind> parts;
};
const std::vector<ObjectPreset>& presets();
// The preset as an object at the origin, named `label` made unique among `siblings`
// the way Unity names copies ("Point light (2)").
splash::Object makeObject(const ObjectPreset& p, const std::vector<splash::Object>& siblings);
std::string uniqueName(const std::string& base, const std::vector<splash::Object>& siblings);

// Search score of `query` against a label and its keywords; 0 = no match.
// Higher is better: whole label prefix, then a word prefix, then a substring,
// then the query's letters in order (fuzzy). Case-insensitive. An empty query
// matches everything with the same score, so the list keeps its order.
int matchScore(const std::string& query, const char* label, const char* keywords);
// Indices of the candidates that match, best first; ties keep their order.
std::vector<int> rank(const std::string& query, const std::vector<std::pair<const char*, const char*>>& labelsAndKeywords);

}  // namespace editor

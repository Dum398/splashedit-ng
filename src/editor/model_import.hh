// Model imports off the UI thread: the queue the editor runs dropped, picked
// and changed models through, one at a time in the order asked. A .blend is
// first exported as a GLB by Blender (splash::BlendExport); every model is
// then read and baked on a worker thread (buildStaged), and only the change
// to the scene is left for the UI thread (applyPlace / applyReimport).
// This half has no ImGui; the status bar and the placing live in the app.
#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/document.hh"
#include "editor/project.hh"
#include "unitymath.hh"

namespace splash {
class BlendExport;
}

namespace editor {

// Where an imported model goes. With none of these set, a new object goes
// where the Add object picker would put it.
struct ModelTarget {
    std::optional<std::vector<int>> replace;  // the Model field's object: its model is swapped
    std::optional<std::vector<int>> under;    // a scene tree row: added as its last child
    std::optional<splash::Vec3> at;           // the viewport: added at the top level, here (world)
};

// buildStaged on a worker thread. Destroying or cancelling it never waits:
// a cancelled build finishes on its own and removes what it wrote.
class StagedBuild {
  public:
    // `keep` is held until the build is over (the BlendExport whose GLB it reads).
    StagedBuild(StagedModel staged, std::shared_ptr<void> keep = {});
    ~StagedBuild();
    StagedBuild(const StagedBuild&) = delete;
    StagedBuild& operator=(const StagedBuild&) = delete;

    bool done() const;
    // Waits up to `timeout` for done(); false when it is still running.
    bool wait(std::chrono::milliseconds timeout) const;
    void cancel();
    // Once done() and not cancelled: the built model, to apply or discard.
    StagedModel take();

  private:
    struct Shared;
    std::shared_ptr<Shared> m;
};

class ModelImports {
  public:
    struct Request {
        std::filesystem::path file;          // placed: the file dropped or picked
        ModelTarget to;                      // where a placed model goes
        std::filesystem::path reimport;      // set: the project model to import again instead
        std::optional<bool> wasMerged;       // for a reimport
        unsigned loadId = 0;                 // the scene it was asked for (Document::loadId)
    };
    // What became of the import at the front. Ready: `staged` is built, for
    // applyPlace / applyReimport. Dropped: another scene was opened.
    struct Outcome {
        enum Kind { Ready, Failed, Cancelled, Dropped } kind = Failed;
        Request request;
        StagedModel staged;
        std::string error;
    };
    enum class Stage { Idle, Blender, Import };

    ModelImports();
    ~ModelImports();

    // Queues an import. A reimport of a model already waiting (not started)
    // is not queued twice: that one will read the newest file.
    void add(Request r);
    // Advances the import at the front, starting it when it is new; call
    // once a frame on the UI thread. Returns what ended this call, in order.
    std::vector<Outcome> update(const Document& doc);
    // Stops the running import: Blender is killed, a build is left to finish
    // on its own and thrown away. update() reports it Cancelled, nothing applied.
    // Destroying the queue waits up to 10 s for such builds to clean up.
    void cancel();

    bool busy() const { return !m_queue.empty(); }
    size_t size() const { return m_queue.size(); }
    // The running import (nullptr when none has started), its stage, the
    // seconds since it started, the update() calls it has been running for
    // (frames the editor drew meanwhile), and whether it is done and only
    // waits to be applied.
    const Request* current() const;
    Stage stage() const;
    double seconds() const;
    int frames() const { return m_frames; }
    bool finishing() const;

  private:
    struct Item;
    std::deque<std::unique_ptr<Item>> m_queue;
    std::vector<std::unique_ptr<StagedBuild>> m_abandoned;  // cancelled builds still running
    int m_frames = 0;
};

}  // namespace editor

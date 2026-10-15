// Re-exports the scene in the background after edits settle, without writing
// files, so the editor can show what the scene costs on the console.
#pragma once

#include <future>
#include <optional>

#include "editor/document.hh"
#include "splashpack.hh"

namespace editor {

class LiveExport {
public:
    // How long the scene must stay unchanged before an export starts.
    explicit LiveExport(double settleSeconds = 0.3) : m_settle(settleSeconds) {}
    ~LiveExport();
    // Call once a frame. `now` is in seconds, from any monotonic clock.
    void update(const Document& doc, double now);
    // Blocks until a running export finishes (tests).
    void wait();
    // The last finished export, for the revision in resultRevision().
    const std::optional<splash::ExportResult>& result() const { return m_result; }
    unsigned resultRevision() const { return m_resultRev; }
    bool busy() const { return m_job.valid(); }
    // Something the export reads besides the scene changed (the project's
    // vram.json): the next update() exports again, after the settle time,
    // though the scene's revision is the same.
    void settingsChanged() { m_settingsGen++; }

private:
    void collect();
    double m_settle;
    std::future<splash::ExportResult> m_job;
    unsigned m_jobRev = 0, m_resultRev = 0;
    std::optional<unsigned> m_seenRev;  // revision last seen by update()
    unsigned m_settingsGen = 0, m_seenGen = 0;
    double m_seenAt = 0;
    bool m_started = false;  // an export has been started for m_seenRev
    std::optional<splash::ExportResult> m_result;
};

}  // namespace editor

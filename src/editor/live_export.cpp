#include "editor/live_export.hh"

#include "editor/project.hh"

#include <chrono>
#include <exception>

namespace editor {

LiveExport::~LiveExport() {
    if (m_job.valid()) m_job.wait();
}

void LiveExport::collect() {
    if (!m_job.valid() || m_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    m_result = m_job.get();
    m_resultRev = m_jobRev;
}

void LiveExport::wait() {
    if (m_job.valid()) m_job.wait();
    collect();
}

void LiveExport::update(const Document& doc, double now) {
    collect();
    if (!m_seenRev || *m_seenRev != doc.revision() || m_seenGen != m_settingsGen) {
        m_seenRev = doc.revision();
        m_seenGen = m_settingsGen;
        m_seenAt = now;
        m_started = false;
    }
    if (m_started || m_job.valid() || now - m_seenAt < m_settle) return;
    m_started = true;
    m_jobRev = doc.revision();
    std::filesystem::path root = doc.projectRoot();
    m_job = std::async(std::launch::async, [scene = doc.scene(), root] {
        splash::ExportOptions opt;
        opt.dryRun = true;
        opt.vram = loadVramSettings(root);
        try {
            return splash::exportSplashpack(scene, root, root / "scene.splashpack", opt);
        } catch (const std::exception& e) {
            splash::ExportResult r;
            r.errors.push_back(e.what());
            return r;
        }
    });
}

}  // namespace editor

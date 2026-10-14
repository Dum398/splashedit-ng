#include "editor/model_import.hh"

#include <cctype>
#include <cstdlib>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include "blend.hh"

namespace fs = std::filesystem;

namespace editor {

struct StagedBuild::Shared {
    std::mutex mutex;
    std::condition_variable finished;
    StagedModel staged;
    std::shared_ptr<void> keep;
    splash::ImportProgress progress;
    bool done = false, cancelled = false, taken = false;
};

StagedBuild::StagedBuild(StagedModel staged, std::shared_ptr<void> keep) : m(std::make_shared<Shared>()) {
    m->staged = std::move(staged);
    m->keep = std::move(keep);
    // The thread owns its share of the state, so the build can outlive this
    // object: whichever of it and cancel() comes last removes the outputs.
    std::thread([sh = m] {
        StagedModel s;
        {
            std::lock_guard<std::mutex> lock(sh->mutex);
            s = std::move(sh->staged);
        }
        buildStaged(s, &sh->progress);
        // Tests stretch the build so a fast machine cannot finish it before
        // the check looks at the running state; a cancel cuts it short.
        if (const char* ms = std::getenv("SPLASHEDIT_IMPORT_DELAY_MS")) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::atoi(ms));
            while (!sh->progress.cancel && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> lock(sh->mutex);
        sh->staged = std::move(s);
        sh->done = true;
        sh->keep.reset();
        if (sh->cancelled) discardStaged(sh->staged);
        sh->finished.notify_all();
    }).detach();
}

StagedBuild::~StagedBuild() { cancel(); }

bool StagedBuild::done() const {
    std::lock_guard<std::mutex> lock(m->mutex);
    return m->done;
}

bool StagedBuild::wait(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(m->mutex);
    return m->finished.wait_for(lock, timeout, [&] { return m->done; });
}

void StagedBuild::cancel() {
    std::lock_guard<std::mutex> lock(m->mutex);
    if (m->taken || m->cancelled) return;
    m->cancelled = true;
    m->progress.cancel = true;
    if (m->done) discardStaged(m->staged);
}

float StagedBuild::fraction() const { return m->progress.fraction; }

unsigned StagedBuild::checks() const { return m->progress.checks; }

StagedModel StagedBuild::take() {
    std::lock_guard<std::mutex> lock(m->mutex);
    if (!m->done || m->cancelled || m->taken) {
        StagedModel none;
        none.error = "the import is not done";
        return none;
    }
    m->taken = true;
    return std::move(m->staged);
}

struct ModelImports::Item {
    Request request;
    std::shared_ptr<splash::BlendExport> blender;  // a .blend, while Blender runs and the build reads its GLB
    std::unique_ptr<StagedBuild> build;
    bool cancelled = false;
    std::chrono::steady_clock::time_point started;
};

ModelImports::ModelImports() = default;

ModelImports::~ModelImports() {
    // Cancelled builds still running remove their temporary folder when they
    // end; give them a little while to, as the editor closes.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (std::unique_ptr<Item>& it : m_queue)
        if (it->build) it->build->cancel();
    for (std::unique_ptr<Item>& it : m_queue)
        if (it->build) m_abandoned.push_back(std::move(it->build));
    for (const std::unique_ptr<StagedBuild>& b : m_abandoned) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        if (left.count() <= 0 || !b->wait(left)) break;
    }
}

static bool isBlend(const fs::path& p) {
    std::u8string u = p.extension().u8string();
    std::string e(u.begin(), u.end());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".blend";
}

void ModelImports::add(Request r) {
    if (!r.reimport.empty())
        for (const std::unique_ptr<Item>& it : m_queue)
            if (!it->blender && !it->build && !it->cancelled && it->request.reimport == r.reimport) {
                if (r.wasMerged) it->request.wasMerged = r.wasMerged;
                return;
            }
    auto it = std::make_unique<Item>();
    it->request = std::move(r);
    m_queue.push_back(std::move(it));
}

void ModelImports::cancel() {
    if (m_queue.empty()) return;
    Item& it = *m_queue.front();
    if (!it.blender && !it.build) return;
    it.cancelled = true;
    if (it.blender) it.blender->cancel();
    if (it.build) it.build->cancel();
}

const ModelImports::Request* ModelImports::current() const {
    if (m_queue.empty() || (!m_queue.front()->blender && !m_queue.front()->build)) return nullptr;
    return &m_queue.front()->request;
}

ModelImports::Stage ModelImports::stage() const {
    if (m_queue.empty()) return Stage::Idle;
    const Item& it = *m_queue.front();
    if (it.build) return Stage::Import;
    return it.blender ? Stage::Blender : Stage::Idle;
}

double ModelImports::seconds() const {
    if (!current()) return 0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_queue.front()->started).count();
}

bool ModelImports::finishing() const {
    if (m_queue.empty()) return false;
    const Item& it = *m_queue.front();
    return it.build ? it.build->done() : it.blender && it.blender->done();
}

float ModelImports::fraction() const {
    if (m_queue.empty() || !m_queue.front()->build) return -1;
    return m_queue.front()->build->fraction();
}

std::vector<ModelImports::Outcome> ModelImports::update(const Document& doc) {
    std::vector<Outcome> out;
    std::erase_if(m_abandoned, [](const std::unique_ptr<StagedBuild>& b) { return b->done(); });
    auto end = [&](Outcome::Kind kind, std::string error = {}, StagedModel staged = {}) {
        Outcome o;
        o.kind = kind;
        o.request = std::move(m_queue.front()->request);
        o.error = std::move(error);
        o.staged = std::move(staged);
        out.push_back(std::move(o));
        if (m_queue.front()->build && !m_queue.front()->build->done()) m_abandoned.push_back(std::move(m_queue.front()->build));
        m_queue.pop_front();  // a running Blender is already being killed; its thread is waited for
    };
    // Imports asked for in a scene that is no longer open are dropped.
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if ((*it)->request.loadId == doc.loadId()) {
            ++it;
            continue;
        }
        if ((*it)->blender) (*it)->blender->cancel();
        if ((*it)->build) {
            (*it)->build->cancel();
            m_abandoned.push_back(std::move((*it)->build));
        }
        Outcome o;
        o.kind = Outcome::Dropped;
        o.request = std::move((*it)->request);
        out.push_back(std::move(o));
        it = m_queue.erase(it);
    }
    while (!m_queue.empty()) {
        Item& it = *m_queue.front();
        const Request& rq = it.request;
        auto stageFor = [&](const fs::path& converted) {
            return rq.reimport.empty() ? stagePlace(doc, rq.file, converted) : stageReimport(doc, rq.reimport, converted);
        };
        if (!it.blender && !it.build) {
            // A cancelled build stops at its next step; the next import
            // waits for that, so builds never overlap.
            if (!m_abandoned.empty()) return out;
            // Folders a killed editor left behind go, once a run.
            static std::once_flag swept;
            std::call_once(swept, [] { sweepStaleImportFolders(); });
            // New: Blender first for a .blend, else straight to the build.
            m_frames = 0;
            it.started = std::chrono::steady_clock::now();
            const fs::path src = rq.reimport.empty() ? rq.file : modelSource(doc, rq.reimport);
            if (isBlend(src) && !doc.projectRoot().empty()) {
                try {
                    it.blender = std::make_shared<splash::BlendExport>(src);
                } catch (const std::exception& e) {
                    end(Outcome::Failed, e.what());
                    continue;
                }
                return out;
            }
            StagedModel s = stageFor({});
            if (!s.error.empty()) {
                end(Outcome::Failed, s.error);
                continue;
            }
            it.build = std::make_unique<StagedBuild>(std::move(s));
            return out;
        }
        if (it.cancelled && it.build) {
            end(Outcome::Cancelled);
            continue;
        }
        if (!it.build) {
            if (!it.blender->done()) {
                ++m_frames;
                return out;
            }
            fs::path glb;
            try {
                glb = it.blender->get();
            } catch (const splash::BlendCancelled&) {
                end(Outcome::Cancelled);
                continue;
            } catch (const std::exception& e) {
                end(it.cancelled ? Outcome::Cancelled : Outcome::Failed, e.what());
                continue;
            }
            if (it.cancelled) {
                end(Outcome::Cancelled);
                continue;
            }
            StagedModel s = stageFor(glb);
            if (!s.error.empty()) {
                end(Outcome::Failed, s.error);
                continue;
            }
            it.build = std::make_unique<StagedBuild>(std::move(s), it.blender);
            ++m_frames;
            return out;
        }
        if (!it.build->done()) {
            ++m_frames;
            return out;
        }
        StagedModel s = it.build->take();
        if (!s.error.empty()) {
            std::string error = s.error;
            discardStaged(s);
            end(Outcome::Failed, error);
            continue;
        }
        end(Outcome::Ready, {}, std::move(s));
        // One import is applied per call, so its scene change lands before the next starts.
        return out;
    }
    return out;
}

}  // namespace editor

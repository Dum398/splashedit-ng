#include "editor/hardware.hh"

#include <fstream>
#include <iterator>

#include "editor/psxmon.hh"
#include "editor/unirom.hh"

namespace editor {

HardwareRun::~HardwareRun() { stop(); }

bool HardwareRun::active() const {
    const Phase p = phase_.load();
    return p == Phase::Connecting || p == Phase::Uploading || p == Phase::Running;
}

std::string HardwareRun::error() const {
    std::lock_guard<std::mutex> lock(m_);
    return error_;
}

std::deque<std::string> HardwareRun::lines() const {
    std::lock_guard<std::mutex> lock(m_);
    return lines_;
}

uint64_t HardwareRun::lineCount() const {
    std::lock_guard<std::mutex> lock(m_);
    return count_;
}

std::deque<std::string> HardwareRun::lines(uint64_t* total) const {
    std::lock_guard<std::mutex> lock(m_);
    if (total) *total = count_;
    return lines_;
}

void HardwareRun::add(std::string line) {
    std::lock_guard<std::mutex> lock(m_);
    lines_.push_back(std::move(line));
    if (lines_.size() > 2000) lines_.pop_front();
    ++count_;
}

void HardwareRun::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(m_);
        error_ = why;
    }
    add(why);
    phase_ = Phase::Failed;
}

HardwareRun::Opener HardwareRun::port(std::string spec) {
    return [spec = std::move(spec)](std::string* err) { return openLink(spec, 115200, err); };
}

void HardwareRun::stop() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    if (active()) phase_ = Phase::Stopped;
}

void HardwareRun::start(Opener open, std::filesystem::path exe, std::filesystem::path dir) {
    stop();
    cancel_ = false;
    progress_ = 0;
    {
        std::lock_guard<std::mutex> lock(m_);
        lines_.clear();
        count_ = 0;
        error_.clear();
    }
    phase_ = Phase::Connecting;
    thread_ = std::thread([this, open = std::move(open), exe = std::move(exe), dir = std::move(dir)] {
        std::ifstream in(exe, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty()) return fail("Cannot read the psxsplash build.");
        std::string err;
        std::unique_ptr<Link> link = open(&err);
        if (!link) return fail(err);
        if (cancel_) {
            phase_ = Phase::Stopped;
            return;
        }
        phase_ = Phase::Uploading;
        // psxmon answers a PING; anything else is taken to be the Unirom shell,
        // which ignores the PING frames as unknown command text.
        uint16_t caps = 0;
        const bool monitor = psxmonPresent(*link, 1500, &caps);
        add(monitor ? "Uploading psxsplash through psxmon..." : "Uploading psxsplash through Unirom...");
        auto progress = [this](int p) { progress_ = p; };
        if (!(monitor ? psxmonUpload(*link, bytes, &err, progress, &cancel_)
                      : uniromUpload(*link, bytes, &err, progress, &cancel_))) {
            if (cancel_) {
                phase_ = Phase::Stopped;
                return;
            }
            return fail(err);
        }
        phase_ = Phase::Running;
        add("Running. Files are served from " + dir.string());
        PcdrvHost host(dir);
        auto line = [this](const std::string& l) { add(l); };
        auto event = [this](const std::string& e) { add("[file] " + e); };
        // Under a psxmon entered from the exception handler's slot, psxsplash
        // asks for files with break calls the monitor stops on; anywhere else
        // it speaks its own SIO1 protocol.
        const bool breaks = monitor && (caps & psxmon::kCapSlot);
        const bool ok = breaks ? psxmonServe(*link, host, cancel_, line, event, &err)
                               : host.serve(*link, cancel_, line, event, &err);
        if (!ok && !cancel_) return fail(err);
        phase_ = Phase::Stopped;
    });
}

}  // namespace editor

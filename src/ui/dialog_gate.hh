// One native file dialog at a time. SDL's dialogs answer on their own
// thread and leave the editor running, so the editor keeps this gate: a
// second dialog is refused while one is open, and the editor ignores its own
// input until the answer comes (ui/file_dialog.h). No SDL here, so
// editor_tests can check it.
#pragma once

#include <atomic>

namespace ui {

class DialogGate {
public:
    // Takes the gate: true when no dialog was open (this one may show).
    bool enter() {
        bool was = false;
        return m_open.compare_exchange_strong(was, true);
    }
    // The open dialog answered (or failed to show).
    void leave() { m_open.store(false); }
    bool open() const { return m_open.load(); }

private:
    std::atomic<bool> m_open{false};
};

}  // namespace ui

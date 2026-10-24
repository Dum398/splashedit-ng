// Files rewritten outside the scene, kept in the undo history: an edit
// writes them first, then pushes a FileSwapsCommand holding each file's
// bytes before and after, so undo and redo put them back.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "editor/document.hh"

namespace editor {

// A whole file's bytes into `out`. False when it cannot be read.
bool readBytes(const std::filesystem::path& file, std::string* out);
// Writes `text` over `file` through a hidden temporary beside it.
bool writeBytes(const std::filesystem::path& file, const std::string& text);

// Files rewritten together, already written when it is executed: undo puts
// each one's bytes back (removing one that was not there), redo the new ones.
class FileSwapsCommand : public Command {
  public:
    struct File {
        std::filesystem::path path;
        std::optional<std::string> before, after;  // nullopt: no such file
    };
    std::vector<File> files;
    std::vector<std::string> recheck;  // project paths the document looks up again

    void apply(Document& doc) override;
    void revert(Document& doc) override;

  private:
    bool m_first = true;
};

}  // namespace editor

#pragma once

#include <functional>
#include <string>

namespace hana::platform {

struct FileReadResult {
    bool ok = false;
    std::string contents;
    std::string error;
};

// File access is asynchronous by contract so the same interface works on the
// web (HTTP fetch) and on native platforms (which may complete immediately,
// before read_text_file returns).
class IFileSystem {
public:
    using ReadCallback = std::function<void(FileReadResult)>;

    virtual ~IFileSystem() = default;
    virtual void read_text_file(const std::string& path, ReadCallback on_done) = 0;
};

} // namespace hana::platform

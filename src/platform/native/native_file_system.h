#pragma once

#include "platform/file_system.h"

namespace hana::platform {

// Synchronous std::ifstream based implementation for desktop platforms.
// The callback is always invoked before read_text_file returns.
class NativeFileSystem final : public IFileSystem {
public:
    void read_text_file(const std::string& path, ReadCallback on_done) override;
};

} // namespace hana::platform

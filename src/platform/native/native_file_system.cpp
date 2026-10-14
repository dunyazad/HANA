#include "platform/native/native_file_system.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace hana::platform {

void NativeFileSystem::read_text_file(const std::string& path, ReadCallback on_done)
{
    FileReadResult result;
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        result.error = "cannot open file '" + path + "'";
        on_done(std::move(result));
        return;
    }
    result.contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (stream.bad()) {
        result.contents.clear();
        result.error = "failed to read file '" + path + "'";
        on_done(std::move(result));
        return;
    }
    result.ok = true;
    on_done(std::move(result));
}

} // namespace hana::platform

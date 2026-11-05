#include "errors.hpp"
#include "logger.hpp"

#include <exception>

namespace database {

void HandleTopLevelException(std::source_location loc) noexcept {
    const std::string_view file = extract_filename(loc.file_name());
    try {
        if (auto eptr = std::current_exception()) {
            std::rethrow_exception(eptr);
        } else {
            LOG_FATAL("Top-level handler invoked with no active exception [{}:{}]",
                      file, loc.line());
        }
    } catch (const DatabaseException& e) {
        LOG_FATAL("Fatal DatabaseException caught: {} [code: {}] [{}:{}]",
                  e.what(), ToString(e.Code()), file, loc.line());
    } catch (const std::exception& e) {
        LOG_FATAL("Fatal std::exception caught: {} [{}:{}]",
                  e.what(), file, loc.line());
    } catch (...) {
        LOG_FATAL("Unknown fatal exception caught [{}:{}]", file, loc.line());
    }
}

} // namespace database

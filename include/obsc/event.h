/**
 * @file event.h
 * @author Kyle Pelham (bonezone2001@gmail.com)
 * @brief The event implementation.
 * 
 * @copyright Copyright (c) 2024
*/

#pragma once
#include <string>
#include <optional>
#include <windows.h>

#include "obsc/obsc_export.hpp"

namespace obsc {

class OBSC_EXPORT Event {
private:
    explicit Event(HANDLE handle) : handle(handle) {}
    HANDLE handle;

public:
    Event() = default;
    ~Event();

    static Event create(const std::optional<std::string>& name = std::nullopt);
    static Event open(const std::string& name);
    
    void createInline(const std::optional<std::string>& name = std::nullopt);
    void openInline(const std::string& name);

    bool signal() const;
    bool reset() const;
    bool signalled() const;
    void wait() const;
    bool wait(DWORD timeoutMs) const;

    HANDLE getHandle() const { return handle; }
};

} // namespace obsc
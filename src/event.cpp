#include "obsc/debug.h"
#include "obsc/hook_info.h"
#include "obsc/event.h"
#include "obsc/constants.h"

namespace obsc {

Event::~Event()
{
    if (handle != NULL)
        CloseHandle(handle);
}

void Event::createInline(const std::optional<std::string>& name)
{
    LPCSTR eventName = name ? name->c_str() : nullptr;
    handle = CreateEventA(nullptr, FALSE, FALSE, eventName);
    
    if (handle == NULL)
        throw std::runtime_error(fmt::format("Failed to create the event {} = 0x{:x}", eventName ? eventName : "unnamed", GetLastError()));

    PRINTLN("Created the event {} = 0x{:x}", eventName ? eventName : "unnamed", reinterpret_cast<uintptr_t>(handle));
}

Event Event::create(const std::optional<std::string>& name)
{
    Event event;
    event.createInline(name);
    return event;
}

void Event::openInline(const std::string& name)
{
    handle = OpenEventA(EVENT_FLAGS, FALSE, name.c_str());
    
    if (handle == NULL)
        throw std::runtime_error(fmt::format("Failed to open the event {} = 0x{:x}", name, GetLastError()));

    PRINTLN("Opened the event {} = 0x{:x}", name, reinterpret_cast<uintptr_t>(handle));
}

Event Event::open(const std::string& name)
{
    Event event;
    event.openInline(name);
    return event;
}

bool Event::signal() const
{
    return SetEvent(handle) != 0;
}

bool Event::reset() const
{
    return ResetEvent(handle) != 0;
}

bool Event::signalled() const
{
    return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

void Event::wait() const
{
    WaitForSingleObject(handle, INFINITE);
}

bool Event::wait(DWORD timeoutMs) const
{
    return WaitForSingleObject(handle, timeoutMs) == WAIT_OBJECT_0;
}

}  // namespace obsc
#include <filesystem>

#include "obsc/debug.h"
#include "obsc/utils.h"
#include "obsc/constants.h"

namespace obsc {

std::string OBSC_EXPORT moduleDir()
{
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::filesystem::path p(buf);
    return p.has_parent_path() ? p.parent_path().string() : std::string(".");
}

std::string OBSC_EXPORT resolveBesideExe(const std::string& name)
{
    return (std::filesystem::path(moduleDir()) / name).string();
}

std::string OBSC_EXPORT runProcessAndCaptureOutput(const std::string& command, DWORD* exitCode)
{
    // Create a pipe for the child process's STDOUT.
    HANDLE hStdOutRead, hStdOutWrite;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;  // Allow the pipe handles to be inherited by the child process.
    sa.lpSecurityDescriptor = NULL;

    // Create the pipe for the process output.
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0))
        throw std::runtime_error("Failed to create pipe");

    // Ensure the read handle to the pipe is not inherited.
    if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0)) 
        throw std::runtime_error("Failed to set handle information");

    // Set up the process startup info.
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.hStdError = hStdOutWrite;
    si.hStdOutput = hStdOutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // Create the child process.
    // Convert command to a writable form (LPSTR).
    std::string cmd = command; // Ensure the command is a mutable string
    if (!CreateProcess(NULL,   // No module name (use command line).
                       &cmd[0], // Command line (mutable char array).
                       NULL,    // Process handle not inheritable.
                       NULL,    // Thread handle not inheritable.
                       TRUE,    // Set handle inheritance to TRUE.
                       0,       // No creation flags.
                       NULL,    // Use parent's environment block.
                       NULL,    // Use parent's starting directory.
                       &si,     // Pointer to STARTUPINFO structure.
                       &pi)     // Pointer to PROCESS_INFORMATION structure.
        ) {
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdOutRead);
        throw std::runtime_error("Failed to create process");
    }

    // Close the write end of the pipe after creating the child process.
    CloseHandle(hStdOutWrite);

    // Read output from the child process.
    char buffer[4096];
    DWORD bytesRead;
    std::string result;
    while (ReadFile(hStdOutRead, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        result.append(buffer, bytesRead);
    }

    // Wait until child process exits.
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (exitCode) { *exitCode = 0; GetExitCodeProcess(pi.hProcess, exitCode); }

    // Close handles.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdOutRead);

    return result;
}

// instead of repeating code, refactor the function to use the runProcessAndCaptureOutput function
void injectGraphicsHook(uint32_t pid, bool antiCheatCompatible, bool is32Bit)
{
    // Resolve the helper binaries next to the EXE (not the cwd), so launching
    // the bot from any working directory still finds them.
    std::string injectorFileName = resolveBesideExe(is32Bit ? FILE_INJECTOR_32_NAME : FILE_INJECTOR_64_NAME);
    std::string hookFileName     = resolveBesideExe(is32Bit ? FILE_HOOK_32_NAME : FILE_HOOK_64_NAME);

    // Check if the injector exists
    if (!std::filesystem::exists(injectorFileName))
        throw std::runtime_error(fmt::format("The injector exe does not exist: {}", injectorFileName));

    // Check if the hook exists
    if (!std::filesystem::exists(hookFileName))
        throw std::runtime_error(fmt::format("The hook dll does not exist: {}", hookFileName));

    // Run injector. Quote both paths -- the exe can live under a path with
    // spaces (e.g. ...\World of Warcraft\...), which CreateProcess would
    // otherwise split on.
    std::string command = fmt::format(
        "\"{}\" \"{}\" {} {}",
        injectorFileName,
        hookFileName,
        std::to_string((uint8_t)antiCheatCompatible),
        std::to_string(pid)
    );

    // inject-helper reports status via exit code (0 = ok, negative = error), not stdout
    DWORD exitCode = 0;
    runProcessAndCaptureOutput(command, &exitCode);
    if (static_cast<int32_t>(exitCode) != 0)
        throw std::runtime_error(fmt::format("inject-helper failed (exit code {})", static_cast<int32_t>(exitCode)));

    PRINTLN("Injected successfully");
}

std::pair<ComPtr<ID3D11Device>, ComPtr<ID3D11DeviceContext>> createDevice()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;

    PRINTLN("Creating the D3D11 device");
    HRESULT result = D3D11CreateDevice(
        nullptr,                        // Adapter
        D3D_DRIVER_TYPE_HARDWARE,       // Driver Type
        nullptr,                        // Software
        0,                              // Flags
        nullptr,                        // Feature Levels
        0,                              // Number of Feature Levels
        D3D11_SDK_VERSION,              // SDK Version
        device.GetAddressOf(),          // Device
        nullptr,                        // Feature Level
        deviceContext.GetAddressOf()    // Device Context
    );

    if (FAILED(result)) throw std::runtime_error("Failed to create the D3D11 device");
    return std::make_pair(device, deviceContext);
}

ComPtr<ID3D11Resource> openResource(ComPtr<ID3D11Device> device, uint32_t handle) {
    ComPtr<ID3D11Resource> resource;

    PRINTLN("Opening the shared resource");
    HRESULT result = device->OpenSharedResource(
        reinterpret_cast<HANDLE>(handle),                  // Handle to the shared resource
        __uuidof(ID3D11Resource),                          // UUID of the interface (ID3D11Resource)
        reinterpret_cast<void**>(resource.GetAddressOf())  // Resource pointer
    );

    if (FAILED(result)) throw std::runtime_error("Failed to open the shared resource");
    return resource;
}

BOOL IsWow64(HANDLE process)
{
    BOOL bIsWow64 = FALSE;

    typedef BOOL(WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);
    LPFN_ISWOW64PROCESS fnIsWow64Process;
    fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(GetModuleHandle(TEXT("kernel32")), "IsWow64Process");

    if (fnIsWow64Process != NULL
        && !fnIsWow64Process(process, &bIsWow64)) {
        fmt::print("Failed to determine if process is 64-bit: {}\n", GetLastError());
        return FALSE;
    }
    return bIsWow64;
}

bool OBSC_EXPORT is32BitProcess(uint32_t pid)
{
    // Open the process
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process == NULL) {
        fmt::print("Failed to open process: {}. PID: {}\n", GetLastError(), pid);
        return false;
    }

    SYSTEM_INFO systemInfo = { 0 };
    GetNativeSystemInfo(&systemInfo);

    // x86 environment
    if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
        return true;

    // Check if the process is an x86 process that is running on x64 environment.
    // IsWow64 returns true if the process is an x86 process
    return IsWow64(process);
}

}  // namespace obsc
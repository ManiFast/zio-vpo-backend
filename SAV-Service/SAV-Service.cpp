#define NOMINMAX
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <rpc.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdlib>

extern "C"
{
//#include "SAVRpc.h"
#include "Generated/SAVRpc.h"
}

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "rpcrt4.lib")

const wchar_t SERVICE_NAME[] = L"SAVService";
const wchar_t SERVICE_DISPLAY_NAME[] = L"SAV Antivirus Service";
const wchar_t GUI_EXE_NAME[] = L"SAV-Antivirus.exe";
const wchar_t RPC_ENDPOINT[] = L"SAVServiceRpc";

SERVICE_STATUS_HANDLE g_StatusHandle = nullptr;
SERVICE_STATUS g_Status{};
HANDLE g_StopEvent = nullptr;
std::mutex g_ProcessMutex;

struct GuiProcess
{
    DWORD sessionId;
    PROCESS_INFORMATION pi;
};

std::vector<GuiProcess> g_GuiProcesses;

extern "C" _Ret_maybenull_ void* __RPC_USER midl_user_allocate(_In_ size_t size)
{
    return malloc(size);
}

extern "C" void __RPC_USER midl_user_free(_In_opt_ void* p)
{
    free(p);
}

extern "C" void SavStopService(void)
{
    if (g_StopEvent)
        SetEvent(g_StopEvent);
}

std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);

    std::wstring path = buf;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        path.erase(pos);

    return path;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

std::wstring GetDirName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L".";
    return path.substr(0, pos);
}

std::wstring FindGuiPath()
{
    std::wstring exeDir = GetExeDir();

    std::wstring tries[] =
    {
        JoinPath(exeDir, GUI_EXE_NAME),
        JoinPath(exeDir, std::wstring(L"..\\") + GUI_EXE_NAME),
        JoinPath(exeDir, std::wstring(L"..\\..\\..\\SAV-Antivirus\\x64\\Debug\\") + GUI_EXE_NAME),
        JoinPath(exeDir, std::wstring(L"..\\..\\..\\SAV-Antivirus\\x64\\Release\\") + GUI_EXE_NAME),
        JoinPath(exeDir, std::wstring(L"..\\..\\..\\SAV-Antivirus\\Debug\\") + GUI_EXE_NAME),
        JoinPath(exeDir, std::wstring(L"..\\..\\..\\SAV-Antivirus\\Release\\") + GUI_EXE_NAME)
    };

    for (const auto& p : tries)
    {
        if (FileExists(p))
            return p;
    }

    return tries[0];
}

void SetStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    static DWORD checkpoint = 1;

    g_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState = state;
    g_Status.dwWin32ExitCode = win32ExitCode;
    g_Status.dwWaitHint = waitHint;

    if (state == SERVICE_RUNNING)
        g_Status.dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
    else
        g_Status.dwControlsAccepted = 0;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        g_Status.dwCheckPoint = checkpoint++;
    else
        g_Status.dwCheckPoint = 0;

    if (g_StatusHandle)
        SetServiceStatus(g_StatusHandle, &g_Status);
}

void CleanupFinishedProcesses()
{
    std::lock_guard<std::mutex> lock(g_ProcessMutex);

    for (auto it = g_GuiProcesses.begin(); it != g_GuiProcesses.end(); )
    {
        DWORD code = STILL_ACTIVE;
        if (!GetExitCodeProcess(it->pi.hProcess, &code) || code != STILL_ACTIVE)
        {
            CloseHandle(it->pi.hThread);
            CloseHandle(it->pi.hProcess);
            it = g_GuiProcesses.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool HasGuiForSession(DWORD sessionId)
{
    CleanupFinishedProcesses();

    std::lock_guard<std::mutex> lock(g_ProcessMutex);
    for (const auto& item : g_GuiProcesses)
    {
        if (item.sessionId == sessionId)
            return true;
    }
    return false;
}

bool LaunchGuiForSession(DWORD sessionId)
{
    if (sessionId == 0)
        return false;

    if (HasGuiForSession(sessionId))
        return true;

    HANDLE userToken = nullptr;
    HANDLE primaryToken = nullptr;
    LPVOID env = nullptr;
    bool ok = false;

    if (!WTSQueryUserToken(sessionId, &userToken))
        return false;

    if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &primaryToken))
    {
        CloseHandle(userToken);
        return false;
    }

    CreateEnvironmentBlock(&env, primaryToken, FALSE);

    std::wstring guiPath = FindGuiPath();
    std::wstring workDir = GetDirName(guiPath);
    std::wstring cmdLine = L"\"" + guiPath + L"\" --tray --from-service";

    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;

    ok = CreateProcessAsUserW(
        primaryToken,
        guiPath.c_str(),
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        flags,
        env,
        workDir.c_str(),
        &si,
        &pi
    ) == TRUE;

    if (ok)
    {
        std::lock_guard<std::mutex> lock(g_ProcessMutex);
        g_GuiProcesses.push_back({ sessionId, pi });
    }

    if (env)
        DestroyEnvironmentBlock(env);

    CloseHandle(primaryToken);
    CloseHandle(userToken);

    return ok;
}

void LaunchGuiForExistingSessions()
{
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
        return;

    for (DWORD i = 0; i < count; ++i)
    {
        if (sessions[i].SessionId != 0)
            LaunchGuiForSession(sessions[i].SessionId);
    }

    WTSFreeMemory(sessions);
}

void TerminateGuiProcesses()
{
    std::lock_guard<std::mutex> lock(g_ProcessMutex);

    for (auto& item : g_GuiProcesses)
    {
        TerminateProcess(item.pi.hProcess, 0);
        WaitForSingleObject(item.pi.hProcess, 3000);
        CloseHandle(item.pi.hThread);
        CloseHandle(item.pi.hProcess);
    }

    g_GuiProcesses.clear();
}

bool StartRpcServer()
{
    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>((wchar_t*)L"ncalrpc"),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>((wchar_t*)RPC_ENDPOINT),
        nullptr
    );

    if (status != RPC_S_OK && status != RPC_S_DUPLICATE_ENDPOINT)
        return false;

    status = RpcServerRegisterIf2(
        SAVRpc_v1_0_s_ifspec,
        nullptr,
        nullptr,
        RPC_IF_ALLOW_LOCAL_ONLY,
        RPC_C_LISTEN_MAX_CALLS_DEFAULT,
        (unsigned)-1,
        nullptr
    );

    if (status != RPC_S_OK)
        return false;

    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    return status == RPC_S_OK;
}

void StopRpcServer()
{
    RpcMgmtStopServerListening(nullptr);
    RpcServerUnregisterIf(nullptr, nullptr, FALSE);
}

DWORD WINAPI ServiceHandlerEx(DWORD control, DWORD eventType, LPVOID eventData, LPVOID)
{
    switch (control)
    {
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventType == WTS_SESSION_LOGON || eventType == WTS_SESSION_UNLOCK)
        {
            auto* note = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (note)
                LaunchGuiForSession(note->dwSessionId);
        }
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        return ERROR_CALL_NOT_IMPLEMENTED;

    default:
        return NO_ERROR;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceHandlerEx, nullptr);
    if (!g_StatusHandle)
        return;

    SetStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_StopEvent)
    {
        SetStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    if (!StartRpcServer())
    {
        DWORD err = GetLastError();
        CloseHandle(g_StopEvent);
        g_StopEvent = nullptr;
        SetStatus(SERVICE_STOPPED, err);
        return;
    }

    SetStatus(SERVICE_RUNNING);
    LaunchGuiForExistingSessions();

    WaitForSingleObject(g_StopEvent, INFINITE);

    SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    StopRpcServer();
    TerminateGuiProcesses();

    CloseHandle(g_StopEvent);
    g_StopEvent = nullptr;

    SetStatus(SERVICE_STOPPED);
}

bool InstallService()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
        return false;

    SC_HANDLE svc = CreateServiceW(
        scm,
        SERVICE_NAME,
        SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS)
        svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);

    bool ok = svc != nullptr;

    if (svc)
        CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    return ok;
}

bool RemoveService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc)
    {
        CloseServiceHandle(scm);
        return false;
    }

    bool ok = DeleteService(svc) == TRUE;

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    return ok;
}

bool StartInstalledService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc)
    {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes = 0;
    QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes);

    bool ok = true;
    if (ssp.dwCurrentState != SERVICE_RUNNING)
        ok = StartServiceW(svc, 0, nullptr) == TRUE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 1)
    {
        std::wstring cmd = argv[1];

        if (cmd == L"install")
        {
            wprintf(L"Install service: %s\n", InstallService() ? L"OK" : L"FAILED");
            return 0;
        }

        if (cmd == L"remove" || cmd == L"uninstall")
        {
            wprintf(L"Remove service: %s\n", RemoveService() ? L"OK" : L"FAILED");
            return 0;
        }

        if (cmd == L"start")
        {
            wprintf(L"Start service: %s\n", StartInstalledService() ? L"OK" : L"FAILED");
            return 0;
        }
    }

    SERVICE_TABLE_ENTRYW serviceTable[] =
    {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
    {
        wprintf(L"Run as service or use: install / start / remove\n");
        return 1;
    }

    return 0;
}

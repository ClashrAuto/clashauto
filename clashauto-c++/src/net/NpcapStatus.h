#pragma once

// Npcap 运行时状态探测（Windows 专用，纯 Win32、header-only）——同一套口径供两处使用：
//   · src/net/L2Endpoint_win.cpp：pcap_open_live 失败时区分「没装」和「装了但没权限」；
//   · src/qml/NpcapInstaller.cpp：设备页提示条 / 安装窗的状态与「修复权限」按钮。
//
// 存在的理由（也正是「设备页总提示打开网卡失败」的根因）：Npcap 安装向导里那个
// **「Restrict Npcap driver's access to Administrators only」默认是勾上的**。勾上后驱动会给
// \Device\NPF_* 装一个只允许 Administrators 的安全描述符——Coast 以普通用户身份运行时
// pcap_open_live 必然返回 "Error opening adapter: 拒绝访问。(5)"，跟装没装 Npcap 无关。
// 该开关落在注册表 HKLM\SYSTEM\CurrentControlSet\Services\npcap\Parameters\AdminOnly，
// 置 0 并重启 npcap 驱动服务即可解除（两步都要管理员，见 NpcapInstaller::fixPermissions）。
#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#  include <string>

namespace npcapstatus {

// 已装 = %SystemRoot%\System32\Npcap\ 下有用户态库——这正是 L2Endpoint_win 会去加载的目录，
// 判定口径与实际加载口径一致。故意不认 System32\wpcap.dll：那可能是早已停更的 WinPcap。
inline bool installed()
{
    wchar_t sys[MAX_PATH] = {0};
    if (!GetSystemDirectoryW(sys, MAX_PATH))
        return false;
    const std::wstring base = std::wstring(sys) + L"\\Npcap\\";
    return GetFileAttributesW((base + L"wpcap.dll").c_str()) != INVALID_FILE_ATTRIBUTES
            || GetFileAttributesW((base + L"Packet.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 驱动是否被限制为「仅管理员可访问」。键读不到（老版本 / 异常安装）按「未限制」处理：
// 真限制了的话 open 照样会失败，只是少一条更精确的解释，不会造成误报。
inline bool adminOnly()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\npcap\\Parameters", 0,
                      KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return false;
    }
    DWORD v = 0;
    DWORD type = 0;
    DWORD sz = sizeof(v);
    const bool ok = RegQueryValueExW(key, L"AdminOnly", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&v), &sz)
                        == ERROR_SUCCESS
                    && type == REG_DWORD;
    RegCloseKey(key);
    return ok && v != 0;
}

// 本进程是否已提权。提权运行时 AdminOnly 限制对我们无效，网关照常能开。
inline bool processElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION el{};
    DWORD sz = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &sz) != FALSE;
    CloseHandle(token);
    return ok && el.TokenIsElevated != 0;
}

// 装了、被限制、且本进程非提权 —— 二层端点必然打不开，但**一键可修**（fixPermissions）。
inline bool restricted() { return installed() && adminOnly() && !processElevated(); }

} // namespace npcapstatus

#endif // _WIN32

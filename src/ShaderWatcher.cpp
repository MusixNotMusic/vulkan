#include "vulkan/ShaderWatcher.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace vkb {

ShaderWatcher::ShaderWatcher() = default;

ShaderWatcher::~ShaderWatcher() {
    stop();
}

void ShaderWatcher::start(const std::string& dir, const std::vector<std::string>& extensions) {
    stop(); // 先停之前的

    m_dir = dir;
    m_extensions = extensions;
    m_dirty = false;
    m_running = true;

    m_eventHandle = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    // ReadDirectoryChangesW 需要 ANSI 或宽字符路径
    std::string dirA = dir;
    m_dirHandle = CreateFileA(
        dirA.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (m_dirHandle == INVALID_HANDLE_VALUE) {
        std::cerr << "[ShaderWatcher] Failed to open directory: " << dir << std::endl;
        m_running = false;
        return;
    }

    m_thread = std::thread(&ShaderWatcher::watchThread, this);
    std::cout << "[ShaderWatcher] Monitoring " << dir << std::endl;
}

void ShaderWatcher::stop() {
    if (!m_running.load()) return;

    m_running = false;
    if (m_eventHandle) {
        SetEvent(m_eventHandle); // 唤醒线程
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_dirHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_dirHandle);
        m_dirHandle = INVALID_HANDLE_VALUE;
    }
    if (m_eventHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = nullptr;
    }
}

bool ShaderWatcher::pollDirty() {
    bool val = m_dirty.exchange(false);
    return val;
}

bool ShaderWatcher::matchExtension(const std::string& filename) const {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto& ext : m_extensions) {
        if (lower.size() >= ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

void ShaderWatcher::watchThread() {
    const DWORD bufferSize = 8192;
    std::vector<char> buffer(bufferSize);

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    while (m_running.load()) {
        DWORD bytesReturned = 0;

        BOOL ok = ReadDirectoryChangesW(
            m_dirHandle,
            buffer.data(),
            bufferSize,
            FALSE,                    // 不递归
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned,
            &overlapped,
            nullptr
        );

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            // 目录可能被删除了，休息一下再重试
            Sleep(500);
            continue;
        }

        // 同时等待 I/O 完成信号和停止事件
        HANDLE handles[2] = { overlapped.hEvent, m_eventHandle };
        DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 + 1) {
            // 停止事件触发
            CancelIo(m_dirHandle);
            break;
        }

        // I/O 完成
        GetOverlappedResult(m_dirHandle, &overlapped, &bytesReturned, TRUE);

        if (bytesReturned == 0) continue;

        // 遍历变更记录
        DWORD offset = 0;
        while (offset < bytesReturned) {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(&buffer[offset]);

            // 宽字符文件名
            int len = WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                info->FileNameLength / sizeof(WCHAR), nullptr, 0, nullptr, nullptr);
            std::string filename(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                info->FileNameLength / sizeof(WCHAR), filename.data(), len, nullptr, nullptr);

            if (matchExtension(filename)) {
                DWORD now = GetTickCount();
                if (now - m_lastTriggerTick > DEBOUNCE_MS) {
                    m_dirty = true;
                    m_lastTriggerTick = now;
                    std::cout << "[ShaderWatcher] Detected change: " << filename << std::endl;
                }
            }

            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }

        // 重置 OVERLAPPED
        ResetEvent(overlapped.hEvent);
    }

    CloseHandle(overlapped.hEvent);
}

} // namespace vkb

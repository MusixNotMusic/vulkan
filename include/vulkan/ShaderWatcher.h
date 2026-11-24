#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace vkb {

// 后台线程监控目录变化，有变更时置位 dirty 标志
class ShaderWatcher {
public:
    ShaderWatcher();
    ~ShaderWatcher();

    // 开始监控指定目录（非递归，仅顶层文件）
    // extensions: 如 { ".vert", ".frag", ".comp" }
    void start(const std::string& dir, const std::vector<std::string>& extensions);
    void stop();

    // 主线程查询：是否有 shader 变更
    bool pollDirty();

private:
    void watchThread();

    std::string m_dir;
    std::vector<std::string> m_extensions;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_dirty{ false };
    std::thread m_thread;
    HANDLE m_dirHandle = INVALID_HANDLE_VALUE;
    HANDLE m_eventHandle = nullptr;
    std::mutex m_mutex;

    // 防抖：上次触发时间，防止保存时多次写入重复触发
    DWORD m_lastTriggerTick = 0;
    static constexpr DWORD DEBOUNCE_MS = 300;

    bool matchExtension(const std::string& filename) const;
};

} // namespace vkb

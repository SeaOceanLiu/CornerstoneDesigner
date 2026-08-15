// App.h
// CornerstoneDesigner
//
// 应用入口：Embedded 模式自持主循环，加载设计器自身 JSON 骨架并桥接菜单动作。

#pragma once

#include <memory>
#include <string>

namespace UICornerstone {
class UICornerstone;
}

namespace Designer {

class App {
public:
    App() = default;
    ~App();                          // 析构在 App.cpp 定义（unique_ptr<UICornerstone> 需完整类型）
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    bool init();
    int  run(int autoSeconds = 0);   // autoSeconds>0 时无人值守渲染指定秒数后干净退出
    void shutdown();

private:
    void registerActions();
    bool loadSkeleton();

    std::unique_ptr<UICornerstone::UICornerstone> m_ui;  // 主实例（设计器自身 UI；预览为其内子视口）
    bool m_exitRequested = false;
    std::string m_lastError;
};

}  // namespace Designer

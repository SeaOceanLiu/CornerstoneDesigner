// ControlModel.h
// CornerstoneDesigner
//
// 控件内存模型：被设计布局的控件树（画布内容），供画布渲染/拖拽/放置/选中。

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Designer {

struct SRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool operator==(const SRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

struct ControlProperties {
    std::string type = "panel";     // kebab-case：button/label/panel/…
    std::string id;
    SRect rect;
    bool visible = true;
    nlohmann::json extended;        // 其它属性原样保留（caption/colors/font/…）
};

class ControlModel {
public:
    std::string id;
    ControlProperties props;
    std::vector<std::unique_ptr<ControlModel>> children;
    ControlModel* parent = nullptr;

    nlohmann::json toJson() const;
    static std::unique_ptr<ControlModel> fromJson(const nlohmann::json& j);
};

}  // namespace Designer

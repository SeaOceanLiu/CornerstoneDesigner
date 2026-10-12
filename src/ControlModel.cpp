// ControlModel.cpp
// CornerstoneDesigner

#include "Designer/ControlModel.h"

#include <nlohmann/json.hpp>

namespace Designer {

nlohmann::json ControlModel::toJson() const {
    nlohmann::json j;
    j["type"] = props.type;
    if (!props.id.empty()) j["id"] = props.id;
    j["rect"] = { { "x", props.rect.x }, { "y", props.rect.y },
                  { "w", props.rect.w }, { "h", props.rect.h } };
    if (!props.visible) j["visible"] = false;
    if (props.extended.is_object())
        for (auto& [k, v] : props.extended.items()) j[k] = v;
    if (!children.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ch : children) arr.push_back(ch->toJson());
        j["children"] = arr;
    }
    return j;
}

std::unique_ptr<ControlModel> ControlModel::fromJson(const nlohmann::json& j) {
    auto node = std::make_unique<ControlModel>();
    if (j.contains("id") && j["id"].is_string()) {
        node->id = j["id"].get<std::string>();
        node->props.id = node->id;   // 全项目按 props.id 访问（同步，防空 id）
    }
    if (j.contains("type") && j["type"].is_string()) node->props.type = j["type"].get<std::string>();
    if (j.contains("rect") && j["rect"].is_object()) {
        node->props.rect.x = j["rect"].value("x", 0.0f);
        node->props.rect.y = j["rect"].value("y", 0.0f);
        node->props.rect.w = j["rect"].value("w", 0.0f);
        node->props.rect.h = j["rect"].value("h", 0.0f);
    }
    if (j.contains("visible")) node->props.visible = j["visible"].get<bool>();
    for (auto& [k, v] : j.items()) {
        if (k == "type" || k == "id" || k == "rect" || k == "visible" || k == "children")
            continue;
        node->props.extended[k] = v;
    }
    // extended 恒为对象（无额外键时默认 null——读取 .value() 会抛异常，加载路径不安全）
    if (!node->props.extended.is_object()) node->props.extended = nlohmann::json::object();
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto& cj : j["children"]) {
            auto child = fromJson(cj);
            if (child) { child->parent = node.get(); node->children.push_back(std::move(child)); }
        }
    }
    return node;
}

}  // namespace Designer

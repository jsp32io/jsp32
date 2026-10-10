#include "app.hpp"
#include "font.hpp"
#include "ui_layout.hpp"
#include "api.hpp"
#include "plugin_zip.hpp"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <sstream>
#include <algorithm>

namespace jsp32 {
namespace {
std::string compact(const Json::Value &value)
{
    Json::StreamWriterBuilder writer; writer["indentation"] = "";
    return Json::writeString(writer, value);
}
std::string display_value(const Json::Value &value)
{
    if (value.isString()) return value.asString();
    if (value.isBool()) return value.asBool() ? "是" : "否";
    return compact(value);
}
Json::Value parse_object(const std::string &text)
{
    std::istringstream input(text); Json::Value result;
    Json::CharReaderBuilder reader; std::string error;
    if (!Json::parseFromStream(reader, input, &result, &error) || !result.isObject())
        throw std::runtime_error("插件配置文件无效");
    return result;
}
const Json::Value *runtime(const App &app, const std::string &id)
{
    for (const auto &item : app.plugin_runtime) if (item.get("id", "").asString() == id) return &item;
    return nullptr;
}
void open_configuration(App &app, const std::string &id)
{
    auto generation = app.plugin_editor.generation + 1;
    app.plugin_editor = PluginConfigEditor{};
    auto &editor = app.plugin_editor;
    editor.open = true; editor.loading = true; editor.id = id;
    editor.device = app.selected; editor.url = app.current_url(); editor.generation = generation;
    auto url = editor.url, device = editor.device;
    app.run([&app, url, device, id, generation] {
        try {
            Api api(url); Json::Value config(Json::objectValue), schema;
            try { config = parse_object(api.file("/plugins/" + id + "/config.json").body); }
            catch (const HttpError &error) { if (error.status != 404) throw; }
            try {
                auto detail = api.eval("(await import('plugins')).plugins.getConfig(" + compact(Json::Value(id)) + ")");
                schema = detail["schema"];
            } catch (const std::exception &) {}
            bool enabled = true, pending = false;
            try {
                auto meta = parse_object(api.file("/plugins/meta.json").body);
                enabled = !meta[id].get("disabled", false).asBool();
            } catch (const HttpError &error) { if (error.status != 404) throw; }
            try {
                auto changes = api.eval("(await import('plugins')).plugins.getPendingChanges(" + compact(Json::Value(id)) + ")");
                pending = changes.isObject() && !changes.empty();
            } catch (const std::exception &) {}
            PluginConfigForm form; form.load(schema, config);
            return std::function<void()>([&app, device, generation, schema, form = std::move(form), enabled, pending]() mutable {
                auto &editor = app.plugin_editor;
                if (!editor.open || app.selected != device || editor.generation != generation) return;
                editor.loading = false; editor.schema_available = schema.isObject();
                editor.form = std::move(form); editor.enabled = enabled; editor.pending = pending;
            });
        } catch (const std::exception &e) {
            return std::function<void()>([&app, device, generation, message = std::string(e.what())] {
                auto &editor = app.plugin_editor;
                if (!editor.open || app.selected != device || editor.generation != generation) return;
                editor.loading = false; editor.load_failed = true; editor.error = message;
            });
        }
    }, "正在读取插件配置");
}
void draw_field(PluginConfigField &field)
{
    ImGui::PushID(field.key.c_str());
    auto label = field.rule.get("name", field.key).asString();
    ImGui::TextUnformatted(label.c_str());
    if (field.rule.isMember("default")) {
        ImGui::SameLine();
        if (ImGui::Checkbox("使用默认值", &field.use_default) && field.use_default)
            field.set_value(field.rule["default"]);
    } else { ImGui::SameLine(); ImGui::TextDisabled("必填"); }
    ImGui::BeginDisabled(field.use_default);
    ImGui::SetNextItemWidth(-1);
    if (field.rule["choices"].isArray()) {
        auto preview = field.value.isNull() ? std::string("请选择") : display_value(field.value);
        if (ImGui::BeginCombo("##value", preview.c_str())) {
            int index = 0;
            for (const auto &choice : field.rule["choices"]) {
                ImGui::PushID(index++);
                if (ImGui::Selectable(display_value(choice).c_str(), choice == field.value)) field.set_value(choice);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    } else if (field.rule["type"] == "boolean") {
        if (field.value.isBool()) {
            bool value = field.value.asBool();
            if (ImGui::Checkbox("启用", &value)) field.value = value;
        } else if (ImGui::BeginCombo("##value", "请选择")) {
            if (ImGui::Selectable("是")) field.value = true;
            if (ImGui::Selectable("否")) field.value = false;
            ImGui::EndCombo();
        }
    } else {
        const char *hint = field.rule["type"] == "integer" ? "请输入整数" :
                           field.rule["type"] == "number" ? "请输入数值" : "请输入内容";
        ImGui::InputTextWithHint("##value", hint, &field.text);
    }
    ImGui::EndDisabled();
    if (field.rule.isMember("min") || field.rule.isMember("max")) {
        std::string range;
        if (field.rule.isMember("min")) range = "最小值 " + display_value(field.rule["min"]);
        if (field.rule.isMember("max")) range += (range.empty() ? "" : " · ") + std::string("最大值 ") + display_value(field.rule["max"]);
        ImGui::TextDisabled("%s", range.c_str());
    }
    ImGui::Spacing(); ImGui::PopID();
}
void draw_configuration(App &app)
{
    auto &editor = app.plugin_editor;
    if (editor.open && editor.device != app.selected) editor.open = false;
    if (editor.open) ImGui::OpenPopup("插件配置");
    auto viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize({std::min(480.0f, viewport->WorkSize.x - 32), std::min(560.0f, viewport->WorkSize.y - 32)}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("插件配置", &editor.open, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextUnformatted(editor.id.c_str());
    ImGui::TextDisabled("保存后重启设备生效");
    const float message_height = !editor.error.empty() ? ImGui::CalcTextSize(editor.error.c_str(), nullptr, false,
        ImGui::GetContentRegionAvail().x).y : ImGui::GetTextLineHeight();
    const float footer_height = message_height + ImGui::GetFrameHeight() + 4 * ImGui::GetStyle().ItemSpacing.y + 4;
    if (ImGui::BeginChild("##fields", {0, -footer_height})) {
        if (editor.loading) ImGui::TextDisabled("正在读取配置...");
        else {
            ImGui::BeginDisabled(app.busy() || editor.load_failed);
            ImGui::Checkbox("启用插件", &editor.enabled);
            ImGui::Separator(); ImGui::Spacing();
            if (!editor.schema_available) {
                ImGui::TextWrapped("暂无可用配置定义。尚未加载或已禁用的插件，请先启用并重启设备，再打开配置。");
            } else if (editor.form.fields.empty()) ImGui::TextDisabled("此插件没有配置项");
            else for (auto &field : editor.form.fields) draw_field(field);
            ImGui::EndDisabled();
        }
    }
    ImGui::EndChild();
    if (!editor.error.empty()) ImGui::TextWrapped("%s", editor.error.c_str());
    else if (editor.pending) ImGui::TextDisabled("有待重启生效的更改");
    else ImGui::Spacing();
    ImGui::Separator();
    ImGui::BeginDisabled(app.busy() || editor.loading);
    if (ImGui::Button("取消", {100, 0})) { editor.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(editor.load_failed);
    if (ImGui::Button("保存", {100, 0})) {
        try {
            auto config = editor.form.configuration();
            auto url = editor.url, device = editor.device, id = editor.id;
            auto generation = editor.generation;
            bool enabled = editor.enabled, has_schema = editor.schema_available;
            editor.error.clear();
            app.run([&app, url, device, id, generation, enabled, has_schema, config] {
                try {
                    Api api(url);
                    if (has_schema) set_plugin_config(api, id, config);
                    set_plugin_enabled(api, id, enabled);
                    return std::function<void()>([&app, device, generation] {
                        if (app.selected != device || app.plugin_editor.generation != generation) return;
                        app.plugin_editor.pending = true;
                        app.success("插件设置已保存，重启设备后生效");
                    });
                } catch (const std::exception &e) {
                    return std::function<void()>([&app, device, generation, message = std::string(e.what())] {
                        if (app.selected == device && app.plugin_editor.generation == generation)
                            app.plugin_editor.error = "保存未完成：" + message;
                    });
                }
            }, "正在保存插件设置");
        } catch (const std::exception &e) { editor.error = e.what(); }
    }
    ImGui::EndDisabled();
    if (editor.pending) {
        ImGui::SameLine();
        if (ImGui::Button("重启设备")) {
            auto url = editor.url;
            app.run([&app, url] {
                Api(url).reboot();
                return std::function<void()>([&app] { app.success("设备正在重启，请稍后刷新"); });
            }, "正在重启设备");
            editor.open = false; ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}
}
void draw_plugins(App &app)
{
    ImGui::PushFont(ui_heading_font(), 20.0f);
    ImGui::Text("插件"); ImGui::PopFont();
    align_actions_right(button_width("上传 ZIP") + ImGui::GetStyle().ItemSpacing.x + button_width("刷新"));
    if (ImGui::Button("上传 ZIP") && !app.busy()) app.open_file(false, "");
    ImGui::SameLine();
    ImGui::BeginDisabled(app.busy());
    if (ImGui::Button("刷新")) app.request_plugin_refresh();
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::BeginTable("plugins", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("插件", 0, 2); ImGui::TableSetupColumn("状态");
        ImGui::TableSetupColumn("启用"); ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableHeadersRow();
        for (const auto &id : app.plugin_ids) {
            auto *state = runtime(app, id);
            bool disabled = state ? (*state).get("disabled", false).asBool() : false;
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(id.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", state ? (*state).get("status", "未知").asString().c_str() : "待重启 / 未运行");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(disabled ? "否" : "是");
            ImGui::TableNextColumn(); ImGui::PushID(id.c_str());
            if (ImGui::SmallButton("配置") && !app.busy()) open_configuration(app, id);
            ImGui::SameLine();
            if (ImGui::SmallButton("下载") && !app.busy()) { app.plugin_selected = id; app.open_file(true, id + ".zip"); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    draw_configuration(app);
}
}

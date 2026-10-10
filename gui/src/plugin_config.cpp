#include "plugin_config.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace jsp32 {
void PluginConfigField::set_value(const Json::Value &input)
{
    value = input;
    if (input.isString()) text = input.asString();
    else if (input.isNumeric()) { Json::StreamWriterBuilder writer; writer["indentation"] = ""; text = Json::writeString(writer, input); }
    else text.clear();
}
void PluginConfigForm::load(const Json::Value &schema, const Json::Value &overrides)
{
    fields.clear();
    if (!schema.isObject()) return;
    for (const auto &key : schema.getMemberNames()) {
        PluginConfigField field;
        field.key = key; field.rule = schema[key];
        field.use_default = !overrides.isMember(key) && field.rule.isMember("default");
        field.set_value(overrides.isMember(key) ? overrides[key] : field.rule["default"]);
        fields.push_back(std::move(field));
    }
}
Json::Value PluginConfigForm::configuration() const
{
    Json::Value config(Json::objectValue);
    for (const auto &field : fields) {
        if (field.use_default) continue;
        const auto &rule = field.rule;
        auto label = rule.get("name", field.key).asString();
        auto fail = [&](const std::string &reason) { throw std::runtime_error(label + "：" + reason); };
        auto type = rule["type"].asString();
        Json::Value value = field.value;
        if (!rule["choices"].isArray()) {
            if (type == "string") value = field.text;
            else if (type == "integer") {
                Json::Int64 number = 0;
                auto [end, error] = std::from_chars(field.text.data(), field.text.data() + field.text.size(), number);
                if (field.text.empty() || error != std::errc{} || end != field.text.data() + field.text.size()) fail("请输入整数");
                value = number;
            } else if (type == "number") {
                double number = 0;
                auto [end, error] = std::from_chars(field.text.data(), field.text.data() + field.text.size(), number);
                if (field.text.empty() || error != std::errc{} || end != field.text.data() + field.text.size() || !std::isfinite(number)) fail("请输入有效数值");
                value = number;
            }
        }
        bool valid = type == "string" ? value.isString() : type == "boolean" ? value.isBool() :
            type == "integer" ? (value.isInt64() || value.isUInt64()) : type == "number" ? value.isNumeric() : false;
        if (!valid) fail("请选择或填写此项");
        if (value.isNumeric() && ((rule.isMember("min") && value.asDouble() < rule["min"].asDouble()) ||
                                 (rule.isMember("max") && value.asDouble() > rule["max"].asDouble()))) fail("数值超出允许范围");
        if (rule["choices"].isArray()) {
            bool found = false;
            for (const auto &choice : rule["choices"]) if (choice == value) found = true;
            if (!found) fail("请选择有效选项");
        }
        config[field.key] = value;
    }
    return config;
}
}

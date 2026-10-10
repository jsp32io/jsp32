#pragma once
#include <json/json.h>
#include <cstdint>
#include <string>
#include <vector>

namespace jsp32 {
struct PluginConfigField {
    std::string key;
    Json::Value rule;
    bool use_default = false;
    std::string text;
    Json::Value value;
    void set_value(const Json::Value &value);
};
class PluginConfigForm {
public:
    void load(const Json::Value &schema, const Json::Value &overrides);
    Json::Value configuration() const;
    std::vector<PluginConfigField> fields;
};
struct PluginConfigEditor {
    bool open = false;
    bool loading = false;
    bool schema_available = false;
    bool load_failed = false;
    bool enabled = true;
    bool pending = false;
    uint64_t generation = 0;
    std::string id, device, url, error;
    PluginConfigForm form;
};
}

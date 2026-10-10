#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <json/json.h>

namespace jsp32 {

enum class ValueType { Bool, UInt16, Int16, UInt32, Int32, Float32 };
enum class ByteOrder { ABCD, BADC, CDAB, DCBA };

struct Monitor {
    std::string name;
    std::string address;
    ValueType type = ValueType::UInt16;
    ByteOrder order = ByteOrder::ABCD;
};

struct Device {
    std::string id;
    std::string name;
    std::string url;
    std::string version;
};

struct Settings {
    std::string selected_device;
    std::vector<Device> devices;
    std::map<std::string, std::vector<Monitor>> monitors;
};

struct Address {
    char area;
    unsigned index;
};

Address parse_address(const std::string &text);
std::string address_text(char area, unsigned index);
int word_count(ValueType type);
bool type_allowed(char area, ValueType type);
std::string type_name(ValueType type);
ValueType parse_type(const std::string &text);
std::string order_name(ByteOrder order);
ByteOrder parse_order(const std::string &text);
std::string format_value(ValueType type, ByteOrder order,
                         const std::vector<uint16_t> &words);
std::vector<uint16_t> parse_value(ValueType type, ByteOrder order,
                                  const std::string &text);
Settings load_settings(const std::string &path);
void save_settings(const std::string &path, const Settings &settings);

} // namespace jsp32

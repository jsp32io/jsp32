#include "model.hpp"

#include <bit>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace jsp32 {

static std::filesystem::path native_path(const std::string &utf8)
{
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

Address parse_address(const std::string &text)
{
    if (text.size() < 2 || (text[0] != 'X' && text[0] != 'Y' &&
                            text[0] != 'M' && text[0] != 'D') ||
        (text.size() > 2 && text[1] == '0')) {
        throw std::invalid_argument("无效的寄存器地址");
    }
    unsigned index = 0;
    auto [end, error] = std::from_chars(text.data() + 1,
                                        text.data() + text.size(), index);
    if (error != std::errc{} || end != text.data() + text.size() || index > 65535) {
        throw std::invalid_argument("无效的寄存器地址");
    }
    return {text[0], index};
}

std::string address_text(char area, unsigned index)
{
    return std::string(1, area) + std::to_string(index);
}

int word_count(ValueType type)
{
    return type == ValueType::UInt32 || type == ValueType::Int32 ||
                   type == ValueType::Float32 ? 2 : 1;
}

bool type_allowed(char area, ValueType type)
{
    return area == 'D' ? type != ValueType::Bool : type == ValueType::Bool;
}

std::string type_name(ValueType type)
{
    switch (type) {
    case ValueType::Bool: return "bool";
    case ValueType::UInt16: return "uint16";
    case ValueType::Int16: return "int16";
    case ValueType::UInt32: return "uint32";
    case ValueType::Int32: return "int32";
    case ValueType::Float32: return "float32";
    }
    throw std::invalid_argument("未知数据类型");
}

ValueType parse_type(const std::string &text)
{
    for (auto type : {ValueType::Bool, ValueType::UInt16, ValueType::Int16,
                      ValueType::UInt32, ValueType::Int32, ValueType::Float32}) {
        if (type_name(type) == text) return type;
    }
    throw std::invalid_argument("未知数据类型");
}

std::string order_name(ByteOrder order)
{
    switch (order) {
    case ByteOrder::ABCD: return "ABCD";
    case ByteOrder::BADC: return "BADC";
    case ByteOrder::CDAB: return "CDAB";
    case ByteOrder::DCBA: return "DCBA";
    }
    throw std::invalid_argument("未知字节序");
}

ByteOrder parse_order(const std::string &text)
{
    for (auto order : {ByteOrder::ABCD, ByteOrder::BADC,
                       ByteOrder::CDAB, ByteOrder::DCBA}) {
        if (order_name(order) == text) return order;
    }
    throw std::invalid_argument("未知字节序");
}

static uint32_t decode_32(ByteOrder order, const std::vector<uint16_t> &words)
{
    if (words.size() != 2) throw std::invalid_argument("需要两个 D 寄存器");
    uint8_t physical[4] = {static_cast<uint8_t>(words[0] >> 8),
                           static_cast<uint8_t>(words[0]),
                           static_cast<uint8_t>(words[1] >> 8),
                           static_cast<uint8_t>(words[1])};
    int sequence[4] = {0, 1, 2, 3};
    if (order == ByteOrder::BADC) { sequence[0]=1; sequence[1]=0; sequence[2]=3; sequence[3]=2; }
    if (order == ByteOrder::CDAB) { sequence[0]=2; sequence[1]=3; sequence[2]=0; sequence[3]=1; }
    if (order == ByteOrder::DCBA) { sequence[0]=3; sequence[1]=2; sequence[2]=1; sequence[3]=0; }
    uint32_t bits = 0;
    for (int i : sequence) bits = (bits << 8) | physical[i];
    return bits;
}

static std::vector<uint16_t> encode_32(ByteOrder order, uint32_t bits)
{
    uint8_t bytes[4] = {static_cast<uint8_t>(bits >> 24),
                        static_cast<uint8_t>(bits >> 16),
                        static_cast<uint8_t>(bits >> 8),
                        static_cast<uint8_t>(bits)};
    if (order == ByteOrder::BADC) { std::swap(bytes[0], bytes[1]); std::swap(bytes[2], bytes[3]); }
    if (order == ByteOrder::CDAB) { std::swap(bytes[0], bytes[2]); std::swap(bytes[1], bytes[3]); }
    if (order == ByteOrder::DCBA) { std::swap(bytes[0], bytes[3]); std::swap(bytes[1], bytes[2]); }
    return {static_cast<uint16_t>((bytes[0] << 8) | bytes[1]),
            static_cast<uint16_t>((bytes[2] << 8) | bytes[3])};
}

std::string format_value(ValueType type, ByteOrder order,
                         const std::vector<uint16_t> &words)
{
    if (words.size() != static_cast<size_t>(word_count(type))) {
        throw std::invalid_argument("寄存器数量不匹配");
    }
    if (type == ValueType::Bool) return words[0] ? "1" : "0";
    if (type == ValueType::UInt16) return std::to_string(words[0]);
    if (type == ValueType::Int16) return std::to_string(std::bit_cast<int16_t>(words[0]));
    uint32_t bits = decode_32(order, words);
    if (type == ValueType::UInt32) return std::to_string(bits);
    if (type == ValueType::Int32) return std::to_string(std::bit_cast<int32_t>(bits));
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<float>::max_digits10) << std::bit_cast<float>(bits);
    return output.str();
}

template <typename T> static T parse_integer(const std::string &text)
{
    T value{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("请输入有效整数");
    }
    return value;
}

std::vector<uint16_t> parse_value(ValueType type, ByteOrder order,
                                  const std::string &text)
{
    if (type == ValueType::Bool) {
        if (text == "1" || text == "true") return {1};
        if (text == "0" || text == "false") return {0};
        throw std::invalid_argument("布尔值只能为 0 或 1");
    }
    if (type == ValueType::UInt16) return {parse_integer<uint16_t>(text)};
    if (type == ValueType::Int16) return {std::bit_cast<uint16_t>(parse_integer<int16_t>(text))};
    if (type == ValueType::UInt32) return encode_32(order, parse_integer<uint32_t>(text));
    if (type == ValueType::Int32) return encode_32(order, std::bit_cast<uint32_t>(parse_integer<int32_t>(text)));
    float value{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("请输入有限的浮点数");
    }
    return encode_32(order, std::bit_cast<uint32_t>(value));
}

Settings load_settings(const std::string &path)
{
    Settings settings;
    std::ifstream input(native_path(path));
    if (!input) return settings;
    Json::Value root;
    Json::CharReaderBuilder parser;
    std::string error;
    if (!Json::parseFromStream(parser, input, &root, &error) || !root.isObject()) {
        throw std::runtime_error("设置文件无法读取: " + error);
    }
    settings.selected_device = root.get("selectedDevice", "").asString();
    for (const auto &item : root["devices"]) {
        if (item.isObject() && item["id"].isString() && item["url"].isString()) {
            settings.devices.push_back({item["id"].asString(),
                                        item.get("name", "JSP32").asString(),
                                        item["url"].asString(),
                                        item.get("version", "").asString()});
        }
    }
    const auto &all = root["monitors"];
    if (all.isObject()) {
        for (const auto &id : all.getMemberNames()) {
            for (const auto &item : all[id]) {
                try {
                    Monitor monitor{item["name"].asString(), item["address"].asString(),
                                    parse_type(item["type"].asString()),
                                    parse_order(item.get("order", "ABCD").asString())};
                    auto address = parse_address(monitor.address);
                    if (type_allowed(address.area, monitor.type)) settings.monitors[id].push_back(monitor);
                } catch (const std::exception &) { /* Skip one invalid saved monitor. */ }
            }
        }
    }
    return settings;
}

void save_settings(const std::string &path, const Settings &settings)
{
    Json::Value root(Json::objectValue);
    root["selectedDevice"] = settings.selected_device;
    root["devices"] = Json::Value(Json::arrayValue);
    for (const auto &device : settings.devices) {
        Json::Value item;
        item["id"] = device.id;
        item["name"] = device.name;
        item["url"] = device.url;
        if (!device.version.empty()) item["version"] = device.version;
        root["devices"].append(item);
    }
    root["monitors"] = Json::Value(Json::objectValue);
    for (const auto &[id, monitors] : settings.monitors) {
        Json::Value items(Json::arrayValue);
        for (const auto &monitor : monitors) {
            Json::Value item;
            item["name"] = monitor.name;
            item["address"] = monitor.address;
            item["type"] = type_name(monitor.type);
            item["order"] = order_name(monitor.order);
            items.append(item);
        }
        root["monitors"][id] = items;
    }
    auto target = native_path(path);
    std::filesystem::create_directories(target.parent_path());
    auto temp = target; temp += ".tmp";
    std::ofstream output(temp, std::ios::trunc);
    if (!output) throw std::runtime_error("无法保存设置文件");
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    output << Json::writeString(writer, root);
    output.close();
    if (!output) throw std::runtime_error("写入设置文件失败");
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("无法替换设置文件");
#else
    std::filesystem::rename(temp, target);
#endif
}

} // namespace jsp32

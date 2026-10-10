#include "model.hpp"
#include "plugin_zip.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>

static std::string utf8_path(const std::filesystem::path &path)
{
    auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

int main()
{
    using namespace jsp32;
    assert(normalize_url("192.0.2.2") == "http://192.0.2.2");
    assert(normalize_url("http://192.0.2.2/api/v1") == "http://192.0.2.2");
    assert(url_encode("/plugins/a b") == "%2Fplugins%2Fa%20b");
    assert(parse_address("D100").index == 100);
    for (auto invalid : {"D01", "A1", "D-1", "D65536"}) {
        bool rejected = false;
        try { parse_address(invalid); } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
    for (auto order : {ByteOrder::ABCD, ByteOrder::BADC, ByteOrder::CDAB, ByteOrder::DCBA}) {
        auto words = parse_value(ValueType::Int32, order, "-1234567");
        assert(format_value(ValueType::Int32, order, words) == "-1234567");
        assert(format_value(ValueType::Float32, order, parse_value(ValueType::Float32, order, "12.5")) == "12.5");
    }
    assert((parse_value(ValueType::UInt32, ByteOrder::ABCD, "305419896") == std::vector<uint16_t>{0x1234,0x5678}));
    Settings settings;
    settings.selected_device = "aa:bb";
    settings.devices.push_back({"aa:bb", "设备", "http://192.0.2.2", "1.2.3"});
    settings.monitors["aa:bb"].push_back({"当前产量", "D100", ValueType::Int32, ByteOrder::ABCD});
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path(u8"jsp32-设备测试");
    std::filesystem::create_directories(root);
    auto path = utf8_path(root / "settings.json");
    save_settings(path, settings);
    auto loaded = load_settings(path);
    assert(loaded.selected_device == "aa:bb");
    assert(loaded.devices.at(0).version == "1.2.3");
    assert(loaded.monitors["aa:bb"].at(0).name == "当前产量");
    PluginPackage package{"heartbeat", {{"index.js", "export default {};"}, {"assets/data.txt", "abc"}}};
    auto zip = utf8_path(root / std::filesystem::path(u8"心跳插件.zip"));
    write_plugin_zip(zip, package);
    auto read = read_plugin_zip(zip);
    assert(read.id == "heartbeat" && read.files.size() == 2);
    std::filesystem::remove_all(root);
}

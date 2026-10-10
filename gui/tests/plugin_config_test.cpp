#include "plugin_config.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace jsp32;
static void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
static Json::Value json(const char *text)
{
    Json::Value result; Json::CharReaderBuilder reader; std::string errors; std::istringstream input(text);
    check(Json::parseFromStream(reader, input, &result, &errors), "fixture parse"); return result;
}
static PluginConfigField &field(PluginConfigForm &form, const std::string &key)
{
    for (auto &f : form.fields) if (f.key == key) return f;
    throw std::runtime_error("field missing");
}
template<class F> static void rejects(F operation)
{
    bool rejected = false;
    try { operation(); } catch (const std::exception &) { rejected = true; }
    check(rejected, "invalid form accepted");
}
int main()
{
    try {
        auto schema = json(R"({
            "interval": {"type":"integer","name":"间隔","default":1000,"min":100,"max":60000},
            "message": {"type":"string","default":"alive"},
            "active": {"type":"boolean"},
            "gain": {"type":"number","default":1.25,"min":0},
            "mode": {"type":"string","choices":["a","b"]}
        })");
        PluginConfigForm form;
        form.load(schema, json(R"({"interval":2000,"active":false,"mode":"b"})"));
        auto config = form.configuration();
        check(config == json(R"({"interval":2000,"active":false,"mode":"b"})"), "defaults omitted and false preserved");
        field(form, "interval").use_default = true;
        check(!form.configuration().isMember("interval"), "restore default removes override");
        field(form, "interval").use_default = false;
        for (auto invalid : {"", "99", "60001", "1.5", "1x", "9223372036854775808"}) {
            field(form, "interval").text = invalid;
            rejects([&] { form.configuration(); });
        }
        field(form, "interval").text = "500";
        field(form, "gain").use_default = false;
        for (auto invalid : {"", "NaN", "inf", "-1", "1x"}) {
            field(form, "gain").text = invalid;
            rejects([&] { form.configuration(); });
        }
        field(form, "gain").text = "2.5";
        check(form.configuration()["gain"].asDouble() == 2.5, "numeric text converts to numeric JSON");
        field(form, "mode").value = "invalid";
        rejects([&] { form.configuration(); });
        field(form, "mode").value = "a";
        field(form, "message").use_default = false;
        field(form, "message").text = "中文内容";
        check(form.configuration()["message"].asString() == "中文内容", "string values");
        form.load(schema, Json::Value(Json::objectValue));
        rejects([&] { form.configuration(); });
        field(form, "active").value = false;
        field(form, "mode").value = "a";
        check(form.configuration().size() == 2, "required fields without defaults editable");
        form.load(Json::Value(), Json::Value());
        check(form.fields.empty(), "unavailable schema");
        form.load(Json::Value(Json::objectValue), Json::Value(Json::objectValue));
        check(form.configuration().isObject() && form.configuration().empty(), "empty schema");
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

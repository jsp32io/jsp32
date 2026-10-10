#include "register_stream.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace jsp32;
static void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void rejects(F fn)
{
    bool rejected = false;
    try { fn(); } catch (const std::exception &) { rejected = true; }
    check(rejected, "expected malformed input rejection");
}
static void decode_tests()
{
    Json::Value info;
    check(register_area_count(info, 'D') == 0 && info.isNull(), "unloaded metadata reads must not mutate state");
    check(!register_info_ready(info), "null metadata is not loaded");
    info["areas"] = Json::Value();
    check(!register_info_ready(info), "empty object is not loaded");
    for (char area : {'X', 'Y', 'M', 'D'}) info["areas"][std::string(1, area)] = 1024;
    check(register_info_ready(info) && register_area_count(info, 'D') == 1024, "complete metadata");
    std::vector<RegisterEvent> events;
    RegisterEventDecoder decoder([&](RegisterEvent e) { events.push_back(std::move(e)); });
    std::string wire = ": connected\r\n\r\nevent: change\r\ndata: {\"r\":\"18446744073709551615\",\"a\":\"D100\",\"v\":65535}\r\n\r\n"
                       ": ping\n\nevent: change\ndata: {\"r\":\"3\",\n"
                       "data: \"a\":\"M10\",\"v\":true}\n\nevent: dropped\ndata: 3\n\n";
    for (const auto &c : wire) decoder.feed(&c, 1);
    check(events.size() == 3, "fragmented events and comments");
    check(events[0].revision == UINT64_MAX && events[0].value == 65535 && events[0].address == "D100", "uint64 revision");
    check(events[1].value == 1 && events[1].address == "M10", "multiline bool event");
    check(events[2].kind == RegisterEvent::Resync, "dropped triggers resync");
    for (auto payload : {"{\"r\":12,\"a\":\"D1\",\"v\":1}", "{\"r\":\"1\",\"a\":\"D1\",\"v\":65536}",
                         "{\"r\":\"1\",\"a\":\"M1\",\"v\":1}", "{\"r\":\"18446744073709551616\",\"a\":\"D1\",\"v\":1}"}) {
        rejects([&] {
            RegisterEventDecoder bad([](RegisterEvent) {});
            std::string frame = std::string("event: change\ndata: ") + payload + "\n\n";
            bad.feed(frame.data(), frame.size());
        });
    }
    rejects([] { RegisterEventDecoder d([](RegisterEvent) {}); std::string big(4097, 'x'); d.feed(big.data(), big.size()); });
    check(register_subscription_query({}).empty(), "empty must not subscribe all");
    check(register_subscription_query({"D101", "D100", "D100"}) == "addresses=D100%2CD101", "deduplicate monitors");
    check(register_subscription_query({"D100", "D101", "M10"}, "D100", 32) == "start=D100&count=32&addresses=M10", "range union");
    rejects([] {
        std::vector<std::string> large;
        for (unsigned i = 0; i < 200; ++i) large.push_back("D" + std::to_string(i));
        register_subscription_query(large);
    });
    std::map<std::string, uint16_t> values;
    std::map<std::string, uint64_t> revisions;
    merge_register_value(values, revisions, "D100", 42, 20);
    merge_register_value(values, revisions, "D100", 41, 19);
    merge_register_value(values, revisions, "D101", 7, 19);
    check(values["D100"] == 42 && values["D101"] == 7, "snapshot race per address");
    revisions.clear();
    merge_register_value(values, revisions, "D100", 0, 1);
    check(values["D100"] == 0, "reboot revision reset");
}
static void transport_test(const std::string &url)
{
    RegisterStream stream;
    stream.start(url, register_subscription_query({}, "D100", 32));
    unsigned connected = 0, disconnected = 0, changes = 0, dropped = 0;
    bool saw_busy_reason = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline && (connected < 2 || changes < 3)) {
        for (auto &event : stream.take()) {
            if (event.kind == RegisterEvent::Connected) {
                ++connected;
                auto snapshot = Api(url).read_registers({"D100", "D101"});
                check(snapshot.revision == 12 && snapshot.values.at("D100") == 42, "HTTP snapshot");
            } else if (event.kind == RegisterEvent::Disconnected) {
                ++disconnected;
                if (event.message.find("too many register watchers") != std::string::npos) saw_busy_reason = true;
            }
            else if (event.kind == RegisterEvent::Resync) ++dropped;
            else if (event.kind == RegisterEvent::Change) ++changes;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto start = std::chrono::steady_clock::now();
    stream.stop();
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "idle stream cancellation");
    check(connected == 2 && disconnected >= 2 && changes == 3 && dropped == 1, "transport 429 retry, reconnect and compact events");
    check(stream.take().empty(), "stop clears stale events");
    check(saw_busy_reason, "429 response reason must be preserved");
}
int main(int argc, char **argv)
{
    try {
        decode_tests();
        if (argc == 2) transport_test(argv[1]);
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

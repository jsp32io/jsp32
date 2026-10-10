#include "discovery.hpp"
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/simple-watch.h>
#include <chrono>

namespace jsp32 {
namespace {
struct Context { Discovery *owner; std::vector<Candidate> *found; std::mutex *mutex; };
void resolved(AvahiServiceResolver *resolver, AvahiIfIndex, AvahiProtocol,
              AvahiResolverEvent event, const char *name, const char *, const char *,
              const char *, const AvahiAddress *address, uint16_t port,
              AvahiStringList *, AvahiLookupResultFlags, void *user)
{
    auto *ctx = static_cast<Context *>(user);
    if (event == AVAHI_RESOLVER_FOUND && address) {
        char ip[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(ip, sizeof(ip), address);
        std::string host = address->proto == AVAHI_PROTO_INET6 ? "[" + std::string(ip) + "]" : ip;
        std::lock_guard lock(*ctx->mutex);
        ctx->found->push_back({name ? name : "JSP32", "http://" + host + ":" + std::to_string(port)});
    }
    avahi_service_resolver_free(resolver);
}
void browsed(AvahiServiceBrowser *browser, AvahiIfIndex interface, AvahiProtocol protocol,
             AvahiBrowserEvent event, const char *name, const char *type,
             const char *domain, AvahiLookupResultFlags, void *user)
{
    if (event != AVAHI_BROWSER_NEW) return;
    auto *ctx = static_cast<Context *>(user);
    avahi_service_resolver_new(avahi_service_browser_get_client(browser), interface, protocol,
                               name, type, domain, AVAHI_PROTO_INET, AvahiLookupFlags(0), resolved, ctx);
}
}
Discovery::Discovery() : thread_([this] { run(); }) {}
Discovery::~Discovery() { stop_ = true; if (thread_.joinable()) thread_.join(); }
std::vector<Candidate> Discovery::take()
{
    std::lock_guard lock(mutex_);
    std::vector<Candidate> result;
    result.swap(found_);
    return result;
}
void Discovery::run()
{
    // Avahi's threaded poll has no external wake-up requirement; short iterations bound shutdown latency.
    while (!stop_) {
        AvahiSimplePoll *poll = avahi_simple_poll_new();
        if (!poll) return;
        int error = 0;
        AvahiClient *client = avahi_client_new(avahi_simple_poll_get(poll), AVAHI_CLIENT_NO_FAIL,
                                               nullptr, nullptr, &error);
        if (!client) { avahi_simple_poll_free(poll); std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
        Context ctx{this, &found_, &mutex_};
        AvahiServiceBrowser *browser = avahi_service_browser_new(client, AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC, "_jsp32._tcp", nullptr, AvahiLookupFlags(0), browsed, &ctx);
        if (browser) {
            for (int i = 0; i < 100 && !stop_; ++i) avahi_simple_poll_iterate(poll, 100);
            avahi_service_browser_free(browser);
        }
        avahi_client_free(client);
        avahi_simple_poll_free(poll);
        if (!browser) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
}

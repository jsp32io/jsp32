#include "discovery.hpp"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <chrono>
#include <memory>

namespace jsp32 {
struct WindowsDiscoveryState {
    std::mutex mutex;
    std::vector<Candidate> found;
    std::atomic<bool> active{true};
};
namespace {
std::string utf8(const wchar_t *wide)
{
    if (!wide) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}
struct ResolveContext {
    std::shared_ptr<WindowsDiscoveryState> state;
    std::wstring name;
    DNS_SERVICE_CANCEL cancel{};
    DNS_SERVICE_RESOLVE_REQUEST request{};
};
void WINAPI resolved(DWORD status, PVOID opaque, PDNS_SERVICE_INSTANCE instance)
{
    std::unique_ptr<ResolveContext> context(static_cast<ResolveContext *>(opaque));
    if (status == ERROR_SUCCESS && instance && instance->ip4Address && instance->wPort &&
        context->state->active) {
        char address[INET_ADDRSTRLEN];
        if (InetNtopA(AF_INET, instance->ip4Address, address, sizeof(address))) {
            std::string name = utf8(instance->pszInstanceName);
            auto suffix = name.find("._jsp32._tcp");
            if (suffix != std::string::npos) name.resize(suffix);
            if (name.empty()) name = "JSP32";
            std::lock_guard lock(context->state->mutex);
            if (context->state->active)
                context->state->found.push_back({name, "http://" + std::string(address) + ":" + std::to_string(instance->wPort)});
        }
    }
    if (instance) DnsServiceFreeInstance(instance);
}
struct BrowseContext {
    std::shared_ptr<WindowsDiscoveryState> state;
    std::atomic<unsigned> references{1};
    void release() { if (references.fetch_sub(1) == 1) delete this; }
};
void WINAPI browsed(DWORD status, PVOID opaque, PDNS_RECORD records)
{
    auto *context = static_cast<BrowseContext *>(opaque);
    context->references.fetch_add(1);
    if (status == ERROR_SUCCESS && context->state->active) {
        for (auto *record = records; record; record = record->pNext) {
            if (record->wType != DNS_TYPE_PTR || !record->Data.PTR.pNameHost) continue;
            auto *resolve = new ResolveContext{context->state, record->Data.PTR.pNameHost, {}};
            resolve->request.Version = DNS_QUERY_REQUEST_VERSION1;
            resolve->request.QueryName = resolve->name.data();
            resolve->request.pResolveCompletionCallback = resolved;
            resolve->request.pQueryContext = resolve;
            if (DnsServiceResolve(&resolve->request, &resolve->cancel) != DNS_REQUEST_PENDING) delete resolve;
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
    if (status == ERROR_CANCELLED) context->release(); // browser ownership
    context->release(); // this callback
}
}
Discovery::Discovery() : windows_(std::make_shared<WindowsDiscoveryState>()), thread_([this] { run(); }) {}
Discovery::~Discovery()
{
    stop_ = true;
    windows_->active = false;
    if (thread_.joinable()) thread_.join();
}
std::vector<Candidate> Discovery::take()
{
    std::lock_guard lock(windows_->mutex);
    std::vector<Candidate> result;
    result.swap(windows_->found);
    return result;
}
void Discovery::run()
{
    auto *context = new BrowseContext{windows_};
    DNS_SERVICE_BROWSE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.QueryName = L"_jsp32._tcp.local";
    request.pBrowseCallback = browsed;
    request.pQueryContext = context;
    DNS_SERVICE_CANCEL cancel{};
    if (DnsServiceBrowse(&request, &cancel) != DNS_REQUEST_PENDING) {
        context->release();
        return;
    }
    while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    DnsServiceBrowseCancel(&cancel);
    // Cancellation delivers ERROR_CANCELLED to browsed(), which releases context.
}
}

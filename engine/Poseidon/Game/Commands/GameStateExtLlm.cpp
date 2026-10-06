// LLM bridge script commands. Project addition - not part of upstream CWR.
//
//   _id   = llmRequest "text"   -> number: request id (1 or more), or -1 if refused
//   _text = llmResult _id       -> string: "" while still waiting, otherwise the reply.
//                                  A failure comes back as text starting with "ERROR:".
//                                  Reading a finished reply removes it.
//
// llmRequest POSTs the text to a relay program on this machine and returns at once.
// The HTTP call runs on a worker thread, so the simulation never waits on it.
//
// The relay address is fixed here (default http://127.0.0.1:8777/) or by the
// POSEIDON_LLM_URL environment variable. A script can never choose it, so a
// mission cannot make the game call arbitrary hosts.

#include <Evaluator/express.hpp>
using namespace Poseidon;

#include <curl/curl.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace
{
constexpr size_t kMaxRequestBytes = 64 * 1024;
constexpr size_t kMaxReplyBytes = 256 * 1024;
constexpr int kMaxInFlight = 4;
constexpr long kConnectTimeoutSec = 3;
constexpr long kTotalTimeoutSec = 180;

struct LlmSlot
{
    bool done = false;
    std::string text;
};

struct LlmBridgeState
{
    std::mutex mutex;
    std::unordered_map<int, LlmSlot> slots;
    int nextId = 1;
    int inFlight = 0;
    std::string url;
};

// Allocated once and never freed on purpose: a worker thread may still be
// finishing while the process shuts down, and it must not touch a destroyed object.
LlmBridgeState& Bridge()
{
    static LlmBridgeState* state = []
    {
        auto* created = new LlmBridgeState;
        const char* env = std::getenv("POSEIDON_LLM_URL");
        created->url = (env != nullptr && env[0] != '\0') ? env : "http://127.0.0.1:8777/";
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return created;
    }();
    return *state;
}

size_t AppendReply(char* data, size_t size, size_t count, void* user)
{
    auto* reply = static_cast<std::string*>(user);
    const size_t bytes = size * count;
    if (reply->size() + bytes > kMaxReplyBytes)
    {
        return 0; // tells curl to abort: the reply is too large
    }
    reply->append(data, bytes);
    return bytes;
}

void FinishRequest(int id, const std::string& text)
{
    LlmBridgeState& bridge = Bridge();
    std::lock_guard<std::mutex> lock(bridge.mutex);
    LlmSlot& slot = bridge.slots[id];
    slot.text = text;
    slot.done = true;
    --bridge.inFlight;
}

void RunRequest(int id, std::string body)
{
    LlmBridgeState& bridge = Bridge();
    std::string reply;
    std::string error;
    char errorBuffer[CURL_ERROR_SIZE] = {0};

    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        error = "could not start the HTTP client";
    }
    else
    {
        curl_slist* headers = curl_slist_append(nullptr, "Content-Type: text/plain; charset=utf-8");
        headers = curl_slist_append(headers, "Expect:");

        curl_easy_setopt(curl, CURLOPT_URL, bridge.url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTotalTimeoutSec);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendReply);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);

        const CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        if (code != CURLE_OK)
        {
            error = (errorBuffer[0] != '\0') ? errorBuffer : curl_easy_strerror(code);
        }
        else if (status < 200 || status >= 300)
        {
            error = "relay answered HTTP " + std::to_string(status);
        }
        else if (reply.empty())
        {
            error = "relay sent an empty reply";
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    FinishRequest(id, error.empty() ? reply : ("ERROR: " + error));
}
} // namespace

GameValue LlmRequest(const GameState* /*state*/, GameValuePar oper1)
{
    const GameStringType text = oper1;
    const char* raw = (const char*)text;
    std::string body = (raw != nullptr) ? raw : "";
    if (body.empty() || body.size() > kMaxRequestBytes)
    {
        return -1.0f;
    }

    LlmBridgeState& bridge = Bridge();
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(bridge.mutex);
        if (bridge.inFlight >= kMaxInFlight)
        {
            return -1.0f;
        }
        id = bridge.nextId++;
        bridge.slots[id] = LlmSlot{};
        ++bridge.inFlight;
    }

    try
    {
        std::thread(RunRequest, id, std::move(body)).detach();
    }
    catch (const std::system_error&)
    {
        FinishRequest(id, "ERROR: could not start a worker thread");
    }
    return static_cast<float>(id);
}

GameValue LlmResult(const GameState* /*state*/, GameValuePar oper1)
{
    const int id = static_cast<int>((float)oper1);

    LlmBridgeState& bridge = Bridge();
    std::lock_guard<std::mutex> lock(bridge.mutex);
    const auto found = bridge.slots.find(id);
    if (found == bridge.slots.end())
    {
        return GameValue("ERROR: unknown request id");
    }
    if (!found->second.done)
    {
        return GameValue("");
    }
    const std::string text = std::move(found->second.text);
    bridge.slots.erase(found);
    return GameValue(text.c_str());
}

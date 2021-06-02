#include "request_handler.h"
#include "channel_encryption.hpp"
#include "http.h"
#include "omq_server.h"
#include "oxen_logger.h"
#include "signature.h"
#include "service_node.h"
#include "string_utils.hpp"
#include "utils.hpp"

#include <chrono>
#include <cpr/cpr.h>
#include <future>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <oxenmq/base32z.h>
#include <oxenmq/base64.h>
#include <oxenmq/hex.h>

using nlohmann::json;

namespace oxen {

// Timeout for onion-request-to-url requests.  Onion requests have a 30s timeout so we choose a
// timeout a bit shorter than that so that we still have a good chance of the error response getting
// back to the client before the entry node's request times out.
inline constexpr auto ONION_URL_TIMEOUT = 25s;

std::string to_string(const Response& res) {

    std::stringstream ss;

    ss << "Status: " << res.status.first << " " << res.status.second
        << ", Content-Type: " << (res.content_type.empty() ? "(unspecified)" : res.content_type)
        << ", Body: <" << res.body << ">";

    return ss.str();
}

namespace {

json snodes_to_json(const std::vector<sn_record_t>& snodes) {

    json res_body;
    json snodes_json = json::array();

    for (const auto& sn : snodes) {
        snodes_json.push_back(json{
                {"address", oxenmq::to_base32z(sn.pubkey_legacy.view()) + ".snode"}, // Deprecated, use pubkey_legacy instead
                {"pubkey_legacy", sn.pubkey_legacy.hex()},
                {"pubkey_x25519", sn.pubkey_x25519.hex()},
                {"pubkey_ed25519", sn.pubkey_ed25519.hex()},
                {"port", std::to_string(sn.port)}, // Why is this a string?
                {"ip", sn.ip}});
    }

    res_body["snodes"] = std::move(snodes_json);

    return res_body;
}

std::string obfuscate_pubkey(std::string_view pk) {
    std::string res;
    res += pk.substr(0, 2);
    res += "...";
    res += pk.substr(pk.length() - 3);
    return res;
}

} // anon. namespace


std::string computeMessageHash(std::vector<std::string_view> parts, bool hex) {
    SHA512_CTX ctx;
    SHA512_Init(&ctx);
    for (const auto& s : parts)
        SHA512_Update(&ctx, s.data(), s.size());

    std::string hashResult;
    hashResult.resize(SHA512_DIGEST_LENGTH);
    SHA512_Final(reinterpret_cast<unsigned char*>(hashResult.data()), &ctx);
    if (hex)
        hashResult = oxenmq::to_hex(hashResult);
    return hashResult;
}

bool validateTimestamp(std::chrono::system_clock::time_point timestamp, std::chrono::milliseconds ttl) {
    auto now = std::chrono::system_clock::now();
    // Timestamp must not be in the future (with some tolerance)
    if (timestamp > now + 10s)
        return false;

    // Don't accept timestamp that has already expired
    if (timestamp + ttl < now)
        return false;

    return true;
}

bool validateTTL(std::chrono::milliseconds ttl) {
    // Minimum time to live of 10 seconds, maximum of 14 days
    return ttl >= 10s && ttl <= 14 * 24h;
}


RequestHandler::RequestHandler(
        ServiceNode& sn,
        const ChannelEncryption& ce)
    : service_node_{sn}, channel_cipher_(ce) {

    // Periodically clean up any proxy request futures
    service_node_.omq_server()->add_timer([this] {
        pending_proxy_requests_.remove_if(
                [](auto& f) { return f.wait_for(0ms) == std::future_status::ready; });
    }, 1s);
}

Response RequestHandler::handle_wrong_swarm(const user_pubkey_t& pubKey) {

    OXEN_LOG(trace, "Got client request to a wrong swarm");

    return {
        http::MISDIRECTED_REQUEST,
        snodes_to_json(service_node_.get_snodes_by_pk(pubKey)).dump(),
        http::json};
}

Response RequestHandler::process_store(const json& params) {

    for (const auto& field : {"pubKey", "ttl", "timestamp", "data"}) {
        if (!params.contains(field)) {
            OXEN_LOG(debug, "Bad client request: no `{}` field", field);
            return {
                http::BAD_REQUEST,
                fmt::format("invalid json: no `{}` field\n", field)};
        }
    }

    const auto& ttl_in = params.at("ttl");
    const auto& timestamp_in = params.at("timestamp");
    const auto& data = params.at("data").get_ref<const std::string&>();

    OXEN_LOG(trace, "Storing message: {}", data);

    user_pubkey_t pk;
    if (!pk.load(params.at("pubKey").get<std::string>())) {
        auto msg = fmt::format("Pubkey must be {} characters long\n",
                               get_user_pubkey_size());
        OXEN_LOG(debug, "{}", msg);
        return {http::BAD_REQUEST, std::move(msg)};
    }

    if (data.size() > MAX_MESSAGE_BODY) {
        OXEN_LOG(debug, "Message body too long: {}", data.size());

        auto msg =
            fmt::format("Message body exceeds maximum allowed length of {}\n",
                        MAX_MESSAGE_BODY);
        return {http::BAD_REQUEST, std::move(msg)};
    }

    if (!service_node_.is_pubkey_for_us(pk)) {
        return this->handle_wrong_swarm(pk);
    }

    using namespace std::chrono;
    std::optional<milliseconds> ttl;
    if (ttl_in.is_number_unsigned())
        ttl.emplace(ttl_in.get<uint64_t>());
    else if (uint64_t ttlInt; ttl_in.is_string() &&
            util::parse_int(ttl_in.get_ref<const std::string&>(), ttlInt))
        ttl.emplace(ttlInt);
    if (!ttl || !validateTTL(*ttl)) {
        OXEN_LOG(debug, "Forbidden. Invalid TTL: {}", ttl_in.dump());
        return {http::FORBIDDEN, "Provided TTL is not valid.\n"};
    }

    std::optional<system_clock::time_point> timestamp;
    if (timestamp_in.is_number_unsigned())
        timestamp.emplace(milliseconds{timestamp_in.get<uint64_t>()});
    else if (uint64_t t; timestamp_in.is_string() &&
            util::parse_int(ttl_in.get_ref<const std::string&>(), t))
        timestamp.emplace(milliseconds{t});
    if (!timestamp || !validateTimestamp(*timestamp, *ttl)) {
        OXEN_LOG(debug, "Forbidden. Invalid Timestamp: {}", timestamp_in.dump());
        return {http::NOT_ACCEPTABLE, "Timestamp error: check your clock\n"};
    }

    auto messageHash = computeMessageHash({
        std::to_string(duration_cast<milliseconds>(timestamp->time_since_epoch()).count()),
        std::to_string(ttl->count()),
        pk.str(),
        data}, true);

    bool success;

    try {
        success = service_node_.process_store({pk.str(), data, messageHash, *ttl, *timestamp});
    } catch (const std::exception& e) {
        OXEN_LOG(critical,
                 "Internal Server Error. Could not store message for {}",
                 obfuscate_pubkey(pk.str()));
        return {http::INTERNAL_SERVER_ERROR, e.what()};
    }

    if (!success) {

        OXEN_LOG(warn, "Service node is initializing");
        return {http::SERVICE_UNAVAILABLE,
            "Service node is initializing\n"};
    }

    OXEN_LOG(trace, "Successfully stored message for {}",
             obfuscate_pubkey(pk.str()));

    json res_body;
    /// NOTE: difficulty is not longer used by modern clients, but
    /// we send this to avoid breaking older clients.
    res_body["difficulty"] = 1;

    return {http::OK, res_body.dump(), http::json};
}

inline const static std::unordered_set<std::string> allowed_oxend_endpoints{{
    "get_service_nodes"s, "ons_resolve"s}};

void RequestHandler::process_oxend_request(
    const json& params, std::function<void(oxen::Response)> cb) {

    std::string endpoint;
    if (auto it = params.find("endpoint");
            it == params.end() || !it->is_string())
        return cb({http::BAD_REQUEST, "missing 'endpoint'"});
    else
        endpoint = it->get<std::string>();

    if (!allowed_oxend_endpoints.count(endpoint))
        return cb({http::BAD_REQUEST, "Endpoint not allowed: " + endpoint});

    std::optional<std::string> oxend_params;
    if (auto it = params.find("params"); it != params.end()) {
        if (!it->is_object())
            return cb({http::BAD_REQUEST, "invalid oxend 'params' argument"});
        oxend_params = it->dump();
    }

    service_node_.omq_server().oxend_request(
        "rpc." + endpoint,
        [cb = std::move(cb)](bool success, auto&& data) {
            std::string err;
            // Currently we only support json endpoints; if we want to support non-json endpoints
            // (which end in ".bin") at some point in the future then we'll need to return those
            // endpoint results differently here.
            if (success && data.size() >= 2 && data[0] == "200")
                return cb({http::OK,
                    R"({"result":)" + std::move(data[1]) + "}",
                    http::json});
            return cb({http::BAD_REQUEST,
                data.size() >= 2 && !data[1].empty()
                    ? std::move(data[1]) : "Unknown oxend error"s});
        },
        oxend_params);
}

Response RequestHandler::process_retrieve_all() {

    std::vector<storage::Item> all_entries;

    bool res = service_node_.get_all_messages(all_entries);

    if (!res) {
        return Response{http::INTERNAL_SERVER_ERROR,
                        "could not retrieve all entries\n"};
    }

    json messages = json::array();

    for (auto& entry : all_entries) {
        json item;
        item["data"] = entry.data;
        item["pk"] = entry.pub_key;
        messages.push_back(item);
    }

    json res_body;
    res_body["messages"] = messages;

    return Response{http::OK, res_body.dump(), http::json};
}

Response RequestHandler::process_snodes_by_pk(const json& params) const {

    auto it = params.find("pubKey");
    if (it == params.end()) {
        OXEN_LOG(debug, "Bad client request: no `pubKey` field");
        return {http::BAD_REQUEST,
                        "invalid json: no `pubKey` field\n"};
    }

    user_pubkey_t pk;
    if (!pk.load(params.at("pubKey").get<std::string>(), success)) {

        auto msg = fmt::format("Pubkey must be {} hex digits long\n",
                               get_user_pubkey_size());
        OXEN_LOG(debug, "{}", msg);
        return Response{http::BAD_REQUEST, std::move(msg)};
    }

    const std::vector<sn_record_t> nodes = service_node_.get_snodes_by_pk(pk);

    OXEN_LOG(debug, "Snodes by pk size: {}", nodes.size());

    const json res_body = snodes_to_json(nodes);

    OXEN_LOG(debug, "Snodes by pk: {}", res_body.dump());

    return Response{http::OK, res_body.dump(), http::json};
}

Response RequestHandler::process_retrieve(const json& params) {

    constexpr const char* fields[] = {"pubKey", "lastHash"};

    for (const auto& field : fields) {
        if (!params.contains(field)) {
            auto msg = fmt::format("invalid json: no `{}` field", field);
            OXEN_LOG(debug, "{}", msg);
            return Response{http::BAD_REQUEST, std::move(msg)};
        }
    }

    user_pubkey_t pk;
    if (!pk.load(params["pubKey"].get<std::string>())) {

        auto msg = fmt::format("Pubkey must be {} characters long\n",
                               get_user_pubkey_size());
        OXEN_LOG(debug, "{}", msg);
        return Response{http::BAD_REQUEST, std::move(msg)};
    }

    if (!service_node_.is_pubkey_for_us(pk)) {
        return this->handle_wrong_swarm(pk);
    }

    const std::string& last_hash =
        params.at("lastHash").get_ref<const std::string&>();

    // Note: We removed long-polling

    std::vector<storage::Item> items;

    if (!service_node_.retrieve(pk.str(), last_hash, items)) {

        auto msg = fmt::format(
            "Internal Server Error. Could not retrieve messages for {}",
            obfuscate_pubkey(pk.str()));
        OXEN_LOG(critical, "{}", msg);

        return Response{http::INTERNAL_SERVER_ERROR, std::move(msg)};
    }

    if (!items.empty()) {
        OXEN_LOG(trace, "Successfully retrieved messages for {}",
                 obfuscate_pubkey(pk.str()));
    }

    json res_body;
    json messages = json::array();

    for (const auto& item : items) {
        json message;
        message["hash"] = item.hash;
        message["expiration"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                item.expiration.time_since_epoch()
        ).count();
        message["data"] = item.data;
        messages.push_back(message);
    }

    res_body["messages"] = messages;

    return Response{http::OK, res_body.dump(), http::json};
}

void RequestHandler::process_client_req(
    std::string_view req_json, std::function<void(oxen::Response)> cb) {

    OXEN_LOG(trace, "process_client_req str <{}>", req_json);

    const json body = json::parse(req_json, nullptr, false);
    if (body.is_discarded()) {
        OXEN_LOG(debug, "Bad client request: invalid json");
        return cb(Response{http::BAD_REQUEST, "invalid json\n"});
    }

    if (OXEN_LOG_ENABLED(trace))
        OXEN_LOG(trace, "process_client_req json <{}>", body.dump(2));

    const auto method_it = body.find("method");
    if (method_it == body.end() || !method_it->is_string()) {
        OXEN_LOG(debug, "Bad client request: no method field");
        return cb(Response{http::BAD_REQUEST, "invalid json: no `method` field\n"});
    }

    const auto& method_name = method_it->get_ref<const std::string&>();

    OXEN_LOG(trace, "  - method name: {}", method_name);

    const auto params_it = body.find("params");
    if (params_it == body.end() || !params_it->is_object()) {
        OXEN_LOG(debug, "Bad client request: no params field");
        return cb(Response{http::BAD_REQUEST, "invalid json: no `params` field\n"});
    }

    if (method_name == "store") {
        OXEN_LOG(debug, "Process client request: store");
        return cb(process_store(*params_it));
    }
    if (method_name == "retrieve") {
        OXEN_LOG(debug, "Process client request: retrieve");
        return cb(process_retrieve(*params_it));
        // TODO: maybe we should check if (some old) clients requests
        // long-polling and then wait before responding to prevent spam

    }
    if (method_name == "get_snodes_for_pubkey") {
        OXEN_LOG(debug, "Process client request: snodes for pubkey");
        return cb(process_snodes_by_pk(*params_it));
    }
    if (method_name == "oxend_request") {
        OXEN_LOG(debug, "Process client request: oxend_request");
        return process_oxend_request(*params_it, std::move(cb));
    }

    OXEN_LOG(debug, "Bad client request: unknown method '{}'", method_name);
    return cb({http::BAD_REQUEST, "no method " + method_name});
}

void RequestHandler::process_storage_test_req(
        uint64_t height,
        legacy_pubkey tester,
        std::string msg_hash_hex,
        std::function<void(MessageTestStatus, std::string, std::chrono::steady_clock::duration)> callback) {

    /// TODO: we never actually test that `height` is within any reasonable
    /// time window (or that it is not repeated multiple times), we should do
    /// that! This is done implicitly to some degree using
    /// `block_hashes_cache_`, which holds a limited number of recent blocks
    /// only and fails if an earlier block is requested

    auto started = std::chrono::steady_clock::now();
    auto [status, answer] = service_node_.process_storage_test_req(
            height, tester, msg_hash_hex);

    if (status == MessageTestStatus::RETRY) {
        // Our first attempt returned a RETRY, so set up a timer to keep retrying

        auto timer = std::make_shared<oxenmq::TimerID>();
        auto& timer_ref = *timer;
        service_node_.omq_server()->add_timer(timer_ref, [
                this,
                timer=std::move(timer),
                height,
                tester,
                hash=std::move(msg_hash_hex),
                started,
                callback=std::move(callback)] {
            auto elapsed = std::chrono::steady_clock::now() - started;

            OXEN_LOG(trace, "Performing storage test retry, {} since started",
                    util::friendly_duration(elapsed));

            auto [status, answer] = service_node_.process_storage_test_req(
                    height, tester, hash);
            if (status == MessageTestStatus::RETRY && elapsed < TEST_RETRY_PERIOD && !service_node_.shutting_down())
                return; // Still retrying so wait for the next call
            service_node_.omq_server()->cancel_timer(*timer);
            callback(status, std::move(answer), elapsed);
        }, TEST_RETRY_INTERVAL);
    } else {
        callback(status, std::move(answer), std::chrono::steady_clock::now() - started);
    }
}

Response RequestHandler::wrap_proxy_response(Response res,
                                             const x25519_pubkey& client_key,
                                             EncryptType enc_type,
                                             bool embed_json,
                                             bool base64) const {

    int status = res.status.first;
    std::string body;
    if (embed_json && res.content_type == http::json)
        body = fmt::format(R"({{"status":{},"body":{}}})", status, res.body);
    else
        body = json{{"status", status}, {"body", res.body}}.dump();

    std::string ciphertext = channel_cipher_.encrypt(enc_type, body, client_key);
    if (base64)
        ciphertext = oxenmq::to_base64(std::move(ciphertext));

    // why does this have to be json???
    return Response{http::OK, std::move(ciphertext), http::json};
}

void RequestHandler::process_onion_req(std::string_view ciphertext,
                                       OnionRequestMetadata data) {
    if (!service_node_.snode_ready())
        return data.cb({
            http::SERVICE_UNAVAILABLE,
            fmt::format("Snode not ready: {}", service_node_.own_address().pubkey_ed25519)});

    OXEN_LOG(debug, "process_onion_req");

    service_node_.record_onion_request();

    var::visit([&](auto&& x) { process_onion_req(std::move(x), std::move(data)); },
            process_ciphertext_v2(channel_cipher_, ciphertext, data.ephem_key, data.enc_type));
}

void RequestHandler::process_onion_req(FinalDestinationInfo&& info,
        OnionRequestMetadata&& data) {
    OXEN_LOG(debug, "We are the final destination in the onion request!");

    process_onion_exit(
            info.body,
            [this, data = std::move(data), json = info.json, b64 = info.base64]
            (oxen::Response res) {
                data.cb(wrap_proxy_response(std::move(res), data.ephem_key, data.enc_type, json, b64));
            });
}

void RequestHandler::process_onion_req(RelayToNodeInfo&& info,
        OnionRequestMetadata&& data) {
    auto& [payload, ekey, etype, dest] = info;

    auto dest_node = service_node_.find_node(dest);
    if (!dest_node) {
        auto msg = fmt::format("Next node not found: {}", dest);
        OXEN_LOG(warn, "{}", msg);
        return data.cb({http::BAD_GATEWAY, std::move(msg)});
    }

    auto on_response = [cb=std::move(data.cb)](bool success, std::vector<std::string> data) {
        // Processing the result we got from upstream

        if (!success) {
            OXEN_LOG(debug, "[Onion request] Request time out");
            return cb({http::GATEWAY_TIMEOUT, "Request time out"});
        }

        // We expect a two-part message, but for forwards compatibility allow extra parts
        if (data.size() < 2) {
            OXEN_LOG(debug, "[Onion request] Invalid response; expected at least 2 parts");
            return cb({http::INTERNAL_SERVER_ERROR, "Invalid response from snode"});
        }

        Response res{http::INTERNAL_SERVER_ERROR, std::move(data[1]), http::json};
        if (int code; util::parse_int(data[0], code))
            res.status = http::from_code(code);

        /// We use http status codes (for now)
        if (res.status != http::OK)
            OXEN_LOG(debug, "Onion request relay failed with: {}", res.body);

        cb(std::move(res));
    };

    OXEN_LOG(debug, "send_onion_to_sn, sn: {}", dest_node->pubkey_legacy);

    data.ephem_key = ekey;
    data.enc_type = etype;
    service_node_.send_onion_to_sn(
            *dest_node, std::move(payload), std::move(data), std::move(on_response));
}


void RequestHandler::process_onion_req(
        RelayToServerInfo&& info, OnionRequestMetadata&& data) {
    OXEN_LOG(debug, "We are to forward the request to url: {}{}",
            info.host, info.target);

    // Forward the request to url but only if it ends in `/lsrpc`
    if (!(info.protocol == "http" || info.protocol == "https") ||
            !is_onion_url_target_allowed(info.target))
        return data.cb(wrap_proxy_response({http::BAD_REQUEST, "Invalid url"},
            data.ephem_key, data.enc_type));

    std::string urlstr;
    urlstr.reserve(info.protocol.size() + 3 + info.host.size() + 6 /*:port*/ + 1 + info.target.size());
    urlstr += info.protocol;
    urlstr += "://";
    urlstr += info.host;
    if (info.port != (info.protocol == "https" ? 443 : 80)) {
        urlstr += ':';
        urlstr += std::to_string(info.port);
    }
    if (!util::starts_with(info.target, "/"))
        urlstr += '/';
    urlstr += info.target;

    service_node_.record_proxy_request();

    pending_proxy_requests_.emplace_front(
        cpr::PostCallback(
            [&omq=*service_node_.omq_server(), cb=std::move(data.cb)](cpr::Response r) {
                Response res;
                if (r.error.code != cpr::ErrorCode::OK) {
                    OXEN_LOG(debug, "Onion proxied request to {} failed: {}", r.url.str(), r.error.message);
                    res.body = r.error.message;
                    if (r.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT)
                        res.status = http::GATEWAY_TIMEOUT;
                    else
                        res.status = http::BAD_GATEWAY;
                } else {
                    res.status.first = r.status_code;
                    res.status.second = r.status_line;
                    for (auto& [k, v] : r.header) {
                        auto& [header, val] = res.headers.emplace_back(std::move(k), std::move(v));
                        if (util::string_iequal(header, "content-type"))
                            res.content_type = val;
                    }
                    res.body = std::move(r.text);
                }

                cb(std::move(res));
            },
            cpr::Url{std::move(urlstr)},
            cpr::Timeout{ONION_URL_TIMEOUT},
            cpr::Ssl(cpr::ssl::TLSv1_2{}),
            cpr::MaxRedirects{0},
            cpr::Body{std::move(info.payload)}
        )
    );
}

void RequestHandler::process_onion_req(ProcessCiphertextError&& error,
        OnionRequestMetadata&& data) {

    switch (error) {
        case ProcessCiphertextError::INVALID_CIPHERTEXT:
            return data.cb({http::BAD_REQUEST, "Invalid ciphertext"});
        case ProcessCiphertextError::INVALID_JSON:
            return data.cb(wrap_proxy_response({http::BAD_REQUEST, "Invalid json"},
                    data.ephem_key, data.enc_type));
    }
}

void RequestHandler::process_onion_exit(
    std::string_view body,
    std::function<void(oxen::Response)> cb) {

    OXEN_LOG(debug, "Processing onion exit!");

    if (!service_node_.snode_ready())
        return cb({http::SERVICE_UNAVAILABLE, "Snode not ready"});

    this->process_client_req(body, std::move(cb));
}

} // namespace oxen

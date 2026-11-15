#include "xbox_live_on_bds/xbl/RtaClient.h"

namespace xbox_live_on_bds::xbl {

namespace {
constexpr std::string_view RtaUrl             = "wss://rta.xboxlive.com/connect";
constexpr std::string_view SessionConnections = "https://sessiondirectory.xboxlive.com/connections/";

// RTA message types: [type, ...]
enum MessageType : int { Subscribe = 1, Unsubscribe = 2, Event = 3, Resync = 4 };
} // namespace

std::unique_ptr<RtaClient>
RtaClient::connect(std::string const& authorization, Callbacks callbacks, std::string& error) {
    auto socket = net::WebSocket::connect(
        RtaUrl,
        {
            {"Authorization", authorization}
    },
        error
    );
    if (!socket) return nullptr;
    std::unique_ptr<RtaClient> client(new RtaClient(std::move(socket), std::move(callbacks)));
    client->subscribe(SessionConnections);
    return client;
}

RtaClient::RtaClient(std::unique_ptr<net::WebSocket> socket, Callbacks callbacks)
: mSocket(std::move(socket)),
  mCallbacks(std::move(callbacks)),
  mThread([this] { receiveLoop(); }) {}

RtaClient::~RtaClient() {
    mSocket->close();
    if (mThread.joinable()) mThread.join();
}

void RtaClient::subscribe(std::string_view resourceUri) {
    int sequence;
    {
        std::lock_guard lock(mSequenceMutex);
        sequence = mNextSequence++;
    }
    mSocket->send(nlohmann::json::array({Subscribe, sequence, resourceUri}).dump());
}

void RtaClient::receiveLoop() {
    while (auto text = mSocket->receive()) {
        // The service controls the shape; a wrong type must drop the message, not end the process.
        try {
            auto message = nlohmann::json::parse(*text, nullptr, false);
            if (!message.is_array() || message.empty() || !message[0].is_number_integer()) continue;
            switch (message[0].get<int>()) {
            case Subscribe: // [1, sequence, status, subscriptionId, data]
                if (message.size() >= 5 && message[4].is_object() && message[4].contains("ConnectionId")
                    && mCallbacks.onConnectionId) {
                    mCallbacks.onConnectionId(message[4]["ConnectionId"].get<std::string>());
                }
                break;
            case Event: // [3, subscriptionId, data]
                if (message.size() >= 3 && mCallbacks.onEvent) mCallbacks.onEvent(message[2]);
                break;
            default:
                break;
            }
        } catch (std::exception const&) {
            // Skip it.
        }
    }
    if (mCallbacks.onClosed) mCallbacks.onClosed(mSocket->closeReason());
}

} // namespace xbox_live_on_bds::xbl

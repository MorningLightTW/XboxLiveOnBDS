#include "xbox_live_on_bds/LinkMonitor.h"

#include <algorithm>
#include <cmath>

#include "ll/api/data/CancellableCallback.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "xbox_live_on_bds/util/Redact.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace xbox_live_on_bds {

namespace {

constexpr auto SampleEvery      = std::chrono::seconds(5);
constexpr int  FirstReportAfter = 3; // samples: long enough for ICE to settle on its final path

std::string_view pathName(bool relay) { return relay ? "Microsoft relay" : "direct connection"; }

long long percentile(std::vector<long long> sorted, double p) {
    if (sorted.empty()) return 0;
    auto at = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(at, sorted.size() - 1)];
}

/// Mean change in ping from one sample to the next; empty below two samples.
std::optional<double> jitterOf(std::vector<long long> const& pings) {
    if (pings.size() < 2) return std::nullopt;
    double sum = 0;
    for (std::size_t i = 1; i < pings.size(); ++i) sum += static_cast<double>(std::llabs(pings[i] - pings[i - 1]));
    return sum / static_cast<double>(pings.size() - 1);
}

} // namespace

LinkMonitor::LinkMonitor(ll::io::Logger& logger) : mLogger(logger) {}

LinkMonitor::~LinkMonitor() { stop(); }

void LinkMonitor::start() {
    if (mRunning) return;
    mRunning = true;
    schedule();
}

void LinkMonitor::stop() {
    mRunning = false;
    if (mTimer) mTimer->cancel();
    mTimer.reset();
    mLinks.clear();
}

void LinkMonitor::schedule() {
    if (!mRunning) return;
    mTimer = ll::thread::ServerThreadExecutor::getDefault().executeAfter(
        [this] {
            sampleAll();
            schedule();
        },
        SampleEvery
    );
}

void LinkMonitor::onJoin(Player const& player) {
    auto uuid               = player.getUuid();
    mLinks[uuid.asString()] = Link{uuid, player.getRealName(), std::chrono::steady_clock::now()};
}

void LinkMonitor::onLeave(Player const& player) {
    auto it = mLinks.find(player.getUuid().asString());
    if (it == mLinks.end()) return;
    summarize(it->second);
    mLinks.erase(it);
}

void LinkMonitor::onServerStopping() {
    for (auto const& [key, link] : mLinks) summarize(link);
    mLinks.clear();
}

void LinkMonitor::sampleAll() {
    auto level = ll::service::getLevel();
    if (!level) return;
    for (auto& [key, link] : mLinks) {
        auto* player = level->getPlayer(link.uuid);
        if (!player) continue;
        auto status = player->getNetworkStatus();
        if (!status) continue;
        bool relay = status->mUsingRelays;
        if (!link.pings.empty() && relay != link.lastRelay) {
            mLogger.debug(
                "{} switched to a {}",
                util::Redactor::instance().player(link.name),
                relay ? "Microsoft relay" : "direct connection"
            );
        }
        link.lastRelay = relay;
        link.pings.push_back(status->mCurrentPing->count());
        link.maxLoss = std::max<float>(link.maxLoss, status->mCurrentPacketLoss);
        if (relay) ++link.relaySamples;
        if (!link.reported && static_cast<int>(link.pings.size()) >= FirstReportAfter) {
            link.reported = true;
            mLogger.debug(
                "{} is on a {}, ping {} ms, packet loss {:.1f}%",
                util::Redactor::instance().player(link.name),
                pathName(relay),
                status->mAveragePing->count(),
                status->mAveragePacketLoss * 100.0f
            );
        }
    }
}

void LinkMonitor::summarize(Link const& link) const {
    if (link.pings.size() < 2) return; // Too short a visit to say anything.
    std::vector<long long> sorted = link.pings;
    std::ranges::sort(sorted);
    double jitter = jitterOf(link.pings).value_or(0);
    auto   minutes =
        std::chrono::duration<double, std::ratio<60>>(std::chrono::steady_clock::now() - link.joined).count();
    mLogger.debug(
        "{} left after {:.1f} min: ping min {} / median {} / 95% {} / max {} ms, jitter {:.0f} ms, worst packet loss "
        "{:.1f}%, on a relay {}% of the time ({} samples)",
        util::Redactor::instance().player(link.name),
        minutes,
        sorted.front(),
        percentile(sorted, 0.5),
        percentile(sorted, 0.95),
        sorted.back(),
        jitter,
        link.maxLoss * 100.0f,
        link.relaySamples * 100 / static_cast<int>(link.pings.size()),
        link.pings.size()
    );
}

std::optional<LinkMonitor::Stats> LinkMonitor::stats(Player const& player) const {
    auto it = mLinks.find(player.getUuid().asString());
    if (it == mLinks.end()) return std::nullopt;
    auto const& link = it->second;
    Stats       stats;
    stats.minutes =
        std::chrono::duration<double, std::ratio<60>>(std::chrono::steady_clock::now() - link.joined).count();
    stats.jitter = jitterOf(link.pings);
    if (!link.pings.empty()) stats.relayPercent = link.relaySamples * 100 / static_cast<int>(link.pings.size());
    return stats;
}

} // namespace xbox_live_on_bds

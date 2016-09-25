/*
 * Copyright (c) 2016, Airbitz, Inc.
 * All rights reserved.
 *
 * See the LICENSE file for more information.
 */

#include "ServerCache.hpp"
#include "../Utility.hpp"
#include "../../crypto/Encoding.hpp"
#include "../../json/JsonArray.hpp"
#include "../../json/JsonObject.hpp"
#include "../../util/Debug.hpp"
#include "../../General.hpp"


namespace abcd {

constexpr auto LIBBITCOIN_PREFIX = "tcp://";
constexpr auto STRATUM_PREFIX = "stratum://";
constexpr auto LIBBITCOIN_PREFIX_LENGTH = 6;
constexpr auto STRATUM_PREFIX_LENGTH = 10;
constexpr auto MAX_SCORE = 100;
constexpr auto MIN_SCORE = -100;

constexpr time_t onHeaderTimeout = 5;
#define RESPONSE_TIME_UNINITIALIZED 999999999

struct ServerScoreJson:
        public JsonObject
{
    ABC_JSON_CONSTRUCTORS(ServerScoreJson, JsonObject)

    ABC_JSON_STRING(serverUrl, "serverUrl", "")
    ABC_JSON_INTEGER(serverScore, "serverScore", 0)
    ABC_JSON_INTEGER(serverResponseTime, "serverResponseTime", RESPONSE_TIME_UNINITIALIZED)
};

ServerCache::ServerCache(const std::string &path):
    path_(path),
    dirty_(false),
    lastUpScoreTime_(0)
{
}

void
ServerCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    servers_.clear();
}

Status
ServerCache::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ABC_Debug(1, "ServerCache::load");

    // Load the saved server scores if they exist
    JsonArray serverScoresJsonArray;

    // It's ok if this fails
    serverScoresJsonArray.load(path_).log();

    // Add any new servers coming out of the auth server
    std::vector<std::string> bitcoinServers = generalBitcoinServers();

    size_t size = bitcoinServers.size();
    for (size_t i = 0; i < size; i++)
    {
        std::string serverUrlNew = bitcoinServers[i];

        size_t sizeScores = serverScoresJsonArray.size();
        bool serversMatch = false;
        for (size_t j = 0; j < sizeScores; j++)
        {
            ServerScoreJson ssj = serverScoresJsonArray[j];
            std::string serverUrl(ssj.serverUrl());

            if (boost::equal(serverUrl, serverUrlNew))
            {
                serversMatch = true;
                break;
            }
        }
        if (!serversMatch)
        {
            // Found a new server. Add it
            ServerScoreJson ssjNew;
            ssjNew.serverUrlSet(serverUrlNew);
            serverScoresJsonArray.append(ssjNew);
            dirty_ = true;
        }
    }

    // Load the servers into the servers_ map
    servers_.clear();

    size_t numServers = serverScoresJsonArray.size();
    for (size_t j = 0; j < numServers; j++)
    {
        ServerScoreJson ssj = serverScoresJsonArray[j];
        std::string serverUrl = ssj.serverUrl();
        int serverScore = ssj.serverScore();
        unsigned long serverResponseTime = ssj.serverResponseTime();
        ServerInfo serverInfo;
        serverInfo.serverUrl = serverUrl;
        serverInfo.score = serverScore;
        serverInfo.responseTime = serverResponseTime;
        serverInfo.numResponseTimes = 0;
        servers_[serverUrl] = serverInfo;
        ABC_DebugLevel(1, "ServerCache::load() %d %d ms %s",
                       serverInfo.score, serverInfo.responseTime, serverInfo.serverUrl.c_str())

    }

    return save_nolock();
}

Status
ServerCache::save_nolock()
{
    JsonArray serverScoresJsonArray;
    ABC_Debug(1, "ServerCache::save");

    if (dirty_)
    {
        for (const auto &server: servers_)
        {
            ServerScoreJson ssj;
            ServerInfo serverInfo = server.second;
            ABC_CHECK(ssj.serverUrlSet(server.first));
            ABC_CHECK(ssj.serverScoreSet(serverInfo.score));
            ABC_CHECK(ssj.serverResponseTimeSet(serverInfo.responseTime));
            ABC_CHECK(serverScoresJsonArray.append(ssj));
            ABC_DebugLevel(1, "ServerCache::save() %d %d ms %s",
                           serverInfo.score, serverInfo.responseTime, serverInfo.serverUrl.c_str())
        }
        ABC_CHECK(serverScoresJsonArray.save(path_));
        dirty_ = false;
    }

    return Status();
}

Status
ServerCache::save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_nolock();
}

Status
ServerCache::serverScoreUp(std::string serverUrl, int changeScore)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto svr = servers_.find(serverUrl);
    if (servers_.end() != svr)
    {
        ServerInfo serverInfo = svr->second;
        serverInfo.score += changeScore;
        if (serverInfo.score > MAX_SCORE)
            serverInfo.score = MAX_SCORE;
        servers_[serverUrl] = serverInfo;
        dirty_ = true;
        ABC_Debug(1, "serverScoreUp:" + serverUrl + " " + std::to_string(serverInfo.score));
    }
    lastUpScoreTime_ = time(nullptr);
    return Status();
}

Status
ServerCache::serverScoreDown(std::string serverUrl, int changeScore)
{
    std::lock_guard<std::mutex> lock(mutex_);

    time_t currentTime = time(nullptr);

    if (currentTime - lastUpScoreTime_ > 60)
    {
        // It has been over 1 minute since we got an upvote for any server.
        // Assume the network is down and don't penalize anyone for now
        return Status();
    }

    auto svr = servers_.find(serverUrl);
    if (servers_.end() != svr)
    {
        ServerInfo serverInfo = svr->second;
        serverInfo.score -= changeScore;
        if (serverInfo.score < MIN_SCORE)
            serverInfo.score = MIN_SCORE;
        servers_[serverUrl] = serverInfo;
        dirty_ = true;
        ABC_Debug(1, "serverScoreDown:" + serverUrl + " " + std::to_string(serverInfo.score));
    }
    return Status();
}

unsigned long long
ServerCache::getCurrentTimeMilliSeconds()
{
    struct timeval tv;

    gettimeofday(&tv, NULL);

    unsigned long long millisecondsSinceEpoch =
            (unsigned long long)(tv.tv_sec) * 1000 +
            (unsigned long long)(tv.tv_usec) / 1000;

    return millisecondsSinceEpoch;
}

void
ServerCache::setResponseTime(std::string serverUrl, unsigned long long responseTimeMilliseconds)
{
    // Collects that last 10 response time values to provide an average response time.
    // This is used in weighting the score of a particular server
    auto svr = servers_.find(serverUrl);
    if (servers_.end() != svr)
    {
        ServerInfo serverInfo = svr->second;
        serverInfo.numResponseTimes++;

        unsigned long long oldtime = serverInfo.responseTime;
        unsigned long long newTime = 0;
        if (RESPONSE_TIME_UNINITIALIZED == oldtime)
        {
            newTime = responseTimeMilliseconds;
        }
        else
        {
            // Every 10th setting of response time, decrease effect of prior values by 5x
            if (serverInfo.numResponseTimes % 10 == 0)
            {
                newTime = (oldtime + (responseTimeMilliseconds * 4)) / 5;
            }
            else
            {
                newTime = (oldtime + responseTimeMilliseconds) / 2;
            }
        }
        serverInfo.responseTime = newTime;
        servers_[serverUrl] = serverInfo;
        ABC_Debug(1, "setResponseTime:" + serverUrl + " oldTime:" + std::to_string(oldtime) + " newTime:" + std::to_string(newTime));
    }
}

bool sortServersByTime(ServerInfo si1, ServerInfo si2)
{
    return si1.responseTime < si2.responseTime;
}

bool sortServersByScore(ServerInfo si1, ServerInfo si2)
{
    return si1.score > si2.score;
}

std::vector<std::string>
ServerCache::getServers(ServerType type, unsigned int numServers)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServerInfo> serverInfos;
    ServerInfo newServerInfo;

    // Get all the servers that match the type
    for (const auto &server: servers_)
    {
        if (ServerTypeStratum == type)
        {
            if (0 != server.first.compare(0, STRATUM_PREFIX_LENGTH, STRATUM_PREFIX))
                continue;
        }
        else if (ServerTypeLibbitcoin == type)
        {
            if (0 != server.first.compare(0, LIBBITCOIN_PREFIX_LENGTH, LIBBITCOIN_PREFIX))
                continue;
        }

        serverInfos.push_back(server.second);
        ServerInfo serverInfo = server.second;

        // If this is a new server, save it for use later
        if (serverInfo.responseTime == RESPONSE_TIME_UNINITIALIZED &&
                serverInfo.score == 0)
            newServerInfo = serverInfo;

        ABC_DebugLevel(1, "getServers unsorted: %d %d ms %s",
                       serverInfo.score, serverInfo.responseTime, serverInfo.serverUrl.c_str())

    }

    // Sort by score
    std::sort(serverInfos.begin(), serverInfos.end(), sortServersByScore);

    //
    // Take the top 50% of servers that have
    // 1. A score between 20 points of the highest score
    // 2. A positive score of at least 5
    // 3. A response time that is not RESPONSE_TIME_UNINITIALIZED
    //
    // Then sort those top servers by response time from lowest to highest
    //
    auto serverStart = serverInfos.begin();
    auto serverEnd = serverStart;
    int size = serverInfos.size();
    ServerInfo startServerInfo = *serverStart;
    int numServersPass = 0;
    for (auto it = serverInfos.begin(); it != serverInfos.end(); ++it)
    {
        ServerInfo serverInfo = *it;
        if (serverInfo.score < startServerInfo.score - 20)
            break;
        if (serverInfo.score <= 5)
            break;
        if (serverInfo.responseTime >= RESPONSE_TIME_UNINITIALIZED)
            break;
        serverEnd = it;

        numServersPass++;
        ABC_DebugLevel(1, "getServers sorted 1: %d %d ms %s",
                       serverInfo.score, serverInfo.responseTime, serverInfo.serverUrl.c_str())
        if (numServersPass >= numServers)
            break;
        if (numServersPass >= size / 2)
            break;
    }

    std::sort(serverStart, serverEnd, sortServersByTime);

    std::vector<std::string> servers;

    bool hasNewServer = false;
    for (auto it = serverInfos.begin(); it != serverInfos.end(); ++it)
    {
        ServerInfo serverInfo = *it;
        ABC_DebugLevel(1, "getServers sorted 2: %d %d ms %s",
                       serverInfo.score, serverInfo.responseTime, serverInfo.serverUrl.c_str())
        servers.push_back(serverInfo.serverUrl);
        if (serverInfo.responseTime == RESPONSE_TIME_UNINITIALIZED &&
            serverInfo.score == 0)
            hasNewServer = true;

            numServers--;
        if (0 == numServers)
            break;
    }

    // If this list does not have a new server in it, try to add one as we always want to give new
    // servers a try.
    if (!hasNewServer)
    {
        if (newServerInfo.serverUrl.size() > 2)
        {
            auto it = servers.begin();
            servers.insert(it, newServerInfo.serverUrl);
        }
    }
    return servers;

}
} // namespace abcd

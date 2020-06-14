/**
 * Socks5BalancerAsio : A Simple TCP Socket Balancer for balance Multi Socks5 Proxy Backend Powered by Boost.Asio
 * Copyright (C) <2020>  <Jeremie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SOCKS5BALANCERASIO_UPSTREAMPOOL_H
#define SOCKS5BALANCERASIO_UPSTREAMPOOL_H

#ifdef MSVC
#pragma once
#endif

#include <boost/asio.hpp>
#include <string>
#include <deque>
#include <memory>
#include <optional>
#include <functional>
#include <sstream>
#include <utility>
#include <random>
#include <chrono>
#include "ConfigLoader.h"
#include "TcpTest.h"
#include "ConnectTestHttps.h"

using UpstreamTimePoint = std::chrono::time_point<std::chrono::system_clock>;

UpstreamTimePoint UpstreamTimePointNow();

std::string printUpstreamTimePoint(UpstreamTimePoint p);

struct UpstreamServer : public std::enable_shared_from_this<UpstreamServer> {
    std::string host;
    uint16_t port;
    std::string name;
    size_t index;

    std::optional<UpstreamTimePoint> lastOnlineTime;
    std::optional<UpstreamTimePoint> lastConnectTime;
    bool lastConnectFailed = true;
//    std::string lastConnectCheckResult;
    bool isOffline = true;
    size_t connectCount = 0;
    bool isManualDisable = false;
    bool disable = false;

    UpstreamServer(
            size_t index,
            std::string name,
            std::string host,
            uint16_t port,
            bool disable
    ) :
            index(index),
            name(name),
            host(host),
            port(port),
            disable(disable) {}

    std::string print() {
        std::stringstream ss;
        ss << "["
           << "index:" << index << ", "
           << "name:" << name << ", "
           << "host:" << host << ", "
           << "port:" << port << ", "
           << "]";
        return ss.str();
    }
};

using UpstreamServerRef = std::shared_ptr<UpstreamServer>;

class UpstreamPool : public std::enable_shared_from_this<UpstreamPool> {
    boost::asio::executor ex;

    std::deque<UpstreamServerRef> _pool;
    size_t lastUseUpstreamIndex = 0;

    std::shared_ptr<ConfigLoader> _configLoader;

    std::default_random_engine randomGenerator;

    UpstreamTimePoint lastChangeUpstreamTime;

    std::shared_ptr<TcpTest> tcpTest;
    std::shared_ptr<ConnectTestHttps> connectTestHttps;

public:
    UpstreamPool(boost::asio::executor ex,
                 std::shared_ptr<TcpTest> tcpTest,
                 std::shared_ptr<ConnectTestHttps> connectTestHttps)
            : ex(ex),
              tcpTest(std::move(tcpTest)),
              connectTestHttps(std::move(connectTestHttps)) {}

    const std::deque<UpstreamServerRef> &pool() {
        return _pool;
    }

    void setConfig(std::shared_ptr<ConfigLoader> configLoader) {
        _configLoader = std::move(configLoader);
        const auto &c = _configLoader->config.upstream;
        _pool.clear();
        for (size_t i = 0; i != c.size(); ++i) {
            auto &r = c[i];
            UpstreamServerRef u = std::make_shared<UpstreamServer>(
                    i, r.name, r.host, r.port, r.disable
            );
            _pool.push_back(u);
        }
    }

    void forceSetLastUseUpstreamIndex(int i) {
        if (i >= 0 && i < _pool.size()) {
            lastUseUpstreamIndex = i;
        }
    }

    size_t getLastUseUpstreamIndex() {
        return lastUseUpstreamIndex;
    }

protected:
    bool checkServer(const UpstreamServerRef &u) const {
        return u
               && u->lastConnectTime.has_value()
               && u->lastOnlineTime.has_value()
               && !u->lastConnectFailed
               && !u->isOffline
               && !u->isManualDisable;
    }

    auto getNextServer() -> UpstreamServerRef {
        const auto _lastUseUpstreamIndex = lastUseUpstreamIndex;
        while (true) {
            ++lastUseUpstreamIndex;
            if (lastUseUpstreamIndex >= _pool.size()) {
                lastUseUpstreamIndex = 0;
            }
            if (checkServer(_pool[lastUseUpstreamIndex])) {
                return _pool[lastUseUpstreamIndex]->shared_from_this();
            }
            if (_lastUseUpstreamIndex == lastUseUpstreamIndex) {
                // cannot find
                return UpstreamServerRef{};
            }
        }
    }

    auto tryGetLastServer() -> UpstreamServerRef {
        const auto _lastUseUpstreamIndex = lastUseUpstreamIndex;
        while (true) {
            if (lastUseUpstreamIndex >= _pool.size()) {
                lastUseUpstreamIndex = 0;
            }
            if (checkServer(_pool[lastUseUpstreamIndex])) {
                return _pool[lastUseUpstreamIndex]->shared_from_this();
            }
            ++lastUseUpstreamIndex;
            if (lastUseUpstreamIndex >= _pool.size()) {
                lastUseUpstreamIndex = 0;
            }
            if (_lastUseUpstreamIndex == lastUseUpstreamIndex) {
                // cannot find
                return UpstreamServerRef{};
            }
        }
    }

    auto filterValidServer() -> std::vector<UpstreamServerRef> {
        std::vector<UpstreamServerRef> r;
        for (auto &a:_pool) {
            if (checkServer(a)) {
                r.emplace_back(a->shared_from_this());
            }
        }
        return r;
    }

public:
    auto getServerBasedOnAddress() -> UpstreamServerRef {
        const auto upstreamSelectRule = _configLoader->config.upstreamSelectRule;

        UpstreamServerRef s{};
        switch (upstreamSelectRule) {
            case RuleEnum::loop:
                s = getNextServer();
                std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
                return s;
            case RuleEnum::one_by_one:
                s = tryGetLastServer();
                std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
                return s;
            case RuleEnum::change_by_time: {
                UpstreamTimePoint t;
                const auto &d = _configLoader->config.serverChangeTime;
                if ((t - lastChangeUpstreamTime) > d) {
                    s = getNextServer();
                    lastChangeUpstreamTime = UpstreamTimePointNow();
                } else {
                    s = tryGetLastServer();
                }
                std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
                return s;
            }
            case RuleEnum::random:
            default: {
                auto rs = filterValidServer();
                if (!rs.empty()) {
                    std::uniform_int_distribution<size_t> distribution(0, rs.size() - 1);
                    size_t i = distribution(randomGenerator);
                    s = rs[i];
                } else {
                    s.reset();
                }
                std::cout << "getServerBasedOnAddress:" << (s ? s->print() : "nullptr") << "\n";
                return s;
            }
        }
    }


private:
    using CheckerTimerType = boost::asio::steady_timer;
    using CheckerTimerPeriodType = boost::asio::chrono::microseconds;
    std::unique_ptr<CheckerTimerType> tcpCheckerTimer;
    std::unique_ptr<CheckerTimerType> connectCheckerTimer;

public:
    void endCheckTimer() {
        if (tcpCheckerTimer) {
            tcpCheckerTimer->cancel();
            tcpCheckerTimer.reset();
        }
        if (connectCheckerTimer) {
            connectCheckerTimer->cancel();
            connectCheckerTimer.reset();
        }
    }

    void startCheckTimer() {
        if (tcpCheckerTimer && connectCheckerTimer) {
            return;
        }
        endCheckTimer();

        tcpCheckerTimer = std::make_unique<CheckerTimerType>(ex, _configLoader->config.tcpCheckStart);
        do_tcpCheckerTimer();

        connectCheckerTimer = std::make_unique<CheckerTimerType>(ex, _configLoader->config.connectCheckStart);
        do_connectCheckerTimer();

    }

    std::string print() {
        std::stringstream ss;
        for (size_t i = 0; i != _pool.size(); ++i) {
            const auto &r = _pool[i];
            ss << r->index << ":[" << "\n"
               << "\t" << "name :" << r->name << "\n"
               << "\t" << "host :" << r->host << "\n"
               << "\t" << "port :" << r->port << "\n"
               << "\t" << "isOffline :" << r->isOffline << "\n"
               << "\t" << "lastConnectFailed :" << r->lastConnectFailed << "\n"
               << "\t" << "lastOnlineTime :" << (
                       r->lastOnlineTime.has_value() ?
                       printUpstreamTimePoint(r->lastOnlineTime.value()) : "empty") << "\n"
               << "\t" << "lastConnectTime :" << (
                       r->lastConnectTime.has_value() ?
                       printUpstreamTimePoint(r->lastConnectTime.value()) : "empty") << "\n"
               << "]" << "\n";
        }
        return ss.str();
    }

private:
    void do_tcpCheckerTimer() {
        auto c = [this, self = shared_from_this()](const boost::system::error_code &e) {
            if (e) {
                return;
            }
            std::cout << "do_tcpCheckerTimer()" << std::endl;
            std::cout << print() << std::endl;

            for (auto &a: _pool) {
                auto t = tcpTest->createTest(a->host, std::to_string(a->port));
                t->run([t, a]() {
                           // on ok
                           if (a->isOffline) {
                               a->lastConnectFailed = false;
                           }
                           a->lastOnlineTime = UpstreamTimePointNow();
                           a->isOffline = false;
                       },
                       [t, a](std::string reason) {
                           // ok error
                           a->isOffline = true;
                       });
            }

            tcpCheckerTimer->expires_at(tcpCheckerTimer->expiry() + _configLoader->config.tcpCheckPeriod);
            do_tcpCheckerTimer();
        };
        tcpCheckerTimer->async_wait(c);
    }

    void do_connectCheckerTimer() {
        auto c = [this, self = shared_from_this()](const boost::system::error_code &e) {
            if (e) {
                return;
            }
            std::cout << "do_connectCheckerTimer()" << std::endl;

            for (const auto &a: _pool) {
                auto t = connectTestHttps->createTest(
                        a->host,
                        std::to_string(a->port),
                        _configLoader->config.testRemoteHost,
                        _configLoader->config.testRemotePort,
                        R"(\)"
                );
                t->run([t, a](ConnectTestHttpsSession::SuccessfulInfo info) {
                           // on ok
                           // std::cout << "SuccessfulInfo:" << info << std::endl;
                           a->lastConnectTime = UpstreamTimePointNow();
                           a->lastConnectFailed = false;
                       },
                       [t, a](std::string reason) {
                           // ok error
                           a->lastConnectFailed = true;
                       });
            }

            connectCheckerTimer->expires_at(tcpCheckerTimer->expiry() + _configLoader->config.connectCheckPeriod);
            do_connectCheckerTimer();
        };
        connectCheckerTimer->async_wait(c);
    }

};


#endif //SOCKS5BALANCERASIO_UPSTREAMPOOL_H

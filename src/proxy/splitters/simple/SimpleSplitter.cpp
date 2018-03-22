/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <inttypes.h>


#include "core/Config.h"
#include "core/Controller.h"
#include "log/Log.h"
#include "net/Url.h"
#include "Platform.h"
#include "proxy/Counters.h"
#include "proxy/events/CloseEvent.h"
#include "proxy/events/LoginEvent.h"
#include "proxy/events/SubmitEvent.h"
#include "proxy/Miner.h"
#include "proxy/splitters/simple/SimpleMapper.h"
#include "proxy/splitters/simple/SimpleSplitter.h"


#define LABEL(x) " \x1B[01;30m" x ":\x1B[0m "


static bool compare(Url *i, Url *j) {
  return *i == *j;
}


SimpleSplitter::SimpleSplitter(xmrig::Controller *controller) : Splitter(controller),
    m_reuseTimeout(controller->config()->reuseTimeout()),
    m_sequence(0)
{
}


SimpleSplitter::~SimpleSplitter()
{
}


uint64_t SimpleSplitter::activeUpstreams() const
{
    uint64_t active = 0;

    for (auto const &kv : m_upstreams) {
        if (kv.second->isActive()) {
           active++;
        }
    }

    return active;
}


void SimpleSplitter::connect()
{
}


void SimpleSplitter::gc()
{
}


void SimpleSplitter::printConnections()
{
    const uint64_t active = activeUpstreams();
    const size_t idle     = m_idles.size();
    const size_t error    = m_upstreams.size() - active - idle;

    if (m_controller->config()->colors()) {
        LOG_INFO("\x1B[01;32m* \x1B[01;37mupstreams\x1B[0m" LABEL("active") "%s%" PRIu64 "\x1B[0m" LABEL("sleep") "\x1B[01;37m%zu\x1B[0m" LABEL("error") "%s%zu\x1B[0m" LABEL("total") "\x1B[01;37m%zu",
                 active ? "\x1B[01;32m" : "\x1B[01;31m", active, idle, error ? "\x1B[01;31m" : "\x1B[01;37m", error, m_upstreams.size());

        LOG_INFO("\x1B[01;32m* \x1B[01;37mminers   \x1B[0m" LABEL("active") "%s%" PRIu64 "\x1B[0m" LABEL("max") "\x1B[01;37m%" PRIu64 "\x1B[0m",
                 Counters::miners() ? "\x1B[01;32m" : "\x1B[01;31m", Counters::miners(), Counters::maxMiners());
    }
    else {
        LOG_INFO("* upstreams: active %" PRIu64 " sleep %zu error %zu total %zu",
                 active, idle, error, m_upstreams.size());

        LOG_INFO("* miners:    active %" PRIu64 " max %" PRIu64,
                 Counters::miners(), Counters::maxMiners());
    }
}


void SimpleSplitter::tick(uint64_t ticks)
{
    const uint64_t now = uv_now(uv_default_loop());
    std::vector<SimpleMapper *> released;

    for (auto const &kv : m_upstreams) {
        if (kv.second->idleTime() > m_reuseTimeout) {
            released.push_back(kv.second);
            continue;
        }

        kv.second->tick(ticks, now);
    }

    if (released.empty()) {
        return;
    }

    for (SimpleMapper *mapper : released) {
        removeIdle(mapper->id());
        removeUpstream(mapper->id());

        delete mapper;
    }
}


#ifdef APP_DEVEL
void SimpleSplitter::printState()
{
}
#endif


void SimpleSplitter::onConfigChanged(xmrig::Config *config, xmrig::Config *previousConfig)
{
    m_reuseTimeout = config->reuseTimeout();

    const std::vector<Url*> &pools         = config->pools();
    const std::vector<Url*> &previousPools = previousConfig->pools();

    if (pools.size() != previousPools.size() || !std::equal(pools.begin(), pools.end(), previousPools.begin(), compare)) {
        for (auto const &kv : m_upstreams) {
            kv.second->reload(pools);
        }
    }
}


void SimpleSplitter::onEvent(IEvent *event)
{
    switch (event->type())
    {
    case IEvent::CloseType:
        remove(static_cast<CloseEvent*>(event)->miner());
        break;

    case IEvent::LoginType:
        login(static_cast<LoginEvent*>(event));
        break;

    case IEvent::SubmitType:
        submit(static_cast<SubmitEvent*>(event));
        break;

    default:
        break;
    }
}


void SimpleSplitter::login(LoginEvent *event)
{
    if (!m_idles.empty()) {
        for (auto const &kv : m_idles) {
            if (kv.second->isReusable()) {
                removeIdle(kv.first);
                kv.second->reuse(event->miner(), event->request);

                return;
            }
        }
    }

    SimpleMapper *mapper = new SimpleMapper(m_sequence++, m_controller);
    m_upstreams[mapper->id()] = mapper;

    mapper->add(event->miner(), event->request);
}


void SimpleSplitter::remove(Miner *miner)
{
    const ssize_t id = miner->mapperId();

    if (id < 0 || m_upstreams.count(id) == 0) {
        return;
    }

    SimpleMapper *mapper = m_upstreams[id];
    mapper->remove(miner);

    if (m_reuseTimeout == 0) {
        removeUpstream(id);

        delete mapper;
    }
    else {
        m_idles[id] = mapper;
    }
}


void SimpleSplitter::removeIdle(uint64_t id)
{
    auto it = m_idles.find(id);
    if (it != m_idles.end()) {
        m_idles.erase(it);
    }
}


void SimpleSplitter::removeUpstream(uint64_t id)
{
    auto it = m_upstreams.find(id);
    if (it != m_upstreams.end()) {
        m_upstreams.erase(it);
    }
}


void SimpleSplitter::submit(SubmitEvent *event)
{
    SimpleMapper *mapper = m_upstreams[event->miner()->mapperId()];
    if (mapper) {
        mapper->submit(event);
    }
}
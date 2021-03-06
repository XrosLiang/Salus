/*
 * Copyright 2019 Peifeng Yu <peifeng@umich.edu>
 * 
 * This file is part of Salus
 * (see https://github.com/SymbioticLab/Salus).
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "execution/scheduler/basescheduler.h"

#include "execution/engine/taskexecutor.h"
#include "execution/scheduler/sessionitem.h"
#include "execution/engine/resourcecontext.h"
#include "execution/operationtask.h"
#include "execution/scheduler/operationitem.h"
#include "platform/logging.h"
#include "utils/debugging.h"
#include "utils/envutils.h"
#include "utils/macros.h"
#include "utils/threadutils.h"

using std::chrono::duration_cast;
using FpMS = std::chrono::duration<double, std::chrono::milliseconds::period>;
using namespace std::chrono_literals;
using namespace date;
using namespace salus;

namespace {
bool useGPU()
{
    auto use = sstl::fromEnvVar("SALUS_SCHED_USE_GPU", true);
    VLOG(2) << "Scheduling using: " << (use ? "GPU,CPU" : "CPU");
    return use;
}
} // namespace

SchedulerRegistary &SchedulerRegistary::instance()
{
    static SchedulerRegistary registary;
    return registary;
}

SchedulerRegistary::SchedulerRegistary() = default;

SchedulerRegistary::~SchedulerRegistary() = default;

SchedulerRegistary::Register::Register(std::string_view name, SchedulerFactory factory)
{
    auto &registary = SchedulerRegistary::instance();
    auto guard = sstl::with_guard(registary.m_mu);
    auto [iter, inserted] = registary.m_schedulers.try_emplace(std::string(name), std::move(factory));
    UNUSED(iter);
    if (!inserted) {
        LOG(FATAL) << "Duplicate registration of execution scheduler under name " << name;
    }
}

std::unique_ptr<BaseScheduler> SchedulerRegistary::create(std::string_view name,
                                                          TaskExecutor &engine) const
{
    auto guard = sstl::with_guard(m_mu);
    auto iter = m_schedulers.find(name);
    if (iter == m_schedulers.end()) {
        LOG(ERROR) << "No scheduler registered under name: " << name;
        return nullptr;
    }
    return iter->second.factory(engine);
}

BaseScheduler::BaseScheduler(TaskExecutor &engine)
    : m_taskExec(engine)
{
}

BaseScheduler::~BaseScheduler() = default;

void BaseScheduler::notifyPreSchedulingIteration(const SessionList &sessions,
                                                 const SessionChangeSet &changeset,
                                                 sstl::not_null<CandidateList *> candidates)
{
    UNUSED(sessions);
    UNUSED(changeset);
    UNUSED(candidates);

    auto g = sstl::with_guard(m_muRes);
    m_missingRes.clear();
}

bool BaseScheduler::maybePreAllocateFor(OperationItem &opItem, const DeviceSpec &spec)
{
    auto item = opItem.sess.lock();
    if (!item) {
        return false;
    }

    auto usage = opItem.op->estimatedUsage(spec);

    Resources missing;
    auto rctx = m_taskExec.makeResourceContext(item, opItem.op->graphId(), spec, usage, &missing);
    if (!rctx) {
        // Failed to pre allocate resources
        auto g = sstl::with_guard(m_muRes);
        m_missingRes.emplace(&opItem, std::move(missing));
        return false;
    }

    auto ticket = rctx->ticket();
    if (!opItem.op->prepare(std::move(rctx))) {
        return false;
    }

    auto g = sstl::with_guard(item->tickets_mu);
    item->tickets.insert(ticket);
    return true;
}

bool BaseScheduler::insufficientMemory(const DeviceSpec &spec)
{
    auto g = sstl::with_guard(m_muRes);

    if (m_missingRes.empty()) {
        return false;
    }

    // we need paging if all not scheduled opItems in this iteration
    // are missing memory resource on the device
    for (const auto &[pOpItem, missing] : m_missingRes) {
        UNUSED(pOpItem);
        for (const auto &[tag, amount] : missing) {
            UNUSED(amount);
            auto insufficientMemory = tag.type == ResourceType::MEMORY && tag.device == spec;
            if (!insufficientMemory) {
                return false;
            }
        }
    }
    return true;
}

std::string BaseScheduler::debugString(const PSessionItem &item) const
{
    UNUSED(item);
    return {};
}

std::string BaseScheduler::debugString() const
{
    return name();
}

POpItem BaseScheduler::submitTask(POpItem &&opItem)
{
    auto item = opItem->sess.lock();
    if (!item) {
        // session already deleted, discard this task sliently
        return nullptr;
    }

    VLOG(3) << "Scheduling opItem in session " << item->sessHandle << ": " << opItem->op;

    LogOpTracing() << "OpItem Event " << opItem->op << " event: inspected";
    bool scheduled = false;
    DeviceSpec spec{};
    for (auto dt : opItem->op->supportedDeviceTypes()) {
        if (dt == DeviceType::GPU && !useGPU()) {
            continue;
        }
        spec.type = dt;
        spec.id = 0;
        if (maybePreAllocateFor(*opItem, spec)) {
            VLOG(3) << "Task scheduled on " << spec;
            scheduled = true;
            break;
        }
    }

    LogOpTracing() << "OpItem Event " << opItem->op << " event: prealloced";

    // Send to thread pool
    if (scheduled) {
        opItem = m_taskExec.runTask(std::move(opItem));
    } else {
        VLOG(2) << "Failed to schedule opItem in session " << item->sessHandle << ": "
                << opItem->op->DebugString();
    }
    return opItem;
}

size_t BaseScheduler::submitAllTaskFromQueue(const PSessionItem &item)
{
    auto &queue = item->bgQueue;
    size_t scheduled = 0;

    if (queue.empty()) {
        return scheduled;
    }

    // Exam if queue front has been waiting for a long time
    if (item->holWaiting > m_taskExec.schedulingParam().maxHolWaiting) {
        VLOG(2) << "In session " << item->sessHandle << ": HOL waiting exceeds maximum: " << item->holWaiting
                << " (max=" << m_taskExec.schedulingParam().maxHolWaiting << ")";
        // Only try to schedule head in this case
        auto &head = queue.front();
        head = submitTask(std::move(head));
        if (!head) {
            queue.pop_front();
            scheduled += 1;
        }
    } else {
        auto size = queue.size();
        SessionItem::UnsafeQueue stage;
        stage.swap(queue);

#if defined(SALUS_ENABLE_PARALLEL_SCHED)
        // Do all schedule in queue in parallel
        std::vector<std::future<std::shared_ptr<OperationItem>>> futures;
        futures.reserve(stage.size());
        for (auto &opItem : stage) {
            auto fu = m_taskExec.pool().post([opItem = std::move(opItem), this]() mutable {
                DCHECK(opItem);
                return submitTask(std::move(opItem));
            });
            futures.emplace_back(std::move(fu));
        }

        for (auto &fu : futures) {
            auto poi = fu.get();
            if (poi) {
                queue.emplace_back(std::move(poi));
            }
        }
#else
        for (auto &opItem : stage) {
            auto poi = submitTask(std::move(opItem));
            if (poi) {
                queue.emplace_back(std::move(poi));
            }
        }
#endif
        VLOG(2) << "All opItem in session " << item->sessHandle << " examined";

        scheduled = size - queue.size();
    }

    // update queue head waiting
    if (queue.empty()) {
        item->queueHeadHash = 0;
        item->holWaiting = 0;
    } else if (queue.front()->hash() == item->queueHeadHash) {
        item->holWaiting += scheduled;
    } else {
        item->queueHeadHash = queue.front()->hash();
        item->holWaiting = 0;
    }

    return scheduled;
}

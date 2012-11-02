/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsthreadpool_h___
#define jsthreadpool_h___

#if defined(JS_THREADSAFE) && defined(JS_ION)
# define JS_THREADSAFE_ION
#endif

#include <stddef.h>
#include "mozilla/StandardInteger.h"
#include "prtypes.h"
#include "js/Vector.h"
#include "jsalloc.h"
#include "prlock.h"
#include "prcvar.h"

struct JSContext;
struct JSRuntime;
struct JSCompartment;
struct JSScript;

namespace js {

class ThreadPoolWorker;

typedef void (*TaskFun)(void *userdata, size_t workerId, uintptr_t stackLimit);

class TaskExecutor
{
public:
    virtual void executeFromWorker(size_t workerId, uintptr_t stackLimit) = 0;
};

/* ThreadPool used for Rivertrail and parallel compilation. */
class ThreadPool
{
private:
    friend struct ThreadPoolWorker;

    // Note:
    //
    // All fields here should only be modified during start-up or
    // while holding the ThreadPool lock.

    JSRuntime *runtime_;

    size_t nextId_;

    /* Array of worker threads; lazilly spawned */
    js::Vector<ThreadPoolWorker*, 8, SystemAllocPolicy> workers_;

    void terminateWorkers();

public:
    ThreadPool(JSRuntime *rt);
    ~ThreadPool();

    bool init();

    size_t numWorkers() { return workers_.length(); }

    // Submits a job that will execute once by some worker.
    bool submitOne(TaskExecutor *executor);

    // Submits a job that will be executed by all workers.
    bool submitAll(TaskExecutor *executor);

    bool terminate();
};

}



#endif

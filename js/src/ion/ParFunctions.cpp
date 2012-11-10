/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ParFunctions.h"
#include "jsinterp.h"
#include "jsinterpinlines.h"
#include "vm/forkjoininlines.h"

namespace js {
namespace ion {

// Load the current thread context.
ForkJoinSlice *ParForkJoinSlice() {
    ForkJoinSlice *context = js::ForkJoinSlice::current();
    return context;
}

// ParallelNewGCThing() is called in place of NewGCThing() when
// executing parallel code.  It uses TLS to acquire the ArenaLists for
// the current thread and allocates from there.
//
// NB--Right now, this takes a JSContext *, but this is not
// thread-local and we basically avoid touching it as much as
// possible!  It's just that it's hard to avoid in the way that
// IonMonkey is setup, near as I can tell right now.
JSObject *
ParNewGCThing(ForkJoinSlice *threadContext, JSCompartment *compartment,
              gc::AllocKind allocKind, uint32_t thingSize) {
    gc::ArenaLists *arenaLists = threadContext->arenaLists;
    void *t = arenaLists->parallelAllocate(compartment, allocKind, thingSize);
    return static_cast<JSObject *>(t);
}

// Check that the object was created by the current thread
// (and hence is writable).
bool ParWriteGuard(ForkJoinSlice *context, JSObject *object) {
    gc::ArenaLists *arenaLists = context->arenaLists;
    return arenaLists->containsArena(context->runtime(),
                                     object->arenaHeader());
}

// This isn't really the right place for this, it's could be a more
// general facility.
void ParBailout(uint32_t id) {
    fprintf(stderr, "TRACE: id=%-10u\n", id);
}

bool ParCheckInterrupt(ForkJoinSlice *context) {
    bool result = context->check();
    if (!result) {
        fprintf(stderr, "Check Interrupt failed!\n");
    }
    return result;
}

}
}

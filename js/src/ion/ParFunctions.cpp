/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsinterp.h"
#include "ParFunctions.h"

#include "jsinterpinlines.h"
#include "vm/forkjoininlines.h"
#include "jscompartmentinlines.h"
#include "jsarrayinlines.h"

namespace js {
namespace ion {

// Load the current thread context.
ForkJoinSlice *ParForkJoinSlice() {
    return js::ForkJoinSlice::current();
}

// ParNewGCThing() is called in place of NewGCThing() when executing
// parallel code.  It uses the ArenaLists for the current thread and
// allocates from there.
JSObject *
ParNewGCThing(ForkJoinSlice *slice, gc::AllocKind allocKind, uint32_t thingSize) {
    void *t = slice->allocator->parallelNewGCThing(allocKind, thingSize);
    return static_cast<JSObject *>(t);
}

// Check that the object was created by the current thread
// (and hence is writable).
bool ParWriteGuard(ForkJoinSlice *slice, JSObject *object) {
    return slice->allocator->arenas.containsArena(slice->runtime(),
                                                  object->arenaHeader());
}

void Trace(uint32_t bblock, uint32_t lir, uint32_t execModeInt,
           const char *opcode) {
    /*
       If you set IONFLAGS=trace, this function will be invoked before every LIR.

       You can either modify it to do whatever you like, or use gdb scripting.
       For example:

       break ParTrace
       commands
       continue
       exit
     */
    fprintf(stderr, "Block %3u / LIR %3u / Mode %u / Opcode %s\n",
            bblock, lir, execModeInt, opcode);
}


bool ParCheckInterrupt(ForkJoinSlice *slice) {
    return slice->check();
}

bool ParExtendArray(ParExtendArrayArgs *args) {
    // It is awkward to have the MIR pass the current slice in, so
    // just fetch it from TLS.  Extending the array is kind of the
    // slow path anyhow as it reallocates the elements vector.
    ForkJoinSlice *slice = js::ForkJoinSlice::current();
    return (args->object->parExtendDenseArray(slice->allocator,
                                              &args->value, 1) == JSObject::ED_OK);
}

} /* namespace ion */
} /* namespace js */

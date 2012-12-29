/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ParallelArray_h__
#define ParallelArray_h__

#include "jsapi.h"
#include "jscntxt.h"
#include "jsobj.h"
#include "vm/ThreadPool.h"
#include "vm/ForkJoin.h"
#include "ion/Ion.h"

namespace js {
namespace parallel {

enum ExecutionStatus {
    // Parallel or seq execution terminated in a fatal way, operation failed
    ExecutionFatal,

    // Parallel exec failed and so we fell back to sequential
    ExecutionSequential,

    // Parallel exec was successful after some number of bailouts
    ExecutionParallel
};

bool Do(JSContext *cx, CallArgs &args);

enum SpewChannel {
    SpewOps,
    SpewCompile,
    SpewBailouts,
    NumSpewChannels
};

#ifdef DEBUG

bool SpewEnabled(SpewChannel channel);
void Spew(SpewChannel channel, const char *fmt, ...);
void SpewBeginOp(JSContext *cx, const char *name);
void SpewBailout(uint32_t count);
ExecutionStatus SpewEndOp(ExecutionStatus status);
void SpewBeginCompile(HandleFunction fun);
ion::MethodStatus SpewEndCompile(ion::MethodStatus status);
void SpewMIR(ion::MDefinition *mir, const char *fmt, ...);
void SpewBailoutIR(const char *lir, const char *mir, JSScript *script, jsbytecode *pc);

#else

static inline bool SpewEnabled(SpewChannel channel) { return false; }
static inline void Spew(SpewChannel channel, const char *fmt, ...) { }
static inline void SpewBeginOp(JSContext *cx, const char *name) { }
static inline void SpewBailout(uint32_t count) {}
static inline ExecutionStatus SpewEndOp(ExecutionStatus status) { return status; }
static inline void SpewBeginCompile(HandleFunction fun) { }
static inline ion::MethodStatus SpewEndCompile(ion::MethodStatus status) { return status; }
static inline void SpewMIR(ion::MDefinition *mir, const char *fmt, ...) { }
static inline void SpewBailoutIR(const char *lir, const char *mir,
                                 JSScript *script, jsbytecode *pc) { }

#endif // DEBUG

} // namespace parallel

class ParallelArrayObject : public JSObject
{
    static Class protoClass;
    static JSFunctionSpec methods[];
    static const uint32_t NumFixedSlots = 4;
    static const uint32_t NumCtors = 4;
    static FixedHeapPtr<PropertyName> ctorNames[NumCtors];

    static bool initProps(JSContext *cx, HandleObject obj);

  public:
    static Class class_;

    static JSBool construct(JSContext *cx, unsigned argc, Value *vp);
    static JSBool constructHelper(JSContext *cx, MutableHandleFunction ctor, CallArgs &args);

    // Creates a new ParallelArray instance with the correct number of slots
    // and so forth.
    //
    // NOTE: This object will NOT have the correct type object!  It is
    // up to you the caller to adjust the type object appropriately
    // before releasing the object into the wild.
    static JSObject *newInstance(JSContext *cx);

    // Get the constructor function for argc number of arguments.
    static JSFunction *getConstructor(JSContext *cx, unsigned argc);

    static JSObject *initClass(JSContext *cx, HandleObject obj);
    static bool is(const Value &v);
};

} // namespace js

extern JSObject *
js_InitParallelArrayClass(JSContext *cx, js::HandleObject obj);

#endif // ParallelArray_h__

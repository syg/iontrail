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
#include "vm/threadpool.h"
#include "vm/forkjoin.h"

namespace js {
namespace parallel {

enum ExecutionStatus {
    // For some reason not eligible for parallel exec, use seq fallback
    ExecutionDisqualified = 0,

    // Parallel execution went off the safe path, use seq fallback
    ExecutionBailout,

    // Parallel or seq execution terminated in a fatal way, operation failed
    ExecutionFatal,

    // Parallel or seq op was successful
    ExecutionSucceeded
};

ExecutionStatus BuildArray(JSContext *cx, CallArgs args);

} // namespace parallel

class ParallelArrayObject : public JSObject
{
    static Class protoClass;
    static JSFunctionSpec methods[];
    static FixedHeapPtr<PropertyName> ctorNames[3];

    static JSBool construct(JSContext *cx, unsigned argc, Value *vp);

  public:
    static Class class_;

    static JSObject *initClass(JSContext *cx, HandleObject obj);
    static bool is(const Value &v);
};

} // namespace js

extern JSObject *
js_InitParallelArrayClass(JSContext *cx, js::HandleObject obj);


#endif // ParallelArray_h__

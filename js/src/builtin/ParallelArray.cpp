/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/ParallelArray.h"

#include "jsapi.h"
#include "jsobj.h"
#include "jsarray.h"

#include "vm/GlobalObject.h"

#include "jstaskset.h"
#include "jsthreadpool.h"

#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsarrayinlines.h"
#include "jsthreadpoolinlines.h"

#include "ion/Ion.h"
#include "ion/IonCompartment.h"
#include "ion/ParallelArrayAnalysis.h"

using namespace js;
using namespace js::parallel;
using namespace js::ion;

//
// Debug spew
//

enum SpewChannel {
    SpewOps,
    SpewTypes,
    NumSpewChannels
};

#ifdef DEBUG

static bool
SpewColorable()
{
    // Only spew colors on xterm-color to not screw up emacs.
    static bool colorable = false;
    static bool checked = false;
    if (!checked) {
        checked = true;
        const char *env = getenv("TERM");
        if (!env)
            return false;
        if (strcmp(env, "xterm-color") == 0 || strcmp(env, "xterm-256color") == 0)
            colorable = true;
    }
    return colorable;
}

static inline const char *
SpewColorReset()
{
    if (!SpewColorable())
        return "";
    return "\x1b[0m";
}

static inline const char *
SpewColorRaw(const char *colorCode) {
    if (!SpewColorable())
        return "";
    return colorCode;
}

static inline const char *
SpewColorRed() { return SpewColorRaw("\x1b[31m"); }
static inline const char *
SpewColorGreen() { return SpewColorRaw("\x1b[32m"); }
static inline const char *
SpewColorYellow() { return SpewColorRaw("\x1b[33m"); }

static bool
IsSpewActive(SpewChannel channel)
{
    // Note: in addition, setting PAFLAGS=trace will cause
    // special instructions to be written into the generated
    // code that look like:
    //
    // mov 0xDEADBEEF, eax
    // mov N, eax

    static bool active[NumSpewChannels];
    static bool checked = false;
    if (!checked) {
        checked = true;
        PodArrayZero(active);
        const char *env = getenv("PAFLAGS");
        if (!env)
            return false;
        if (strstr(env, "ops"))
            active[SpewOps] = true;
        if (strstr(env, "types"))
            active[SpewTypes] = true;
        if (strstr(env, "full")) {
            for (uint32_t i = 0; i < NumSpewChannels; i++)
                active[i] = true;
        }
    }
    return active[channel];
}

static void
Spew(JSContext *cx, SpewChannel channel, const char *fmt, ...)
{
    if (!IsSpewActive(channel))
        return;

    jsbytecode *pc;
    JSScript *script = cx->stack.currentScript(&pc);
    if (!script || !pc)
        return;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[ParallelArray] %s:%u: ", script->filename, PCToLineNumber(script, pc));
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
}

static const char *
ExecutionStatusToString(ExecutionStatus status)
{
    switch (status) {
      case ExecutionDisqualified:
        return "disqualified";
      case ExecutionBailout:
        return "bailout";
      case ExecutionFatal:
        return "fatal";
      case ExecutionSucceeded:
        return "success";
    }
    return "(unknown status)";
}

static void
SpewExecution(JSContext *cx, const char *op, ExecutionStatus status)
{
    const char *statusColor;
    switch (status) {
      case ExecutionDisqualified:
      case ExecutionFatal:
        statusColor = SpewColorRed();
        break;
      case ExecutionBailout:
        statusColor = SpewColorYellow();
        break;
      case ExecutionSucceeded:
        statusColor = SpewColorGreen();
        break;
    }
    Spew(cx, SpewOps, "%s: %s%s%s", op, statusColor,
         ExecutionStatusToString(status), SpewColorReset());
}

#else

static bool IsSpewActive(SpewChannel channel) { return false; }
static void Spew(JSContext *cx, SpewChannel channel, const char *fmt, ...) {}
static void SpewExecution(JSContext *cx, const char *op, const ExecutionMode &mode,
                          ExecutionStatus status) {}

#endif

static ExecutionStatus
ToExecutionStatus(JSContext *cx, const char *opName, ParallelResult pr)
{
    ExecutionStatus status;
    switch (pr) {
      case TP_SUCCESS:
        status = ExecutionSucceeded;
        break;

      case TP_RETRY_SEQUENTIALLY:
        status = ExecutionBailout;
        break;

      case TP_FATAL:
        status = ExecutionFatal;
        break;
    }

    SpewExecution(cx, opName, status);
    return status;
}

// Can only enter callees with a valid IonScript.
template <uint32_t argc>
class FastestIonInvoke
{
    EnterIonCode enter_;
    void *jitcode_;
    void *calleeToken_;
    Value argv_[argc + 2];

  public:
    Value *args;

    FastestIonInvoke(JSContext *cx, HandleObject fun)
      : args(argv_ + 2)
    {
        // Set 'callee' and 'this'.
        JS_ASSERT(fun->isFunction());
        RootedFunction callee(cx, fun->toFunction());
        argv_[0] = ObjectValue(*callee);
        argv_[1] = UndefinedValue();

        // Find JIT code pointer.
        IonScript *ion = callee->script()->ionScript(COMPILE_MODE_PAR);
        IonCode *code = ion->method();
        jitcode_ = code->raw();
        enter_ = cx->compartment->ionCompartment()->enterJITInfallible();
        calleeToken_ = CalleeToToken(callee);
    }

    bool invoke() {
        Value result;
        enter_(jitcode_, argc + 1, argv_ + 1, NULL, calleeToken_, &result);
        return !result.isMagic();
    }
};

class AutoEnterParallelSection
{
  private:
    JSContext *cx_;
    uint8 *prevIonTop_;
    uintptr_t ionStackLimit_;
    types::AutoEnterTypeInference enter_;

  public:
    AutoEnterParallelSection(JSContext *cx)
      : cx_(cx)
      , prevIonTop_(cx->runtime->ionTop)
      , ionStackLimit_(cx->runtime->ionStackLimit)
      , enter_(cx)
    {
        // Temporarily suspend native stack limit while par code
        // is executing, since it doesn't apply to the par threads:
#       if JS_STACK_GROWTH_DIRECTION > 0
        cx->runtime->ionStackLimit = UINTPTR_MAX;
#       else
        cx->runtime->ionStackLimit = 0;
#       endif

        cx->runtime->gcHelperThread.waitBackgroundSweepEnd();
    }

    ~AutoEnterParallelSection() {
        cx_->runtime->ionStackLimit = ionStackLimit_;
        cx_->runtime->ionTop = prevIonTop_;
    }
};

class ArrayTaskSet : public TaskSet {
  protected:
    JSContext *cx_;
    const char *name_;
    HandleObject buffer_;
    HandleObject fun_;

  public:
    ArrayTaskSet(JSContext *cx, const char *name, HandleObject buffer, HandleObject fun)
      : cx_(cx), name_(name), buffer_(buffer), fun_(fun)
    {}

    ExecutionStatus apply() {
        Spew(cx_, SpewOps, "%s: attempting parallel compilation", name_);
        if (!compileForParallelExecution())
            return ExecutionDisqualified;

        Spew(cx_, SpewOps, "%s: entering parallel section", name_);
        AutoEnterParallelSection enter(cx_);
        ParallelResult pr = js::ExecuteTaskSet(cx_, *this);
        return ToExecutionStatus(cx_, name_, pr);
    }

    bool compileForParallelExecution() {
        if (!ion::IsEnabled(cx_))
            return false;

        // The kernel should be a self-hosted function.
        if (!fun_->isFunction())
            return false;

        RootedFunction callee(cx_, fun_->toFunction());

        if (!callee->isInterpreted() || !callee->isSelfHostedBuiltin())
            return false;

        // Ensure that the function is analyzed by TI.
        bool hasIonScript;
        {
            AutoAssertNoGC nogc;
            hasIonScript = callee->script()->hasIonScript(COMPILE_MODE_PAR);
        }

        if (!hasIonScript) {
            // If the script has not been compiled in parallel, then type
            // inference will have no particular information.  In that case,
            // we need to do a few "warm-up" iterations to give type inference
            // some data to work with and to record all functions called.
            ParallelCompileContext compileContext(cx_);
            ion::js_IonOptions.startParallelWarmup(&compileContext);
            if (!warmup(3))
                return false;

            // If warmup returned false, assume that we finished warming up.
            ion::js_IonOptions.finishParallelWarmup();

            // After warming up, compile the outer kernel as a special
            // self-hosted kernel that can unsafely write to the buffer.
            if (!compileContext.compileKernelAndInvokedFunctions(callee))
                return false;
        }

        return true;
    }

    virtual bool warmup(uint32_t limit) { return true; }
    virtual bool pre(size_t numThreads) { return true; }
    virtual bool parallel(ThreadContext &taskSetCx) = 0;
    virtual bool post(size_t numThreads) { return true; }
};

static inline bool
InWarmup() {
    return ion::js_IonOptions.parallelWarmupContext != NULL;
}

class FillArrayTaskSet : public ArrayTaskSet
{
  public:
    FillArrayTaskSet(JSContext *cx, HandleObject buffer, HandleObject fun)
      : ArrayTaskSet(cx, "fill", buffer, fun)
    {
        JS_ASSERT(buffer->isDenseArray());
    }

    bool warmup(uint32_t limit) {
        JS_ASSERT(InWarmup());

        // Warm up with a high enough (and fake) number of threads so that
        // we don't run for too long.
        uint32_t numThreads = buffer_->getArrayLength() / limit;

        FastInvokeGuard fig(cx_, ObjectValue(*fun_), COMPILE_MODE_SEQ);
        InvokeArgsGuard &args = fig.args();
        if (!cx_->stack.pushInvokeArgs(cx_, 3, &args))
            return false;

        args.setCallee(ObjectValue(*fun_));
        args.setThis(UndefinedValue());

        args[0].setObject(*buffer_);
        args[1].setInt32(0);
        args[2].setInt32(numThreads);

        if (!fig.invoke(cx_))
            return false;

        return InWarmup();
    }

    bool pre(size_t numThreads) {
        // Ensure initialized length up to actual length so we don't crash in
        // parallel code.
        // FIXME: Make resizing arrays work in parallel.
        uint32_t length = buffer_->getArrayLength();
        uint32_t initlen = buffer_->getDenseArrayInitializedLength();

        JSObject::EnsureDenseResult result = JSObject::ED_SPARSE;
        result = buffer_->ensureDenseArrayElements(cx_, initlen, length - initlen);
        if (result != JSObject::ED_OK)
            return false;

        return length >= numThreads;
    }

    bool parallel(ThreadContext &threadCx) {
        printf("%ld %ld\n", threadCx.threadId, threadCx.numThreads);
        // 3 arguments: buffer, thread id, and number of threads.
        FastestIonInvoke<3> fii(cx_, fun_);
        fii.args[0] = ObjectValue(*buffer_);
        fii.args[1] = Int32Value(threadCx.threadId);
        fii.args[2] = Int32Value(threadCx.numThreads);
        return fii.invoke();
    }
};

ExecutionStatus
js::parallel::FillArray(JSContext *cx, HandleObject buffer, HandleObject fun)
{
    JS_ASSERT(fun->isFunction());

    FillArrayTaskSet taskSet(cx, buffer, fun);
    ExecutionStatus status = taskSet.apply();

    // If we bailed out, invalidate the kernel to be reanalyzed (all the way
    // down) and recompiled.
    //
    // TODO: This is too coarse grained.
    if (status == ExecutionBailout) {
        RootedScript script(cx, fun->toFunction()->script());
        Invalidate(cx, script, COMPILE_MODE_PAR);
    }

    return status;
}

//
// ParallelArrayObject
//

FixedHeapPtr<PropertyName> ParallelArrayObject::ctorNames[3];

// TODO: non-generic self hosted
JSFunctionSpec ParallelArrayObject::methods[] = {
    { "map", JSOP_NULLWRAPPER, 1, JSFUN_INTERPRETED, "ParallelArrayMap" },
    JS_FS_END
};

Class ParallelArrayObject::protoClass = {
    "ParallelArray",
    JSCLASS_HAS_CACHED_PROTO(JSProto_ParallelArray),
    JS_PropertyStub,         // addProperty
    JS_PropertyStub,         // delProperty
    JS_PropertyStub,         // getProperty
    JS_StrictPropertyStub,   // setProperty
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub
};

Class ParallelArrayObject::class_ = {
    "ParallelArray",
    JSCLASS_HAS_CACHED_PROTO(JSProto_ParallelArray),
    JS_PropertyStub,         // addProperty
    JS_PropertyStub,         // delProperty
    JS_PropertyStub,         // getProperty
    JS_StrictPropertyStub,   // setProperty
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub
};

JSBool
ParallelArrayObject::construct(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args0 = CallArgsFromVp(argc, vp);

    // See comment in ParallelArray.js about splitting constructors.
    uint32_t whichCtor = args0.length() > 1 ? 2 : args0.length();
    RootedValue ctor(cx, UndefinedValue());
    if (!cx->global()->getIntrinsicValue(cx, ctorNames[whichCtor], &ctor))
        return false;

    FastInvokeGuard fig(cx, ctor, COMPILE_MODE_SEQ);
    InvokeArgsGuard &args = fig.args();

    RootedObject result(cx, NewBuiltinClassInstance(cx, &class_));
    if (!result)
        return false;

    if (!cx->stack.pushInvokeArgs(cx, args0.length(), &args))
        return false;

    args.setCallee(ctor);
    args.setThis(ObjectValue(*result));

    for (uint32_t i = 0; i < args0.length(); i++)
        args[i] = args0[i];

    if (!fig.invoke(cx))
        return false;

    args0.rval().setObject(*result);
    return true;
}

JSObject *
ParallelArrayObject::initClass(JSContext *cx, HandleObject obj)
{
    JS_ASSERT(obj->isNative());

    // Cache constructor names.
    const char *ctorStrs[3] = { "ParallelArrayConstruct0",
                                "ParallelArrayConstruct1",
                                "ParallelArrayConstruct2" };
    for (uint32_t i = 0; i < 3; i++) {
        JSAtom *atom = Atomize(cx, ctorStrs[i], strlen(ctorStrs[i]), InternAtom);
        if (!atom)
            return NULL;
        ctorNames[i].init(atom->asPropertyName());
    }

    Rooted<GlobalObject *> global(cx, &obj->asGlobal());

    RootedObject proto(cx, global->createBlankPrototype(cx, &protoClass));
    if (!proto)
        return NULL;

    JSProtoKey key = JSProto_ParallelArray;
    RootedFunction ctor(cx, global->createConstructor(cx, construct,
                                                      cx->names().ParallelArray, 0));
    if (!ctor ||
        !LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndBrand(cx, proto, NULL, methods) ||
        !DefineConstructorAndPrototype(cx, global, key, ctor, proto))
    {
        return NULL;
    }

    return proto;
}

bool
ParallelArrayObject::is(const Value &v)
{
    return v.isObject() && v.toObject().hasClass(&class_);
}

JSObject *
js_InitParallelArrayClass(JSContext *cx, js::HandleObject obj)
{
    return ParallelArrayObject::initClass(cx, obj);
}

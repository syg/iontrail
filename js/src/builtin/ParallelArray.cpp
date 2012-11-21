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

#include "vm/threadpool.h"

#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsarrayinlines.h"
#include "vm/forkjoininlines.h"

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
template <uint32_t maxArgc>
class FastestIonInvoke
{
    EnterIonCode enter_;
    void *jitcode_;
    void *calleeToken_;
    Value argv_[maxArgc + 2];
    uint32_t argc_;

  public:
    Value *args;

    FastestIonInvoke(JSContext *cx, HandleObject fun, uint32_t argc)
      : argc_(argc),
        args(argv_ + 2)
    {
        JS_ASSERT(argc <= maxArgc + 2);
        JS_ASSERT(fun->isFunction());

        // Set 'callee' and 'this'.
        RootedFunction callee(cx, fun->toFunction());
        argv_[0] = ObjectValue(*callee);
        argv_[1] = UndefinedValue();

        // Find JIT code pointer.
        IonScript *ion = callee->script()->parallelIonScript();
        IonCode *code = ion->method();
        jitcode_ = code->raw();
        enter_ = cx->compartment->ionCompartment()->enterJIT();
        calleeToken_ = CalleeToToken(callee);
    }

    bool invoke() {
        Value result;
        enter_(jitcode_, argc_ + 1, argv_ + 1, NULL, calleeToken_, &result);
        return !result.isMagic();
    }
};

class ArrayOp : public ForkJoinOp {
  protected:
    JSContext *cx_;
    const char *name_;
    HeapPtrObject buffer_;
    HeapPtrObject fun_;

  public:
    ArrayOp(JSContext *cx, const char *name, HandleObject buffer, HandleObject fun)
      : cx_(cx), name_(name), buffer_(buffer), fun_(fun)
    {}

    ExecutionStatus apply() {
        Spew(cx_, SpewOps, "%s: attempting parallel compilation", name_);
        if (!ion::IsEnabled(cx_))
            return ExecutionDisqualified;

        MethodStatus status = compileForParallelExecution();
        if (status == Method_Error)
            return ExecutionFatal;
        if (status != Method_Compiled)
            return ExecutionDisqualified;

        Spew(cx_, SpewOps, "%s: entering parallel section", name_);
        ParallelResult pr = js::ExecuteForkJoinOp(cx_, *this);
        return ToExecutionStatus(cx_, name_, pr);
    }

    MethodStatus compileForParallelExecution() {
        // The kernel should be a self-hosted function.
        if (!fun_->isFunction())
            return Method_Skipped;

        RootedFunction callee(cx_, fun_->toFunction());

        if (!callee->isInterpreted() || !callee->isSelfHostedBuiltin())
            return Method_Skipped;

        // Ensure that the function is analyzed by TI.
        bool hasIonScript;
        {
            AutoAssertNoGC nogc;
            hasIonScript = callee->script()->hasParallelIonScript();
        }

        if (!hasIonScript) {
            // If the script has not been compiled in parallel, then type
            // inference will have no particular information.  In that case,
            // we need to do a few "warm-up" iterations to give type inference
            // some data to work with and to record all functions called.
            ParallelCompileContext compileContext(cx_);
            ion::js_IonOptions.startParallelWarmup(&compileContext);
            if (!warmup(3))
                return Method_Error;

            // If warmup returned false, assume that we finished warming up.
            ion::js_IonOptions.finishParallelWarmup();

            // After warming up, compile the outer kernel as a special
            // self-hosted kernel that can unsafely write to the buffer.
            return compileContext.compileKernelAndInvokedFunctions(callee);
        }

        return Method_Compiled;
    }

    virtual bool warmup(uint32_t limit) { return true; }
    virtual bool pre(size_t numThreads) { return true; }
    virtual bool parallel(ForkJoinSlice &slice) = 0;
    virtual bool post(size_t numThreads) { return true; }
};

static inline bool
InWarmup() {
    return ion::js_IonOptions.parallelWarmupContext != NULL;
}

class BuildArrayOp : public ArrayOp
{
    Value *funArgs_;
    uint32_t funArgc_;

  public:
    BuildArrayOp(JSContext *cx, HandleObject buffer, HandleObject fun,
                 Value *funArgs, uint32_t funArgc)
      : ArrayOp(cx, "fill", buffer, fun),
        funArgs_(funArgs),
        funArgc_(funArgc)
    {
        JS_ASSERT(funArgc <= 2);
        JS_ASSERT(buffer->isDenseArray());
    }

    bool warmup(uint32_t limit) {
        JS_ASSERT(InWarmup());

        // Warm up with a high enough (and fake) number of threads so that
        // we don't run for too long.
        uint32_t numThreads = buffer_->getArrayLength() / limit;

        InvokeArgsGuard args;
        if (!cx_->stack.pushInvokeArgs(cx_, 3 + funArgc_, &args))
            return false;

        args.setCallee(ObjectValue(*fun_));
        args.setThis(UndefinedValue());

        args[0].setObject(*buffer_);
        args[1].setInt32(0);
        args[2].setInt32(numThreads);
        for (uint32_t i = 0; i < funArgc_; i++)
            args[3 + i] = funArgs_[i];

        if (!Invoke(cx_, args))
            return false;

        JS_ASSERT(InWarmup());
        return true;
    }

    bool pre(size_t numThreads) {
        return buffer_->getDenseArrayInitializedLength() >= numThreads;
    }

    bool parallel(ForkJoinSlice &slice) {
        // Setting maximum argc at 7: the constructor uses 1 extra (the
        // kernel); map, reduce, scan use 2 extra (the kernel and the source
        // array); scatter uses 4 extra (the kernel, the default value, the
        // conflict resolution function, and the source array).
        js::PerThreadData *pt = slice.perThreadData;
        RootedObject fun(pt, fun_);
        FastestIonInvoke<7> fii(cx_, fun, funArgc_ + 3);

        // The first 3 arguments: buffer, thread id, and number of threads.
        fii.args[0] = ObjectValue(*buffer_);
        fii.args[1] = Int32Value(slice.sliceId);
        fii.args[2] = Int32Value(slice.numSlices);
        for (uint32_t i = 0; i < funArgc_; i++)
            fii.args[3 + i] = funArgs_[i];
        return fii.invoke();
    }
};

ExecutionStatus
js::parallel::BuildArray(JSContext *cx, CallArgs args)
{
    JS_ASSERT(args[1].isObject());
    JS_ASSERT(args[1].toObject().isFunction());

    // Nesting is not allowed.
    if (InWarmup()) {
        args.rval().setUndefined();
        return ExecutionDisqualified;
    }

    uint32_t length;
    if (!ToUint32(cx, args[0], &length))
        return ExecutionFatal;
    RootedObject fun(cx, &args[1].toObject());

    // Make a new buffer and initialize it up to length.
    RootedObject buffer(cx, NewDenseAllocatedArray(cx, length));
    if (!buffer)
        return ExecutionFatal;
    JSObject::EnsureDenseResult edr = buffer->ensureDenseArrayElements(cx, length, 0);
    if (edr != JSObject::ED_OK)
        return ExecutionFatal;

    NonBuiltinScriptFrameIter iter(cx);
    JS_ASSERT(!iter.done());
    RootedScript script(cx, iter.script());
    if (!types::SetInitializerObjectType(cx, script, iter.pc(), buffer))
        return ExecutionFatal;

    BuildArrayOp op(cx, buffer, fun, args.array() + 2, args.length() - 2);
    ExecutionStatus status = op.apply();

    // If we bailed out, invalidate the kernel to be reanalyzed (all the way
    // down) and recompiled.
    //
    // TODO: This is too coarse grained.
    if (status == ExecutionSucceeded) {
        args.rval().setObject(*buffer);
    } else {
        if (status == ExecutionBailout) {
            RootedScript script(cx, fun->toFunction()->script());
            Invalidate(cx, script, ParallelExecution);
        }

        args.rval().setUndefined();
    }

    return status;
}

//
// ParallelArrayObject
//

FixedHeapPtr<PropertyName> ParallelArrayObject::ctorNames[3];

// TODO: non-generic self hosted
JSFunctionSpec ParallelArrayObject::methods[] = {
    { "map",      JSOP_NULLWRAPPER, 1, 0, "ParallelArrayMap"      },
    { "reduce",   JSOP_NULLWRAPPER, 1, 0, "ParallelArrayReduce"   },
    { "scan",     JSOP_NULLWRAPPER, 1, 0, "ParallelArrayScan"     },
    { "scatter",  JSOP_NULLWRAPPER, 1, 0, "ParallelArrayScatter"  },
    { "filter",   JSOP_NULLWRAPPER, 1, 0, "ParallelArrayFilter"   },
    { "get",      JSOP_NULLWRAPPER, 1, 0, "ParallelArrayGet" },
    { "toString", JSOP_NULLWRAPPER, 1, 0, "ParallelArrayToString" },
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

    RootedObject result(cx, NewBuiltinClassInstance(cx, &class_));
    if (!result)
        return false;

    InvokeArgsGuard args;
    if (!cx->stack.pushInvokeArgs(cx, args0.length(), &args))
        return false;

    args.setCallee(ctor);
    args.setThis(ObjectValue(*result));

    for (uint32_t i = 0; i < args0.length(); i++)
        args[i] = args0[i];

    if (!Invoke(cx, args))
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

    // Define the length getter.
    const char lengthStr[] = "ParallelArrayLength";
    JSAtom *atom = Atomize(cx, lengthStr, strlen(lengthStr));
    if (!atom)
        return NULL;
    Rooted<PropertyName *> lengthProp(cx, atom->asPropertyName());
    RootedObject lengthGetter(cx, cx->runtime->getSelfHostedFunction(cx, lengthProp));
    if (!lengthGetter)
        return NULL;

    RootedId lengthId(cx, AtomToId(cx->names().length));
    unsigned flags = JSPROP_PERMANENT | JSPROP_SHARED | JSPROP_GETTER;
    RootedValue value(cx, UndefinedValue());
    if (!DefineNativeProperty(cx, proto, lengthId, value,
                              JS_DATA_TO_FUNC_PTR(PropertyOp, lengthGetter.get()), NULL,
                              flags, 0, 0))
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

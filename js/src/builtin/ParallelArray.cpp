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

#include "vm/String.h"
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
    fprintf(stderr, "[ParallelArray] %s:%u: ", script->filename, PCToLineNumber(script, pc));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
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

#ifdef DEBUG
    SpewExecution(cx, opName, status);
#endif

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

class ArrayOp : public ForkJoinOp
{
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
            if (!warmup())
                return Method_Error;

            // If warmup returned false, assume that we finished warming up.
            ion::js_IonOptions.finishParallelWarmup();

            // After warming up, compile the outer kernel as a special
            // self-hosted kernel that can unsafely write to the buffer.
            Spew(cx_, SpewOps, "%s: compilation", name_);
            return compileContext.compileKernelAndInvokedFunctions(callee);
        }

        return Method_Compiled;
    }

    virtual bool warmup() { return true; }
    virtual bool parallel(ForkJoinSlice &slice) = 0;
};

static inline bool
InWarmup() {
    return ion::js_IonOptions.parallelWarmupContext != NULL;
}

class BuildArrayOp : public ArrayOp
{
    static const uint32_t baseArgc = 4;

    Value *funArgs_;
    uint32_t funArgc_;

  public:
    BuildArrayOp(JSContext *cx, HandleObject buffer, HandleObject fun,
                 Value *funArgs, uint32_t funArgc)
      : ArrayOp(cx, "fill", buffer, fun),
        funArgs_(funArgs),
        funArgc_(funArgc)
    {
        JS_ASSERT(buffer->isDenseArray());
    }

    bool warmup() {
        JS_ASSERT(InWarmup());

        uint32_t slices = ForkJoinSlices(cx_);
        for (uint32_t id = 0; id < slices; id++) {
            Spew(cx_, SpewOps, "%s: warmup %u/%u", name_, id, slices);

            InvokeArgsGuard args;
            if (!cx_->stack.pushInvokeArgs(cx_, baseArgc + funArgc_, &args))
                return false;

            args.setCallee(ObjectValue(*fun_));
            args.setThis(UndefinedValue());

            // run the warmup with each of the ids
            args[0].setObject(*buffer_);
            args[1].setInt32(id);
            args[2].setInt32(slices);
            args[3].setBoolean(true); // warmup
            for (uint32_t i = 0; i < funArgc_; i++)
                args[baseArgc + i] = funArgs_[i];

            if (!Invoke(cx_, args))
                return false;
        }

        JS_ASSERT(InWarmup());
        return true;
    }

    bool parallel(ForkJoinSlice &slice) {
        // Setting maximum argc at 10, since it is more than we
        // actually use in practice.  If you add parameters, you may
        // have to adjust this.
        js::PerThreadData *pt = slice.perThreadData;
        RootedObject fun(pt, fun_);
        FastestIonInvoke<10> fii(cx_, fun, funArgc_ + baseArgc);

        // The first 3 arguments: buffer, thread id, and number of threads.
        fii.args[0] = ObjectValue(*buffer_);
        fii.args[1] = Int32Value(slice.sliceId);
        fii.args[2] = Int32Value(slice.numSlices);
        fii.args[3] = BooleanValue(false); // warmup
        for (uint32_t i = 0; i < funArgc_; i++)
            fii.args[baseArgc + i] = funArgs_[i];
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

    if (length < ForkJoinSlices(cx))
        return ExecutionDisqualified;

    RootedObject fun(cx, &args[1].toObject());

    // Make a new buffer and initialize it up to length.
    RootedObject buffer(cx, NewDenseAllocatedArray(cx, length));
    if (!buffer)
        return ExecutionFatal;

    types::TypeObject *newtype = types::GetTypeCallerInitObject(cx, JSProto_Array);
    if (!newtype)
        return ExecutionFatal;
    buffer->setType(newtype);

    JSObject::EnsureDenseResult edr = buffer->ensureDenseArrayElements(cx, length, 0);
    if (edr != JSObject::ED_OK)
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

const uint32_t ParallelArrayObject::NumCtors;
FixedHeapPtr<PropertyName> ParallelArrayObject::ctorNames[NumCtors];
FixedHeapPtr<PropertyName> ParallelArrayObject::propNames[NumFixedSlots];

// TODO: non-generic self hosted
JSFunctionSpec ParallelArrayObject::methods[] = {
    { "map",       JSOP_NULLWRAPPER, 1, 0, "ParallelArrayMap"       },
    { "reduce",    JSOP_NULLWRAPPER, 1, 0, "ParallelArrayReduce"    },
    { "scan",      JSOP_NULLWRAPPER, 1, 0, "ParallelArrayScan"      },
    { "scatter",   JSOP_NULLWRAPPER, 1, 0, "ParallelArrayScatter"   },
    { "filter",    JSOP_NULLWRAPPER, 1, 0, "ParallelArrayFilter"    },
    { "partition", JSOP_NULLWRAPPER, 1, 0, "ParallelArrayPartition" },
    { "flatten",   JSOP_NULLWRAPPER, 0, 0, "ParallelArrayFlatten" },
    /*{ "get",      JSOP_NULLWRAPPER, 1, 0, "ParallelArrayGet" },*/
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

/*static*/ bool
ParallelArrayObject::initProps(JSContext *cx, HandleObject obj)
{
    for (uint32_t slot = 0; slot < NumFixedSlots; slot++) {
        RootedValue val(cx, (slot == Offset ? JSVAL_ZERO : JSVAL_NULL));
        if (!JSObject::setProperty(cx, obj, obj, propNames[slot], &val, true))
            return false;
    }
    return true;
}

/*static*/ JSBool
ParallelArrayObject::construct(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args0 = CallArgsFromVp(argc, vp);

    // See comment in ParallelArray.js about splitting constructors.
    uint32_t whichCtor = js::Min(args0.length(), NumCtors - 1);
    RootedValue ctor(cx, UndefinedValue());
    if (!cx->global()->getIntrinsicValue(cx, ctorNames[whichCtor], &ctor))
        return false;

    gc::AllocKind kind = gc::GetGCObjectKind(NumFixedSlots);
    RootedObject result(cx, NewBuiltinClassInstance(cx, &class_, kind));
    if (!result)
        return false;

    // Add in the basic PA properties now with default values:
    if (!initProps(cx, result))
        return false;

    // Create the type object for the PA.  Add in the current
    // properties as definite properties if this type object is newly
    // created.  To tell if it is newly created, we check whether it
    // has any properties yet or not, since any returned type object
    // must have been created by this same C++ code and hence would
    // already have properties if it had been returned before.
    types::TypeObject *paTypeObject =
        types::GetTypeCallerInitObject(cx, JSProto_ParallelArray);
    if (!paTypeObject)
        return false;
    if (paTypeObject->getPropertyCount() == 0) {
        if (!paTypeObject->addDefiniteProperties(cx, result))
            return false;
        JS_ASSERT(paTypeObject->getPropertyCount() == NumFixedSlots);
    }
    result->setType(paTypeObject);

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
    {
        const char *ctorStrs[NumCtors] = { "ParallelArrayConstruct0",
                                           "ParallelArrayConstruct1",
                                           "ParallelArrayConstruct2",
                                           "ParallelArrayConstruct3" };
        for (uint32_t i = 0; i < NumCtors; i++) {
            JSAtom *atom = Atomize(cx, ctorStrs[i], strlen(ctorStrs[i]), InternAtom);
            if (!atom)
                return NULL;
            ctorNames[i].init(atom->asPropertyName());
        }
    }

    // Cache property names.
    {
        const char *propStrs[NumFixedSlots] = { "buffer",
                                                "offset",
                                                "shape",
                                                "get" };
        for (uint32_t i = 0; i < NumFixedSlots; i++) {
            JSAtom *atom = Atomize(cx, propStrs[i], strlen(propStrs[i]), InternAtom);
            if (!atom)
                return NULL;
            propNames[i].init(atom->asPropertyName());
        }
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
    {
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

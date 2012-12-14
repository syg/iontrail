#include <stdio.h>

#include "Ion.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "ParallelArrayAnalysis.h"
#include "IonSpewer.h"

#include "vm/Stack.h"

namespace js {
namespace ion {

typedef uint32_t typeset_t;

static inline typeset_t typeset(MIRType type) {
    return 1 << (uint32_t) type;
}

static inline bool isSubset(typeset_t small, typeset_t big) {
    return ((big & small) == small);
}

static inline typeset_t containsType(typeset_t set, MIRType type) {
    return (set & typeset(type)) != 0;
}

#define SAFE_OP(op)                             \
    virtual bool visit##op(M##op *prop) { return true; }

#define CUSTOM_OP(op)                        \
    virtual bool visit##op(M##op *prop);

#define DROP_OP(op)                             \
    virtual bool visit##op(M##op *ins) {        \
        MBasicBlock *block = ins->block();      \
        block->discard(ins);                    \
        return true;                            \
    }

#define SPECIALIZED_OP(op)                                                    \
    virtual bool visit##op(M##op *ins) {                                      \
        return visitSpecializedInstruction(ins, ins->specialization());       \
    }

#define UNSAFE_OP(op)                                               \
    virtual bool visit##op(M##op *prop) {                           \
        IonSpew(IonSpew_ParallelArray, "Unsafe op %s found", #op);  \
        return false;                                               \
    }

#define WRITE_GUARDED_OP(op, obj)                   \
    virtual bool visit##op(M##op *prop) {           \
        return insertWriteGuard(prop, prop->obj()); \
    }

#define MAYBE_WRITE_GUARDED_OP(op, obj)                                       \
    virtual bool visit##op(M##op *prop) {                                     \
        if (prop->racy())                                                     \
            return true;                                                      \
        return insertWriteGuard(prop, prop->obj());                           \
    }

class ParallelArrayVisitor : public MInstructionVisitor
{
    ParallelCompileContext &compileContext_;
    MIRGraph &graph_;

    bool insertWriteGuard(MInstruction *writeInstruction,
                          MDefinition *valueBeingWritten);

    bool replaceWithParNew(MInstruction *newInstruction,
                           JSObject *templateObject);

    bool replace(MInstruction *oldInstruction,
                 MInstruction *replacementInstruction);

    bool visitSpecializedInstruction(MInstruction *ins, MIRType spec);

  public:
    ParallelArrayVisitor(ParallelCompileContext &compileContext,
                         MIRGraph &graph)
        : compileContext_(compileContext),
          graph_(graph)
    {
    }

    MDefinition *parSlice() { return graph_.parSlice(); }

    // I am taking the policy of blacklisting everything that's not
    // obviously safe for now.  We can loosen as we need.

    SAFE_OP(Constant)
    SAFE_OP(Parameter)
    SAFE_OP(Callee)
    SAFE_OP(TableSwitch)
    SAFE_OP(Goto)
    CUSTOM_OP(Test)
    SPECIALIZED_OP(Compare)
    SAFE_OP(Phi)
    SAFE_OP(Beta)
    UNSAFE_OP(OsrValue)
    UNSAFE_OP(OsrScopeChain)
    UNSAFE_OP(ReturnFromCtor)
    CUSTOM_OP(CheckOverRecursed)
    DROP_OP(RecompileCheck)
    UNSAFE_OP(DefVar)
    UNSAFE_OP(CreateThis)
    SAFE_OP(PrepareCall)
    SAFE_OP(PassArg)
    CUSTOM_OP(Call)
    UNSAFE_OP(ApplyArgs)
    SAFE_OP(BitNot)
    UNSAFE_OP(TypeOf)
    SAFE_OP(ToId)
    SAFE_OP(BitAnd)
    SAFE_OP(BitOr)
    SAFE_OP(BitXor)
    SAFE_OP(Lsh)
    SAFE_OP(Rsh)
    SPECIALIZED_OP(Ursh)
    SPECIALIZED_OP(MinMax)
    SAFE_OP(Abs)
    SAFE_OP(Sqrt)
    SAFE_OP(MathFunction)
    SPECIALIZED_OP(Add)
    SPECIALIZED_OP(Sub)
    SPECIALIZED_OP(Mul)
    SPECIALIZED_OP(Div)
    SPECIALIZED_OP(Mod)
    UNSAFE_OP(Concat)
    UNSAFE_OP(CharCodeAt)
    UNSAFE_OP(FromCharCode)
    SAFE_OP(Return)
    CUSTOM_OP(Throw)
    SAFE_OP(Box)     // Boxing just creates a JSVal, doesn't alloc.
    SAFE_OP(Unbox)
    SAFE_OP(GuardObject)
    SAFE_OP(ToDouble)
    SAFE_OP(ToInt32)
    SAFE_OP(TruncateToInt32)
    UNSAFE_OP(ToString)
    SAFE_OP(NewSlots)
    CUSTOM_OP(NewArray)
    CUSTOM_OP(NewObject)
    CUSTOM_OP(NewCallObject)
    CUSTOM_OP(NewParallelArray)
    UNSAFE_OP(InitProp)
    SAFE_OP(Start)
    UNSAFE_OP(OsrEntry)
    SAFE_OP(Nop)
    UNSAFE_OP(RegExp)
    CUSTOM_OP(Lambda)
    UNSAFE_OP(ImplicitThis)
    SAFE_OP(Slots)
    SAFE_OP(Elements)
    SAFE_OP(ConstantElements)
    SAFE_OP(LoadSlot)
    WRITE_GUARDED_OP(StoreSlot, slots)
    SAFE_OP(FunctionEnvironment) // just a load of func env ptr
    SAFE_OP(TypeBarrier) // causes a bailout if the type is not found: a-ok with us
    SAFE_OP(MonitorTypes) // causes a bailout if the type is not found: a-ok with us
    UNSAFE_OP(GetPropertyCache)
    UNSAFE_OP(GetElementCache)
    UNSAFE_OP(BindNameCache)
    SAFE_OP(GuardShape)
    SAFE_OP(GuardClass)
    SAFE_OP(ArrayLength)
    SAFE_OP(TypedArrayLength)
    SAFE_OP(TypedArrayElements)
    SAFE_OP(InitializedLength)
    WRITE_GUARDED_OP(SetInitializedLength, elements)
    SAFE_OP(Not)
    SAFE_OP(BoundsCheck)
    SAFE_OP(BoundsCheckLower)
    SAFE_OP(LoadElement)
    SAFE_OP(LoadElementHole)
    MAYBE_WRITE_GUARDED_OP(StoreElement, elements)
    WRITE_GUARDED_OP(StoreElementHole, elements)
    UNSAFE_OP(ArrayPopShift)
    UNSAFE_OP(ArrayPush)
    SAFE_OP(LoadTypedArrayElement)
    SAFE_OP(LoadTypedArrayElementHole)
    MAYBE_WRITE_GUARDED_OP(StoreTypedArrayElement, elements)
    UNSAFE_OP(ClampToUint8)
    SAFE_OP(LoadFixedSlot)
    WRITE_GUARDED_OP(StoreFixedSlot, object)
    UNSAFE_OP(CallGetProperty)
    UNSAFE_OP(GetNameCache)
    SAFE_OP(CallGetIntrinsicValue) // Bails in parallel mode
    UNSAFE_OP(CallGetElement)
    UNSAFE_OP(CallSetElement)
    UNSAFE_OP(CallSetProperty)
    UNSAFE_OP(DeleteProperty)
    UNSAFE_OP(SetPropertyCache)
    UNSAFE_OP(IteratorStart)
    UNSAFE_OP(IteratorNext)
    UNSAFE_OP(IteratorMore)
    UNSAFE_OP(IteratorEnd)
    SAFE_OP(StringLength)
    UNSAFE_OP(ArgumentsLength)
    UNSAFE_OP(GetArgument)
    SAFE_OP(Floor)
    SAFE_OP(Round)
    UNSAFE_OP(InstanceOf)
    CUSTOM_OP(InterruptCheck)
    SAFE_OP(ParSlice)
    SAFE_OP(ParNew)
    SAFE_OP(ParNewDenseArray)
    SAFE_OP(ParNewCallObject)
    SAFE_OP(ParLambda)
};

ParallelCompileContext::ParallelCompileContext(JSContext *cx)
    : cx_(cx),
      invokedFunctions_(cx)
{
}

bool
ParallelCompileContext::addInvocation(StackFrame *fp)
{
    AutoAssertNoGC nogc;

    // Stop warmup mode if we invoked a frame that we can't enter from
    // parallel code.
    if (!fp->isFunctionFrame() || !fp->fun()->isInterpreted()) {
        IonSpew(IonSpew_ParallelArray, "invoked unsafe fn during warmup");
        js_IonOptions.finishParallelWarmup();
        return true;
    }

    JSFunction *fun = fp->fun();

    // We don't go through normal Ion or JM paths that bump the use count, so
    // do it here so we can get inlining of hot functions.
    fun->nonLazyScript()->incUseCount();

    // Already compiled for parallel execution? Our work is done.
    if (fun->nonLazyScript()->hasParallelIonScript())
        return true;

    if (!invokedFunctions_.append(fun)) {
        IonSpew(IonSpew_ParallelArray, "failed to append!");
        return false;
    }

    return true;
}

MethodStatus
ParallelCompileContext::compileKernelAndInvokedFunctions(HandleFunction kernel)
{
    // Compile the kernel first as it can unsafely write to a buffer argument.
    if (!kernel->nonLazyScript()->hasParallelIonScript()) {
        IonSpew(IonSpew_ParallelArray, "Compiling kernel %p:%s:%u",
                kernel.get(), kernel->nonLazyScript()->filename, kernel->nonLazyScript()->lineno);
        MethodStatus status = compileFunction(kernel);
        if (status != Method_Compiled) {
            return status;
        }
    }

    for (size_t i = 0; i < invokedFunctions_.length(); i++) {
        RootedFunction fun(cx_, invokedFunctions_[i]->toFunction());

        IonSpew(IonSpew_ParallelArray, "Compiling invoked fn %p:%s:%u",
                fun.get(), fun->nonLazyScript()->filename, fun->nonLazyScript()->lineno);

        if (fun->nonLazyScript()->hasParallelIonScript()) {
            IonSpew(IonSpew_ParallelArray, "Already compiled");
            continue;
        }

        MethodStatus status = compileFunction(fun);
        if (status != Method_Compiled)
            return status;
    }

    // Subtle: it is possible for GC to occur during compilation of
    // one of the invoked functions, which would cause the earlier
    // functions (such as the kernel itself) to be collected.  In this
    // event, we give up and fallback to sequential for now.
    if (!kernel->nonLazyScript()->hasParallelIonScript()) {
        IonSpew(IonSpew_ParallelArray, "Kernel script %p:%s:%u was garbage-collected or invalidated",
                kernel.get(), kernel->nonLazyScript()->filename, kernel->nonLazyScript()->lineno);
        return Method_Skipped;
    }
    for (size_t i = 0; i < invokedFunctions_.length(); i++) {
        RootedFunction fun(cx_, invokedFunctions_[i]->toFunction());
        if (!fun->nonLazyScript()->hasParallelIonScript()) {
            IonSpew(IonSpew_ParallelArray, "Invoked script %p:%s:%u was garbage-collected or invalidated",
                    fun.get(), fun->nonLazyScript()->filename, fun->nonLazyScript()->lineno);
            return Method_Skipped;
        }
    }


    return Method_Compiled;
}

bool
ParallelCompileContext::canCompile(MIRGraph *graph)
{
    // Scan the IR and validate the instructions used in a peephole fashion
    ParallelArrayVisitor visitor(*this, *graph);
    for (MBasicBlockIterator block(graph->begin()); block != graph->end(); block++) {
        for (MInstructionIterator ins(block->begin()); ins != block->end();) {
            // we may be removing or replcae the current instruction,
            // so advance `ins` now.
            MInstruction *instr = *ins++;

            if (!instr->accept(&visitor)) {
                IonSpew(IonSpew_ParallelArray,
                        "ParallelArray fn uses unsafe instructions!\n");
                return false;
            }
        }
    }

    IonSpew(IonSpew_ParallelArray, "ParallelArray invoked with safe fn\n");

    IonSpewPass("Parallel Array Analysis");

    return true;
}

bool
ParallelArrayVisitor::visitTest(MTest *) {
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Memory allocation
//
// Simple memory allocation opcodes---those which ultimately compile
// down to a (possibly inlined) invocation of NewGCThing()---are
// replaced with MParNew, which is supplied with the thread context.
// These allocations will take place using per-helper-thread arenas.

bool
ParallelArrayVisitor::visitNewParallelArray(MNewParallelArray *ins) {
    MParNew *parNew = new MParNew(parSlice(), ins->templateObject());
    replace(ins, parNew);
    return true;
}

bool
ParallelArrayVisitor::visitNewCallObject(MNewCallObject *ins) {
    // fast path: replace with ParNewCallObject op
    MParNewCallObject *parNewCallObjectInstruction =
        MParNewCallObject::New(parSlice(), ins);
    replace(ins, parNewCallObjectInstruction);
    return true;
}

bool
ParallelArrayVisitor::visitLambda(MLambda *ins) {
    if (ins->fun()->hasSingletonType() ||
        types::UseNewTypeForClone(ins->fun())) {
        // slow path: bail on parallel execution.
        return false;
    }

    // fast path: replace with ParLambda op
    MParLambda *parLambdaInstruction = MParLambda::New(parSlice(), ins);
    replace(ins, parLambdaInstruction);
    return true;
}

bool
ParallelArrayVisitor::visitNewObject(MNewObject *newInstruction) {
    if (newInstruction->shouldUseVM()) {
        IonSpew(IonSpew_ParallelArray, "New object which SHOULD USE VM");
        return false;
    }

    return replaceWithParNew(newInstruction,
                             newInstruction->templateObject());
}

bool
ParallelArrayVisitor::visitNewArray(MNewArray *newInstruction) {
    if (newInstruction->shouldUseVM()) {
        IonSpew(IonSpew_ParallelArray, "New array which SHOULD USE VM");
        return false;
    }

    return replaceWithParNew(newInstruction,
                             newInstruction->templateObject());
}

bool
ParallelArrayVisitor::replaceWithParNew(MInstruction *newInstruction,
                                        JSObject *templateObject) {
    MParNew *parNewInstruction = new MParNew(parSlice(), templateObject);
    replace(newInstruction, parNewInstruction);
    return true;
}

bool
ParallelArrayVisitor::replace(MInstruction *oldInstruction,
                              MInstruction *replacementInstruction)
{
    MBasicBlock *block = oldInstruction->block();
    block->insertBefore(oldInstruction, replacementInstruction);
    block->discard(oldInstruction);
    oldInstruction->replaceAllUsesWith(replacementInstruction);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Write Guards
//
// We only want to permit writes to locally guarded objects.
// Furthermore, we want to avoid PICs and other non-thread-safe things
// (though perhaps we should support PICs at some point).  If we
// cannot determine the origin of an object, we can insert a write
// guard which will check whether the object was allocated from the
// per-thread-arena or not.

bool
ParallelArrayVisitor::insertWriteGuard(MInstruction *writeInstruction,
                                       MDefinition *valueBeingWritten)
{
    // Many of the write operations do not take the JS object
    // but rather something derived from it, such as the elements.
    // So we need to identify the JS object:
    MDefinition *object;
    switch (valueBeingWritten->type()) {
      case MIRType_Object:
        object = valueBeingWritten;
        break;

      case MIRType_Slots:
        switch (valueBeingWritten->op()) {
          case MDefinition::Op_Slots:
            object = valueBeingWritten->toSlots()->object();
            break;

          case MDefinition::Op_NewSlots:
            // Values produced by new slots will ALWAYS be
            // thread-local.
            return true;

          default:
            IonSpew(IonSpew_ParallelArray, "Cannot insert write guard for MIR opcode %d",
                    valueBeingWritten->op());
            return false;
        }
        break;

      case MIRType_Elements:
        switch (valueBeingWritten->op()) {
          case MDefinition::Op_Elements:
            object = valueBeingWritten->toElements()->object();
            break;

          case MDefinition::Op_TypedArrayElements:
            object = valueBeingWritten->toTypedArrayElements()->object();
            break;

          case MDefinition::Op_ConstantElements:
            IonSpew(IonSpew_ParallelArray, "write to constant elements");
            return false; // this can't be thread-safe

          default:
            IonSpew(IonSpew_ParallelArray, "Cannot insert write guard for MIR opcode %d",
                    valueBeingWritten->op());
            return false;
        }
        break;

      default:
        IonSpew(IonSpew_ParallelArray, "Cannot insert write guard for MIR Type %d",
                valueBeingWritten->type());
        return false;
    }

    if (object->isUnbox())
        object = object->toUnbox()->input();

    switch (object->op()) {
      case MDefinition::Op_ParNew:
        // MParNew will always be creating something thread-local, omit the guard
        IonSpew(IonSpew_ParallelArray, "Write to par new prop does not require guard");
        return true;
      default:
        break;
    }

    MBasicBlock *block = writeInstruction->block();
    MParWriteGuard *writeGuard = MParWriteGuard::New(parSlice(), object);
    block->insertBefore(writeInstruction, writeGuard);
    writeGuard->adjustInputs(writeGuard);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Calls
//
// We only support calls to interpreted functions that that have already been
// Ion compiled. If a function has no IonScript, we bail out. The compilation
// is done during warmup of the parallel kernel, see js::RunScript.

bool
ParallelArrayVisitor::visitCall(MCall *ins)
{
    // DOM? Scary.
    if (ins->isDOMFunction()) {
        IonSpew(IonSpew_ParallelArray, "call to dom function");
        return false;
    }

    JSFunction *target = ins->getSingleTarget();
    if (target) {
        // Native? Scary.
        if (target->isNative()) {
            IonSpew(IonSpew_ParallelArray, "call to native function");
            return false;
        }
        return true;
    }

    if (ins->isConstructing()) {
        IonSpew(IonSpew_ParallelArray, "call to unknown constructor");
        return false;
    }

    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Stack limit, interrupts
//
// In sequential Ion code, the stack limit is stored in the JSRuntime.
// We store it in the thread context.  We therefore need a separate
// instruction to access it, one parameterized by the thread context.
// Similar considerations apply to checking for interrupts.

bool
ParallelArrayVisitor::visitCheckOverRecursed(MCheckOverRecursed *ins)
{
    MParCheckOverRecursed *replacement = new MParCheckOverRecursed(parSlice());
    return replace(ins, replacement);
}

bool
ParallelArrayVisitor::visitInterruptCheck(MInterruptCheck *ins)
{
    MParCheckInterrupt *replacement = new MParCheckInterrupt(parSlice());
    return replace(ins, replacement);
}

/////////////////////////////////////////////////////////////////////////////
// Specialized ops
//
// Some ops, like +, can be specialized to ints/doubles.  Anything
// else is terrifying.
//
// TODO---Eventually, we should probably permit arbitrary + but bail
// if the operands are not both integers/floats.

bool
ParallelArrayVisitor::visitSpecializedInstruction(MInstruction *ins, MIRType spec)
{
    switch (spec) {
      case MIRType_Int32:
      case MIRType_Double:
        return true;

      default:
        IonSpew(IonSpew_ParallelArray, "Instr. %s not specialized to int or double",
                ins->opName());
        return false;
    }
}

/////////////////////////////////////////////////////////////////////////////
// Throw

bool
ParallelArrayVisitor::visitThrow(MThrow *thr)
{
    MBasicBlock *block = thr->block();
    JS_ASSERT(block->lastIns() == thr);
    block->discardLastIns();
    MParBailout *bailout = new MParBailout();
    if (!bailout)
        return false;
    block->end(bailout);
    return true;
}

}
}


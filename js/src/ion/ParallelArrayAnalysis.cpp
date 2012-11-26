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

#define COND_SAFE_OP(op)                        \
    virtual bool visit##op(M##op *prop);

#define DROP_OP(op)                             \
    virtual bool visit##op(M##op *ins) {        \
        MBasicBlock *block = ins->block();      \
        block->discard(ins);                    \
        return true;                            \
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

class ParallelArrayVisitor : public MInstructionVisitor
{
    ParallelCompileContext &compileContext_;
    MBasicBlock *entryBlock_;
    MInstruction *threadContext_;

    MInstruction *threadContext();

    bool insertWriteGuard(MInstruction *writeInstruction,
                          MDefinition *valueBeingWritten);

    bool replaceWithParNew(MInstruction *newInstruction,
                           JSObject *templateObject);

    bool replace(MInstruction *oldInstruction,
                 MInstruction *replacementInstruction);

  public:
    ParallelArrayVisitor(ParallelCompileContext &compileContext,
                         MBasicBlock *entryBlock)
        : compileContext_(compileContext),
          entryBlock_(entryBlock),
          threadContext_(new MParThreadContext())
    {
    }

    // I am taking the policy of blacklisting everything that's not
    // obviously safe for now.  We can loosen as we need.

    SAFE_OP(Constant)
    SAFE_OP(Parameter)
    SAFE_OP(Callee)
    SAFE_OP(TableSwitch)
    SAFE_OP(Goto)
    COND_SAFE_OP(Test)
    COND_SAFE_OP(Compare)
    SAFE_OP(Phi)
    SAFE_OP(Beta)
    UNSAFE_OP(OsrValue)
    UNSAFE_OP(OsrScopeChain)
    UNSAFE_OP(ReturnFromCtor)
    COND_SAFE_OP(CheckOverRecursed)
    DROP_OP(RecompileCheck)
    UNSAFE_OP(DefVar)
    UNSAFE_OP(CreateThis)
    SAFE_OP(PrepareCall)
    SAFE_OP(PassArg)
    COND_SAFE_OP(Call)
    UNSAFE_OP(ApplyArgs)
    SAFE_OP(BitNot)
    UNSAFE_OP(TypeOf)
    SAFE_OP(ToId)
    SAFE_OP(BitAnd)
    SAFE_OP(BitOr)
    SAFE_OP(BitXor)
    SAFE_OP(Lsh)
    SAFE_OP(Rsh)
    SAFE_OP(Ursh)
    SAFE_OP(Abs)
    SAFE_OP(Sqrt)
    SAFE_OP(MathFunction)
    SAFE_OP(Add)
    SAFE_OP(Sub)
    SAFE_OP(Mul)
    SAFE_OP(Div)
    SAFE_OP(Mod)
    SAFE_OP(Concat)
    SAFE_OP(CharCodeAt)
    SAFE_OP(FromCharCode)
    SAFE_OP(Return)
    UNSAFE_OP(Throw)
    SAFE_OP(Box)     // Boxing just creates a JSVal, doesn't alloc.
    SAFE_OP(Unbox)
    UNSAFE_OP(GuardObject)
    SAFE_OP(ToDouble)
    SAFE_OP(ToInt32)
    SAFE_OP(TruncateToInt32)
    UNSAFE_OP(ToString)
    UNSAFE_OP(NewSlots)
    COND_SAFE_OP(NewArray)
    COND_SAFE_OP(NewObject)
    UNSAFE_OP(NewCallObject)
    UNSAFE_OP(InitProp)
    COND_SAFE_OP(Start)
    UNSAFE_OP(OsrEntry)
    UNSAFE_OP(RegExp)
    UNSAFE_OP(Lambda)
    UNSAFE_OP(ImplicitThis)
    SAFE_OP(Slots)
    SAFE_OP(Elements)
    SAFE_OP(ConstantElements)
    SAFE_OP(LoadSlot)
    WRITE_GUARDED_OP(StoreSlot, slots)
    SAFE_OP(FunctionEnvironment) // just a load of func env ptr
    SAFE_OP(TypeBarrier) // causes a bailout if the type is not found: a-ok with us
    SAFE_OP(MonitorTypes) // causes a bailout if the type is not found: a-ok with us
    SAFE_OP(GetPropertyCache)
    SAFE_OP(GetElementCache)
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
    WRITE_GUARDED_OP(StoreElement, elements) // FIXME--unsafe if it triggers a resize
    WRITE_GUARDED_OP(StoreElementHole, elements) // FIXME--unsafe if it triggers a resize
    UNSAFE_OP(ArrayPopShift)
    UNSAFE_OP(ArrayPush)
    SAFE_OP(LoadTypedArrayElement)
    SAFE_OP(LoadTypedArrayElementHole)
    UNSAFE_OP(StoreTypedArrayElement)
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
    SAFE_OP(InstanceOf)
    COND_SAFE_OP(InterruptCheck) // FIXME---replace this with a version that bails
};

ParallelCompileContext::ParallelCompileContext(JSContext *cx)
    : cx_(cx),
      invokedFunctions_(cx),
      compilingKernel_(false)
{
}

bool
ParallelCompileContext::canUnsafelyWrite(MInstruction *write, MDefinition *obj)
{
    // By convention, allow the elements of the first argument of the
    // self-hosted kernel parallel code to be stored to, unsafely, without a
    // guard.
    if (!write->isStoreElement() && !write->isStoreElementHole())
        return false;

    return compilingKernel_ && obj->isParameter() && obj->toParameter()->index() == 0;
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
    fun->script()->incUseCount();

    // Already compiled for parallel execution? Our work is done.
    if (fun->script()->hasParallelIonScript())
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
    JS_ASSERT(!compilingKernel_);

    MethodStatus status;

    // Compile the kernel first as it can unsafely write to a buffer argument.
    if (!kernel->script()->hasParallelIonScript()) {
        compilingKernel_ = true;
        status = compileFunction(kernel);
        if (status != Method_Compiled) {
            compilingKernel_ = false;
            return status;
        }
        compilingKernel_ = false;
    }

    for (size_t i = 0; i < invokedFunctions_.length(); i++) {
        RootedFunction fun(cx_, invokedFunctions_[i]->toFunction());

        if (fun->script()->hasParallelIonScript())
            continue; // Already compiled.

        status = compileFunction(fun);
        if (status != Method_Compiled)
            return status;
    }

    return status;
}

bool
ParallelCompileContext::canCompile(MIRGraph *graph)
{
    // Scan the IR and validate the instructions used in a peephole fashion
    ParallelArrayVisitor visitor(*this, graph->entryBlock());
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

bool
ParallelArrayVisitor::visitCompare(MCompare *) {
    return true;
}

// ___________________________________________________________________________
// Thread Context
//
// The Thread Context carries "per-helper-thread" information.
// Whenever necessary, we load it once in the beginning of the
// function from the thread-local storage.  The pointer is then
// supplied to the various Par MIR instructions that require access to
// the per-helper-thread data.

MInstruction *
ParallelArrayVisitor::threadContext()
{
    return threadContext_;
}

bool
ParallelArrayVisitor::visitStart(MStart *ins) {
    MBasicBlock *block = ins->block();
    block->insertAfter(ins, threadContext_);
    return true;
}

// ___________________________________________________________________________
// Memory allocation
//
// Simple memory allocation opcodes---those which ultimately compile
// down to a (possibly inlined) invocation of NewGCThing()---are
// replaced with MParNew, which is supplied with the thread context.
// These allocations will take place using per-helper-thread arenas.

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
    MDefinition *threadContext = this->threadContext();
    MParNew *parNewInstruction = new MParNew(threadContext, templateObject);
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

// ___________________________________________________________________________
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
      case MDefinition::Op_Parameter:
        if (compileContext_.canUnsafelyWrite(writeInstruction, object)) {
            // XXX: Super gross hack. We know the buffer in self-hosted code
            // is initialized up to length, so we won't ever write to a hole.
            IonSpew(IonSpew_ParallelArray, "Write to buffer in self-hosted code does not require guard");

            MStoreElement *store;
            if (writeInstruction->isStoreElementHole()) {
                IonSpew(IonSpew_ParallelArray, "Unholing already-unsafe buffer write");
                MStoreElementHole *storeHole = writeInstruction->toStoreElementHole();
                store = MStoreElement::New(storeHole->elements(), storeHole->index(),
                                           storeHole->value());
                replace(writeInstruction, store);
            } else {
                store = writeInstruction->toStoreElement();
            }

            // Since we initialize with the JS_ARRAY_HOLE, we have to always
            // write the tag.
            store->setElementType(MIRType_Value);

            return true;
        }
      break;
      default: break;
    }

    MDefinition *threadContext = this->threadContext();
    MBasicBlock *block = writeInstruction->block();
    MParWriteGuard *writeGuard = MParWriteGuard::New(threadContext, object);
    block->insertBefore(writeInstruction, writeGuard);
    writeGuard->adjustInputs(writeGuard);
    return true;
}

// ___________________________________________________________________________
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

    return true;
}

// ___________________________________________________________________________
// Stack limit, interrupts
//
// In sequential Ion code, the stack limit is stored in the JSRuntime.
// We store it in the thread context.  We therefore need a separate
// instruction to access it, one parameterized by the thread context.
// Similar considerations apply to checking for interrupts.

bool
ParallelArrayVisitor::visitCheckOverRecursed(MCheckOverRecursed *ins)
{
    MDefinition *threadContext = this->threadContext();
    MParCheckOverRecursed *replacement = new MParCheckOverRecursed(threadContext);
    return replace(ins, replacement);
}

bool
ParallelArrayVisitor::visitInterruptCheck(MInterruptCheck *ins)
{
    MDefinition *threadContext = this->threadContext();
    MParCheckInterrupt *replacement = new MParCheckInterrupt(threadContext);
    return replace(ins, replacement);
}

}
}

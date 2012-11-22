#include <stdio.h>

#include "Ion.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "ParallelArrayAnalysis.h"
#include "IonSpewer.h"

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
    ParallelCompilationContext &compileContext_;
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
    ParallelArrayVisitor(ParallelCompilationContext &compileContext,
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
    SAFE_OP(RecompileCheck) // XXX NDM XXX NDM
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

ParallelCompilationContext::ParallelCompilationContext(JSContext *cx)
    : cx_(cx),
      invokedFunctions_(cx)
{
}

bool
ParallelCompilationContext::compileFunctionAndInvokedFunctions(HandleFunction fun0)
{
    JS_ASSERT(fun0->isInterpreted());

    if (!invokedFunctions_.append(fun0.get()))
        return false;

    for (size_t i = 0; i < invokedFunctions_.length(); i++) {
        RootedFunction rootedFun(cx_, invokedFunctions_[i]->toFunction());
        HandleFunction fun(rootedFun);

        if (fun->script()->hasParallelIonScript())
            continue; // Already compiled.

        if (CanEnterParallelArrayKernel(cx_, fun, *this) != Method_Compiled)
            return false;
    }

    return true;
}

bool
ParallelCompilationContext::addInvokedFunction(JSFunction *fun)
{
    JS_ASSERT(fun->isInterpreted());

    // Already compiled for parallel execution? Our work is done.
    if (fun->script()->hasParallelIonScript()) {
        return true;
    }

    if (!invokedFunctions_.append(fun)) {
        IonSpew(IonSpew_ParallelArray, "failed to append!");
        return false;
    }

    return true;
}

bool
ParallelCompilationContext::canCompileParallelArrayKernel(MIRGraph *graph)
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

    switch (object->op()) {
      case MDefinition::Op_ParNew:
          // MParNew will always be creating something thread-local, omit the guard
          IonSpew(IonSpew_ParallelArray, "Write to par new prop does not require guard");
          return true;
      default: break;
    }

    MDefinition *threadContext = this->threadContext();
    MBasicBlock *block = writeInstruction->block();
    MParWriteGuard *writeGuard = MParWriteGuard::New(threadContext, object);
    block->insertBefore(writeInstruction, writeGuard);
    return true;
}

// ___________________________________________________________________________
// Calls
//
// For now, we only support calls to known targets.  Furthermore,
// those targets must be non-native (that is, JS) or else they must
// appear on a whitelist of safe functions.
//
// The main reason for these restrictions is that we do not
// (currently) support compilation during the parallel phase.
// Therefore, we accumulate and compile the full set of known targets
// ahead of time.  During execution, if we find that a target is not
// compiled, we'll just bailout of parallel mode.
//
// The visitCall() visit method checks to see that the call has a
// known target.  If the target is not already compiled in parallel
// mode, the target is also added to the ParallelCompilationContext's
// list of targets. An outer loop will then ensure that the target is
// itself compiled.

bool
ParallelArrayVisitor::visitCall(MCall *ins)
{
    // DOM? Scary.
    if (ins->isDOMFunction()) {
        IonSpew(IonSpew_ParallelArray, "call to dom function");
        return false;
    }

    JSFunction *target = ins->getSingleTarget();

    // Unknown target? Worrisome.
    if (target == NULL) {
        IonSpew(IonSpew_ParallelArray, "call with unknown target");
        return false;
    }

    // C++? Frightening.
    if (target->isNative()) {
        IonSpew(IonSpew_ParallelArray, "call with native target");
        return false;
    }

    // JavaScript? Not too many args, I hope.
    if (ins->numActualArgs() > js_IonOptions.maxStackArgs) {
        IonSpew(IonSpew_ParallelArray, "call with too many args: %d",
                ins->numActualArgs());
        return false;
    }

    // OK, everything checks out. Add to the list of functions that
    // will need to be compiled for this function to actually execute
    // successfully.
    if (!compileContext_.addInvokedFunction(target)) {
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

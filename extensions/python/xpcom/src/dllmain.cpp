/* Copyright (c) 2000-2001 ActiveState Tool Corporation.
   See the file LICENSE.txt for licensing information. */

//
// This code is part of the XPCOM extensions for Python.
//
// Written May 2000 by Mark Hammond.
//
// Based heavily on the Python COM support, which is
// (c) Mark Hammond and Greg Stein.
//
// (c) 2000, ActiveState corp.

#include "PyXPCOM_std.h"
#include <prthread.h>

#ifdef XP_WIN
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#endif

static PRInt32 g_cLockCount = 0;
static PRBool bDidInitPython = PR_FALSE;
static PyThreadState *ptsGlobal = nsnull;
PyInterpreterState *PyXPCOM_InterpreterState;
static PRLock *g_lockMain = nsnull;

PRUintn tlsIndex = 0;


////////////////////////////////////////////////////////////
// Thread-state helpers/global functions.
//

// This function must be called at some time when the interpreter lock and state is valid.
// Called by init{module} functions and also COM factory entry point.
void PyXPCOM_InterpreterState_Ensure()
{
	if (PyXPCOM_InterpreterState==NULL) {
		PyThreadState *threadStateSave = PyThreadState_Swap(NULL);
		if (threadStateSave==NULL)
			Py_FatalError("Can not setup interpreter state, as current state is invalid");

		PyXPCOM_InterpreterState = threadStateSave->interp;
		PyThreadState_Swap(threadStateSave);
	}
}

void PyXPCOM_InterpreterState_Free()
{
	PyXPCOM_ThreadState_Free();
	PyXPCOM_InterpreterState = NULL; // Eek - should I be freeing something?
}

// This structure is stored in the TLS slot.  At this stage only a Python thread state
// is kept, but this may change in the future...
struct ThreadData{
	PyThreadState *ts;
};

// Ensure that we have a Python thread state available to use.
// If this is called for the first time on a thread, it will allocate
// the thread state.  This does NOT change the state of the Python lock.
// Returns TRUE if a new thread state was created, or FALSE if a
// thread state already existed.
PRBool PyXPCOM_ThreadState_Ensure()
{
	ThreadData *pData = (ThreadData *)PR_GetThreadPrivate(tlsIndex);
	if (pData==NULL) { /* First request on this thread */
		/* Check we have an interpreter state */
		if (PyXPCOM_InterpreterState==NULL) {
				Py_FatalError("Can not setup thread state, as have no interpreter state");
		}
		pData = (ThreadData *)nsAllocator::Alloc(sizeof(ThreadData));
		if (!pData)
			Py_FatalError("Out of memory allocating thread state.");
		memset(pData, 0, sizeof(*pData));
		if (NS_FAILED( PR_SetThreadPrivate( tlsIndex, pData ) ) ) {
			NS_ABORT_IF_FALSE(0, "Could not create thread data for this thread!");
			Py_FatalError("Could not thread private thread data!");
		}
		pData->ts = PyThreadState_New(PyXPCOM_InterpreterState);
		return PR_TRUE; // Did create a thread state state
	}
	return PR_FALSE; // Thread state was previously created
}

// Asuming we have a valid thread state, acquire the Python lock.
void PyXPCOM_InterpreterLock_Acquire()
{
	ThreadData *pData = (ThreadData *)PR_GetThreadPrivate(tlsIndex);
	NS_ABORT_IF_FALSE(pData, "Have no thread data for this thread!");
	PyThreadState *thisThreadState = pData->ts;
	PyEval_AcquireThread(thisThreadState);
}

// Asuming we have a valid thread state, release the Python lock.
void PyXPCOM_InterpreterLock_Release()
{
	ThreadData *pData = (ThreadData *)PR_GetThreadPrivate(tlsIndex);
	NS_ABORT_IF_FALSE(pData, "Have no thread data for this thread!");
	PyThreadState *thisThreadState = pData->ts;
	PyEval_ReleaseThread(thisThreadState);
}

// Free the thread state for the current thread
// (Presumably previously create with a call to
// PyXPCOM_ThreadState_Ensure)
void PyXPCOM_ThreadState_Free()
{
	ThreadData *pData = (ThreadData *)PR_GetThreadPrivate(tlsIndex);
	if (!pData) return;
	PyThreadState *thisThreadState = pData->ts;
	PyThreadState_Delete(thisThreadState);
	PR_SetThreadPrivate(tlsIndex, NULL);
	nsAllocator::Free(pData);
}

void PyXPCOM_ThreadState_Clear()
{
	ThreadData *pData = (ThreadData *)PR_GetThreadPrivate(tlsIndex);
	PyThreadState *thisThreadState = pData->ts;
	PyThreadState_Clear(thisThreadState);
}

////////////////////////////////////////////////////////////
// Lock/exclusion global functions.
//

void PyXPCOM_AcquireGlobalLock(void)
{
	NS_PRECONDITION(g_lockMain != nsnull, "Cant acquire a NULL lock!");
	PR_Lock(g_lockMain);
}
void PyXPCOM_ReleaseGlobalLock(void)
{
	NS_PRECONDITION(g_lockMain != nsnull, "Cant release a NULL lock!");
	PR_Unlock(g_lockMain);
}

void PyXPCOM_DLLAddRef(void)
{
	// Must be thread-safe, although cant have the Python lock!
	CEnterLeaveXPCOMFramework _celf;
	PRInt32 cnt = PR_AtomicIncrement(&g_cLockCount);
	if (cnt==1) { // First call 
		if (!Py_IsInitialized()) {
			Py_Initialize();
			// Make sure our Windows framework is all setup.
			PyXPCOM_Globals_Ensure();
			// Make sure we have _something_ as sys.argv.
			if (PySys_GetObject("argv")==NULL) {
				PyObject *path = PyList_New(0);
				PyObject *str = PyString_FromString("");
				PyList_Append(path, str);
				PySys_SetObject("argv", path);
				Py_XDECREF(path);
				Py_XDECREF(str);
			}

			// Must force Python to start using thread locks, as
			// we are free-threaded (maybe, I think, sometimes :-)
			PyEval_InitThreads();
			// Release Python lock, as first thing we do is re-get it.
			ptsGlobal = PyEval_SaveThread();
			// NOTE: We never finalize Python!!
		}
	}
}
void PyXPCOM_DLLRelease(void)
{
	PR_AtomicDecrement(&g_cLockCount);
}

extern "C" PRBool _init(void)
{
	PRStatus status;
	g_lockMain = PR_NewLock();
	status = PR_NewThreadPrivateIndex( &tlsIndex, NULL );
	NS_WARN_IF_FALSE(status==0, "Could not allocate TLS storage");
	if (NS_FAILED(status)) {
		PR_DestroyLock(g_lockMain);
		return PR_FALSE;
	}
	return PR_TRUE;
}

extern "C" void _fini(void)
{
	PR_DestroyLock(g_lockMain);
	// I can't locate a way to kill this - 
	// should I pass a dtor to PR_NewThreadPrivateIndex??
	// TlsFree(tlsIndex);
}

#ifdef XP_WIN

extern "C" __declspec(dllexport)
BOOL WINAPI DllMain(HANDLE hInstance, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason) {
		case DLL_PROCESS_ATTACH: {
			if (!_init())
				return FALSE;
			break;
		}
		case DLL_PROCESS_DETACH: 
		{
			_fini();
			break;
		}
		default:
			break;
	}
	return TRUE;    // ok
}
#endif // XP_WIN

namespace js {

ThreadContext *
ThreadContext::current() {
#ifdef JS_THREADSAFE_ION
    return (ThreadContext*) PR_GetThreadPrivate(ThreadPrivateIndex);
#else
    return NULL;
#endif
}

}

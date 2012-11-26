namespace js {

ForkJoinSlice *
ForkJoinSlice::current() {
#ifdef JS_THREADSAFE
    return (ForkJoinSlice*) PR_GetThreadPrivate(ThreadPrivateIndex);
#else
    return NULL;
#endif
}

}

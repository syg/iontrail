namespace js {

void* Allocator::malloc_(size_t bytes) {
    return compartment->rt->malloc_(bytes, compartment);
}

void* Allocator::calloc_(size_t bytes) {
    return compartment->rt->calloc_(bytes, compartment);
}

void* Allocator::realloc_(void* p, size_t bytes) {
    return compartment->rt->realloc_(p, bytes, compartment);
}

void* Allocator::realloc_(void* p, size_t oldBytes, size_t newBytes) {
    return compartment->rt->realloc_(p, oldBytes, newBytes, compartment);
}

template <class T> T *Allocator::pod_malloc() {
    return compartment->rt->pod_malloc<T>(compartment);
}

template <class T> T *Allocator::pod_calloc() {
    return compartment->rt->pod_calloc<T>(compartment);
}

template <class T> T *Allocator::pod_malloc(size_t numElems) {
    return compartment->rt->pod_malloc<T>(numElems, compartment);
}

template <class T> T *Allocator::pod_calloc(size_t numElems) {
    return compartment->rt->pod_calloc<T>(numElems, compartment);
}

}

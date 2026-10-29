// Routes all C++ allocations (operator new/delete) of the executable through
// mimalloc. Must be linked into exactly one translation unit per executable.
#include <mimalloc-new-delete.h>

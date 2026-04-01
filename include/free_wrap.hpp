#pragma once

#include <cstdlib>

struct freewrapper {
    template <class T> void operator()(T *ptr) const noexcept { free(ptr); }
};

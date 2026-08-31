#include "ConfigMutation.hpp"

#include <QRecursiveMutex>

namespace {
    QRecursiveMutex configMutationMutex;
}

namespace NekoGui_ConfigMutation {
    Guard::Guard(bool wait) {
        if (wait) {
            configMutationMutex.lock();
            locked = true;
        } else {
            locked = configMutationMutex.tryLock();
        }
    }

    Guard::~Guard() {
        if (locked) configMutationMutex.unlock();
    }

    bool Guard::acquired() const {
        return locked;
    }
} // namespace NekoGui_ConfigMutation

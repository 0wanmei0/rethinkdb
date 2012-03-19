#ifndef ARCH_RUNTIME_STARTER_HPP_
#define ARCH_RUNTIME_STARTER_HPP_

// Implementation in runtime.cc.

#include "errors.hpp"
#include <boost/function.hpp>

/* `run_in_thread_pool()` starts a RethinkDB thread pool, runs the given
function in a coroutine inside of it, waits for the function to return, and then
shuts down the thread pool. */

void run_in_thread_pool(const boost::function<void()>& fun, int num_threads = 1);

#endif  // ARCH_RUNTIME_STARTER_HPP_

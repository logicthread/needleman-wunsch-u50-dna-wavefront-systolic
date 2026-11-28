/* hls_task.h -- host-side STAND-IN for hls::task (see hls_stream.h for the caveats).
 * Vitis csim runs each task function in its own thread, called repeatedly; this does the same.
 * Threads are detached and never joined: the process ends with exit(). */
#ifndef HLS_TASK_STUB_H
#define HLS_TASK_STUB_H
#include <thread>
#include <functional>
#include "hls_stream.h"
namespace hls {
class task {
  public:
    template <class F, class... A> task(F f, A &...a) {
        std::thread([f, &a...] { for (;;) f(a...); }).detach();
    }
};
}  // namespace hls
#define HLS_TASK static hls::task
#define HLS_TASK_STREAM static hls::stream
#endif

/* hls_stream.h -- host-side STAND-IN for Xilinx hls::stream, for compiling the kernel on a machine without
 * Vitis. Semantics follow the Vitis csim model: unbounded FIFO (depth is ignored, full() is always false),
 * blocking read(), non-blocking empty()/read_nb()/write_nb(). It is NOT the Xilinx library and proves nothing
 * about RTL, timing, FIFO sizing or deadlock freedom -- only the kernel's functional behaviour. */
#ifndef HLS_STREAM_STUB_H
#define HLS_STREAM_STUB_H
#include <condition_variable>
#include <deque>
#include <mutex>
namespace hls {
template <class T, int DEPTH = 0> class stream;
template <class T> class stream<T, 0> {
  public:
    stream() {}
    stream(const char *) {}
    stream(const stream &) = delete;
    stream &operator=(const stream &) = delete;
    void write(const T &v) { { std::lock_guard<std::mutex> g(m_); q_.push_back(v); } cv_.notify_one(); }
    T read() { std::unique_lock<std::mutex> g(m_); cv_.wait(g, [&] { return !q_.empty(); }); T v = q_.front(); q_.pop_front(); return v; }
    void read(T &v) { v = read(); }
    bool read_nb(T &v) { std::lock_guard<std::mutex> g(m_); if (q_.empty()) return false; v = q_.front(); q_.pop_front(); return true; }
    bool write_nb(const T &v) { write(v); return true; }
    bool empty() { std::lock_guard<std::mutex> g(m_); return q_.empty(); }
    bool full() { return false; }
    unsigned size() { std::lock_guard<std::mutex> g(m_); return (unsigned)q_.size(); }
    stream &operator<<(const T &v) { write(v); return *this; }
    stream &operator>>(T &v) { v = read(); return *this; }
  private:
    std::mutex m_; std::condition_variable cv_; std::deque<T> q_;
};
template <class T, int DEPTH> class stream : public stream<T, 0> {
  public:
    stream() {}
    stream(const char *) {}
};
}  // namespace hls
#endif

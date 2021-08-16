/**
 * DAIL in Alibaba Group
 *
 */

#include "my_thread_pool.hh"

#include <memory>

work_thread::work_thread(unsigned worker_id)
    : _work_item()
    , _start_eventfd(0)
    , _worker([this, worker_id] { work(worker_id); }) {
}

work_thread::~work_thread() {
    _stopped.store(true, std::memory_order_relaxed);
    _start_eventfd.signal(1);
    _worker.join();
}

void work_thread::work(unsigned int worker_id) {
    const std::string thread_name = "resource_thread[" + std::to_string(worker_id) + "]";
    pthread_setname_np(pthread_self(), thread_name.c_str());
    sigset_t mask;
    sigfillset(&mask);
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    seastar::throw_pthread_error(r);
    while (true) {
        uint64_t count;
        auto read_bytes = ::read(_start_eventfd.get_read_fd(), &count, sizeof(count));
        assert(read_bytes == sizeof(count));
        if (_stopped.load(std::memory_order_relaxed)) {
            break;
        }
        auto wi = _work_item.release();
        wi->process();
        auto from_shard_id = wi->_from_shard;
        my_thread_pool::complete_work_item(wi);
        my_thread_pool::return_worker(this);
        if (my_thread_pool::_reactors_idle[from_shard_id].load(std::memory_order_seq_cst)) {
            /// Here the work thread need to wake up reactor.
            /// So we need reactor::wakeup() to be public,
            /// Or seastar can provide us with other ways to accomplish this.
            _employer->wakeup();
        }
    }
}

unsigned my_thread_pool::_queue_length = 16;
unsigned my_thread_pool::_worker_num = 4;
int64_t my_thread_pool::_sleep_duration_in_microseconds = 500;
std::unique_ptr<boost::lockfree::queue<work_thread*>> my_thread_pool::_threads;
std::unique_ptr<my_thread_pool::completed_queue[], my_thread_pool::completed_queue_deleter> my_thread_pool::_completed;
std::unique_ptr<std::atomic<bool>[], my_thread_pool::atomic_flag_deleter> my_thread_pool::_reactors_idle;

void my_thread_pool::completed_queue_deleter::operator()(my_thread_pool::completed_queue* q) const {
    ::operator delete[](q);
}

void my_thread_pool::atomic_flag_deleter::operator()(std::atomic<bool> *flags) const {
    ::operator delete[](flags);
}

void my_thread_pool::configure() {
    // Create completed queues
    _completed = decltype(my_thread_pool::_completed){
        reinterpret_cast<completed_queue*>(operator new[] (sizeof(completed_queue{_queue_length}) * seastar::smp::count)),
        completed_queue_deleter{}};
    for (unsigned i = 0; i < seastar::smp::count; i++) {
        new (&my_thread_pool::_completed[i]) completed_queue(_queue_length);
    }
    // Create reactor idle flags
    _reactors_idle = decltype(my_thread_pool::_reactors_idle){
        reinterpret_cast<std::atomic<bool>*>(operator new[] (sizeof(std::atomic<bool>) * seastar::smp::count)),
        atomic_flag_deleter{}};
    for (unsigned i = 0; i < seastar::smp::count; i++) {
        new (&my_thread_pool::_reactors_idle[i]) std::atomic<bool>(false);
    }
    // Create initial work threads
    _threads = std::make_unique<boost::lockfree::queue<work_thread*>>(_queue_length);
    for (unsigned i = 0; i < _worker_num; i++) {
        _threads->push(new work_thread(i));
    }
}

void my_thread_pool::stop() {
    _threads->consume_all([&] (work_thread* wt) {
        delete wt;
    });
    for (unsigned i = 0; i < seastar::smp::count; i++) {
        assert(_completed[i].empty());
    }
}

void my_thread_pool::complete_work_item(work_thread::work_item *wi) {
    _completed[wi->_from_shard].push(wi);
}

void my_thread_pool::return_worker(work_thread* wt) {
    _threads->push(wt);
}

bool my_thread_pool::poll_queues() {
    auto nr = _completed[seastar::this_shard_id()].consume_all([] (work_thread::work_item* wi) {
        wi->complete();
        delete wi;
    });
    return nr;
}

bool my_thread_pool::pure_poll_queues() {
    return !_completed[seastar::this_shard_id()].empty();
}

void my_thread_pool::enter_interrupt_mode() {
    _reactors_idle[seastar::this_shard_id()].store(true, std::memory_order_seq_cst);
}

void my_thread_pool::exit_interrupt_mode() {
    _reactors_idle[seastar::this_shard_id()].store(false, std::memory_order_relaxed);
}

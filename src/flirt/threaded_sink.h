#ifndef FLIRT_THREADED_SINK_H
#define FLIRT_THREADED_SINK_H

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "flirt/message_batch.h"

namespace flirt
{

// Runs a batch consumer in its own thread behind a bounded FIFO. Batches are
// handed over by swapping them with a recycled one, so stream order and batch
// boundaries are unchanged. A Batch is default constructible, movable and has
// clear(), which empties it but may keep its allocations.
//
// An exception on either side ends both: a consumer exception wakes a producer
// blocked in push() and is rethrown there or by finish(); after a producer
// exception the destructor stops the consumer, drops queued batches and joins
// it. The consumer thread is joined by finish() or at the latest by the
// destructor, so objects the consumer uses must outlive this one.
template<typename Batch>
class BasicThreadedSink
{
    public:
        using Consumer = std::function<void (Batch &batch)>;

        explicit BasicThreadedSink(Consumer consumer, size_t capacity = 16)
            : consumer_(std::move(consumer))
            , capacity_(capacity ? capacity : 1)
            , thread_([this] { run(); })
        { }

        ~BasicThreadedSink() { stop(); }

        BasicThreadedSink(const BasicThreadedSink &) = delete;
        BasicThreadedSink &operator=(const BasicThreadedSink &) = delete;

        // Producer: queues the batch's contents, leaving `batch` empty (with
        // recycled allocations). Blocks while the queue is full. Rethrows the
        // consumer's exception.
        void push(Batch &batch)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return queue_.size() < capacity_ || error_; });

            if (error_)
                std::rethrow_exception(error_);

            Batch queued;
            if (!spare_.empty())
            {
                queued = std::move(spare_.front());
                spare_.pop_front();
            }
            std::swap(queued, batch);
            batch.clear();
            queue_.push_back(std::move(queued));
            changed_.notify_all();
        }

        // Producer, after the last push(): waits until every batch has been
        // consumed, joins the consumer and rethrows its exception.
        void finish()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                finishing_ = true;
            }
            changed_.notify_all();

            if (thread_.joinable())
                thread_.join();

            if (error_)
                std::rethrow_exception(error_);
        }

    private:
        void stop() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                aborted_ = true;
                queue_.clear();
            }
            changed_.notify_all();

            if (thread_.joinable())
                thread_.join();
        }

        void run()
        {
            std::unique_lock<std::mutex> lock(mutex_);

            while (true)
            {
                changed_.wait(lock, [this] { return !queue_.empty() || finishing_ || aborted_; });

                if (aborted_ || queue_.empty())
                    return; // aborted, or finishing with nothing left

                Batch batch = std::move(queue_.front());
                queue_.pop_front();
                changed_.notify_all(); // room for a blocked producer
                lock.unlock();

                try
                {
                    consumer_(batch);
                }
                catch (...)
                {
                    lock.lock();
                    error_ = std::current_exception();
                    queue_.clear();
                    changed_.notify_all();
                    return;
                }

                batch.clear();
                lock.lock();
                spare_.push_back(std::move(batch));
            }
        }

        Consumer consumer_;
        const size_t capacity_;
        std::mutex mutex_;
        std::condition_variable changed_;
        std::deque<Batch> queue_, spare_;
        bool finishing_ = false;    // no more batches will come
        bool aborted_ = false;      // drop queued batches and stop
        std::exception_ptr error_;  // the consumer's exception
        std::thread thread_;        // last: starts once the members above exist
};

// The consumer of run_pipeline()'s message batches (ROOT output).
using ThreadedSink = BasicThreadedSink<MessageBatch>;

}

#endif

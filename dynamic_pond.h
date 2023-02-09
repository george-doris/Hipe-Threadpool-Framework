#pragma once
#include "header.h"


namespace hipe {


/**
 * @brief A dynamic thread pond
 */
class DynamicThreadPond
{

    // stop the pond
    bool stop = {false};

    // thread number
    int thread_numb = {0};

    // waiting for tasks
    bool waiting = {false};

    // task number
    std::atomic_int total_tasks = {0};

    // shared task queue
    std::queue<HipeTask> shared_tq = {};

    // locker shared by threads
    std::mutex shared_locker;

    // cv to awake the paused thread
    std::condition_variable awake_cv;

    // task done
    std::condition_variable task_done_cv;

    // dynamic thread pond
    std::vector<std::thread> pond;

    // the shrinking number of threads
    std::atomic_int shrink_numb = {0};

    // deleted thread index
    std::queue<int> deleted_threads;

    // number of the tasks loaded by thread
    std::atomic_int task_loaded = {0};


public:
    /**
     * @brief construct DynamicThreadPond
     * @param tnumb initial thread number
     */
    explicit DynamicThreadPond(int tnumb = 0)
      : thread_numb(tnumb) {
        assert(tnumb >= 0);
        for (int i = 0; i < thread_numb; ++i) {
            pond.emplace_back(&DynamicThreadPond::worker, this, i);
        }
    }

    ~DynamicThreadPond() {
        if (!stop) {
            close();
        }
    }

public:
    /**
     * @brief close the pond
     * Tasks blocking in the queue will be thrown.
     */
    void close() {
        {
            HipeUniqGuard locker(shared_locker);
            stop = true;
            awake_cv.notify_all();
        }
        for (auto &thread : pond) {
            thread.join();
        }
    }

    /**
     * @brief add threads
     * @param tnumb thread number
     * The pond will recycle the deleted thread firstly. But if there are  deleted threads,
     * the pond will expand through creating new thread.
     */
    void addThreads(int tnumb = 1) {
        int idx = 0;
        assert(tnumb >= 0);
        while (tnumb--) {
            shared_locker.lock();
            if (!deleted_threads.empty()) {
                idx = deleted_threads.front();
                deleted_threads.pop();
                shared_locker.unlock();

                pond[idx].join();
                pond[idx] = std::thread(&DynamicThreadPond::worker, this, idx);

            } else {
                shared_locker.unlock();
                int index = static_cast<int>(pond.size());
                pond.emplace_back(&DynamicThreadPond::worker, this, index);
            }
            thread_numb++;
        }
    }


    /**
     * @brief delete some threads
     * @param tnumb thread number
     * If there are not enough threads, the Hipe will throw error.
     * The deletion will not happen immediately, but just notify that
     * there are some threads need to be deleted, as a result,
     * it is nonblocking.
     */
    void delThreads(int tnumb = 1) {
        assert((tnumb <= thread_numb) && (tnumb >= 0));
        shrink_numb += tnumb;
        thread_numb -= tnumb;
        awake_cv.notify_all();
    }

    /**
     * @brief adjust thread number to target
     * @param target_tnumb target thread number
     */
    void adjustThreads(int target_tnumb) {
        assert(target_tnumb >= 0);
        if (target_tnumb > thread_numb) {
            addThreads(target_tnumb - thread_numb);
            return;
        }
        if (target_tnumb < thread_numb) {
            delThreads(thread_numb - target_tnumb);
            return;
        }
    }


    // get task number of the pond, tasks in progress are also counted.
    int getTasksRemain() {
        return total_tasks.load();
    }

    // get number of the tasks loaded by thread
    int getTaskLoaded() {
        return task_loaded.load();
    }

    /**
     * reset the number of tasks loaded by thread and return the old value (atomic operation)
     * @return the old value
     */
    int resetTaskLoaded() {
        return task_loaded.exchange(0);
    }

    // get thread number now
    int getThreadNumb() const {
        return thread_numb;
    }

    // wait for tasks in the pond done
    void waitForTasks() {
        waiting = true;
        HipeUniqGuard locker(shared_locker);
        task_done_cv.wait(locker, [this] { return !total_tasks; });
        waiting = false;
    }

    /**
     * @brief submit task
     * @param foo An runnable object
     */
    template <typename Runnable_>
    void submit(Runnable_ &&foo) {
        {
            HipeLockGuard lock(shared_locker);
            shared_tq.emplace(std::forward<Runnable_>(foo));
            ++total_tasks;
        }
        awake_cv.notify_one();
    }

    /**
     * @brief submit task and get return
     * @param foo a runnable object
     * @return a future
     */
    template <typename Runnable_>
    auto submitForReturn(Runnable_ &&foo) -> std::future<typename std::result_of<Runnable_()>::type> {
        using RT = typename std::result_of<Runnable_()>::type;
        std::packaged_task<RT()> pack(std::forward<Runnable_>(foo));
        std::future<RT> fut(pack.get_future());

        {
            HipeLockGuard lock(shared_locker);
            shared_tq.emplace(std::move(pack));
            ++total_tasks;
        }
        awake_cv.notify_one();
        return fut;
    }

    /**
     * submit in a batch and the task container must override "[]"
     * @param cont task container
     * @param size the size of the container
     */
    template <typename Container_>
    void submitInBatch(Container_ &cont, size_t size) {
        {
            HipeLockGuard lock(shared_locker);
            total_tasks += static_cast<int>(size);
            for (size_t i = 0; i < size; ++i) {
                shared_tq.emplace(std::move(cont[i]));
            }
        }
        awake_cv.notify_all();
    }


private:
    // working threads' default loop
    void worker(int index) {
        // task container
        HipeTask task;

        while (!stop) {
            // wait notify
            HipeUniqGuard locker(shared_locker);
            awake_cv.wait(locker, [this] { return !shared_tq.empty() || stop || shrink_numb > 0; });

            if (shrink_numb) {
                shrink_numb--;
                deleted_threads.push(index);
                break;
            }
            if (!stop) {
                task = std::move(shared_tq.front());
                shared_tq.pop();
                locker.unlock();

                task_loaded++;

                // just invoke
                util::invoke(task);
                --total_tasks;

                if (waiting) {
                    HipeUniqGuard inner_locker(shared_locker);
                    task_done_cv.notify_one();
                }
            }
        }
    }
};


} // namespace hipe
#include <stdexec/execution.hpp>
//
#include <exec/when_any.hpp>
#include <functional>
#include <iostream>
#include <set>

class timed_run_loop_scheduler;

class timed_run_loop {
   public:
    friend class timed_run_loop_scheduler;

    template <stdexec::stoppable_token StopToken>
    auto run(StopToken stop_token) noexcept -> void;

    auto scheduler() noexcept -> timed_run_loop_scheduler;

   private:
    struct timer_base {
        explicit timer_base(
            std::chrono::system_clock::time_point deadline) noexcept
            : deadline_{deadline} {}

        virtual void set_value() noexcept = 0;
        virtual void set_error(std::exception_ptr err) noexcept = 0;
        virtual void set_stopped() noexcept = 0;

        std::chrono::system_clock::time_point deadline_;

       protected:
        ~timer_base() = default;
    };

    struct stop_request {
        timer_base* target;
    };

    struct timer_op_handle {
        timer_base* pointer_;

        friend auto operator<(timer_op_handle lhs, timer_op_handle rhs) noexcept
            -> bool {
            return (lhs.pointer_->deadline_ < rhs.pointer_->deadline_) ||
                   (lhs.pointer_->deadline_ == rhs.pointer_->deadline_ &&
                    std::less<>{}(lhs.pointer_, rhs.pointer_));
        }

        friend bool operator==(timer_op_handle lhs,
                               timer_op_handle rhs) noexcept = default;
    };

    class timer_queue {
       public:
        auto push(timer_op_handle timer) -> void { timers_.insert(timer); }

        auto top() const -> timer_op_handle {
            assert(timers_.begin() != timers_.end());
            return *timers_.begin();
        }

        auto pop() -> void {
            assert(timers_.begin() != timers_.end());
            timers_.erase(timers_.begin());
        }

        auto remove(timer_op_handle timer) -> bool {
            auto pos = std::find(timers_.begin(), timers_.end(), timer);
            if (pos != timers_.end()) {
                timers_.erase(pos);
                return true;
            }
            return false;
        }

        auto empty() const noexcept -> bool { return timers_.empty(); }

       private:
        std::set<timer_op_handle> timers_;
    };

    // this mutex is used to synchronize with all remote threads
    std::mutex mutex_{};
    // the condition variable will be used to wait for events to happen
    std::condition_variable cv_{};
    // A queue for newly scheduled operations, written to by remote threads
    // and emptied by driving threads
    std::vector<std::variant<timer_base*, stop_request>> submission_queue_{};
};

template <stdexec::stoppable_token StopToken>
auto timed_run_loop::run(StopToken stop_token) noexcept -> void {
    // as long as this is false the run method will wait for work even if
    // the it doesn't process any work.
    // once this boolean is true run() will return as soon as all queues
    // get empty
    bool stop_requested{false};

    // First, we create a callback to set the boolean flag to true
    // once a stop request was made
    struct on_stop {
        timed_run_loop* self;
        bool& stopped;

        void operator()() const noexcept {
            {
                // if this throws I want to terminate the program
                std::lock_guard lock{self->mutex_};
                stopped = true;
            }
            self->cv_.notify_one();
        }
    };

    using CallbackT = typename StopToken::template callback_type<on_stop>;
    CallbackT callback(stop_token, on_stop{this, stop_requested});
    // we create a local copy of a submission queue that will be processsed
    // and use a timer_queue to manage timers sorted by their deadline
    std::vector<std::variant<timer_base*, stop_request>> submission_queue{};
    timer_queue timers{};
    while (true) {
        // in every iteration of the run loop we will empty the local submission
        // queue. We immediately complete ready ops and push all others into the
        // timer queue
        auto now = std::chrono::system_clock::now();
        for (const std::variant<timer_base*, stop_request>& op :
             submission_queue) {
            if (op.index() == 0) {
                timer_base* timer = *std::get_if<0>(&op);
                if (timer->deadline_ <= now) {
                    timer->set_value();
                } else {
                    try {
                        timers.push(timer_op_handle{timer});
                    } catch (...) {
                        timer->set_error(std::current_exception());
                    }
                }
            } else if (op.index() == 1) {
                stop_request cancellation = *std::get_if<1>(&op);
                // if the target submission has not completed yet we make it
                // complete now. if the target is not in the timer queue then it
                // has already been completed. note, the cancellation submission
                // always completes after the target submission completes.
                if (cancellation.target &&
                    timers.remove(timer_op_handle{cancellation.target})) {
                    cancellation.target->set_stopped();
                }
            }
        }
        submission_queue.clear();
        // After we emptied the submission queue we check whether any ready
        // timers are waiting for its completion in the timer queue
        while (!timers.empty()) {
            timer_op_handle timer = timers.top();
            if (timer.pointer_->deadline_ <= now) {
                timers.pop();
                timer.pointer_->set_value();
            } else {
                break;
            }
        }
        // from here on we take a lock. no mutations from remote are possible
        std::unique_lock lock{mutex_};
        if (stop_requested) {
            // stop requested? Let's quit.
            lock.unlock();
            while (!timers.empty()) {
                timer_op_handle timer = timers.top();
                timers.pop();
                timer.pointer_->set_stopped();
            }
            return;
        } else if (!submission_queue_.empty()) {
            // new submissions are available lets swap the queues and
            // proceed from the beginning
            std::swap(submission_queue, submission_queue_);
        } else if (timers.empty()) {
            // no stop but there are no timers? Let's wait.
            cv_.wait(lock);
        } else if (!timers.empty()) {
            // timers available? Let's wait for the next deadline.
            auto deadline = timers.top().pointer_->deadline_;
            cv_.wait_until(lock, deadline);
        }
    }
}

struct timed_scheduler_t /* : stdexec::scheduler_t */ {};

class timed_run_loop_scheduler {
    template <class Receiver>
    struct operation;

    struct sender {
        using sender_concept = stdexec::sender_t;

        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(), stdexec::set_error_t(std::exception_ptr),
            stdexec::set_stopped_t()>;

        template <stdexec::receiver_of<completion_signatures> Receiver>
        auto connect(Receiver receiver) const noexcept -> operation<Receiver> {
            return {std::move(receiver), deadline, *run_loop};
        }

        template <std::same_as<stdexec::set_value_t> Tag>
        auto get_completion_scheduler() const noexcept
            -> timed_run_loop_scheduler;

        timed_run_loop* run_loop;
        std::chrono::system_clock::time_point deadline;
    };

   public:
    using scheduler_concept = timed_scheduler_t;

    timed_run_loop_scheduler(timed_run_loop& run_loop) : run_loop_{&run_loop} {}

    auto now() const noexcept -> std::chrono::system_clock::time_point {
        return std::chrono::system_clock::now();
    }

    auto schedule_at(std::chrono::system_clock::time_point deadline)
        const noexcept -> sender {
        return {run_loop_, deadline};
    }

    auto schedule_after(
        std::chrono::system_clock::duration duration) const noexcept {
        return stdexec::let_value(stdexec::just(), [*this, duration] {
            return schedule_at(now() + duration);
        });
    }

    auto schedule() const noexcept -> sender { return schedule_at(now()); }

    friend bool operator==(timed_run_loop_scheduler,
                           timed_run_loop_scheduler) noexcept = default;

   private:
    template <class Receiver>
    struct operation : timed_run_loop::timer_base {
        using operation_concept = stdexec::operation_state_t;

        using stop_token_type =
            stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

        struct on_stop {
            operation* self;
            void operator()() const noexcept try {
                self->stop_requested_ = true;
                {
                    std::lock_guard lock{self->run_loop_.mutex_};
                    self->run_loop_.submission_queue_.emplace_back(
                        timed_run_loop::stop_request{self});
                }
                self->run_loop_.cv_.notify_one();
            } catch (...) {
                // ignore stop request
            }
        };

        using stop_callback =
            typename stop_token_type::template callback_type<on_stop>;

        Receiver receiver_;
        timed_run_loop& run_loop_;
        bool stop_requested_{false};
        std::optional<stop_callback> stop_callback_;

        template <class CompletionFn, class... Args>
        void do_complete(CompletionFn set_completion, Args&&... args) noexcept {
            // here we desroy the stop callback, which synchronizes with a
            // parallel call to it
            stop_callback_.reset();
            // if we reach this line then there can be no other threads that
            // change the stop_requested_ member variable.
            // any potential modification on stop_requested_ happens-before we
            // destroy the stop_callback
            if (stop_requested_) {
                stdexec::set_stopped(std::move(receiver_));
            } else {
                set_completion(std::move(receiver_),
                               std::forward<Args>(args)...);
            }
        }

        void set_value() noexcept override { do_complete(stdexec::set_value); }

        void set_stopped() noexcept override {
            // here we destroy the callback to not trigger any unneeded
            // submission of a stop request after the completion function is
            // being called
            stop_callback_.reset();
            stdexec::set_stopped(std::move(receiver_));
        }

        void set_error(std::exception_ptr eptr) noexcept override {
            do_complete(stdexec::set_error, std::move(eptr));
        }

        operation(Receiver receiver,
                  std::chrono::system_clock::time_point deadline,
                  timed_run_loop& run_loop) noexcept
            : timer_base{deadline},
              receiver_{std::move(receiver)},
              run_loop_{run_loop} {}

        void start() noexcept try {
            auto stop_token =
                stdexec::get_stop_token(stdexec::get_env(receiver_));
            stop_callback_.emplace(stop_token, on_stop{this});
            {
                std::lock_guard lock{run_loop_.mutex_};
                run_loop_.submission_queue_.push_back(this);
            }
            run_loop_.cv_.notify_one();
        } catch (...) {
            stdexec::set_error(std::move(receiver_), std::current_exception());
        }
    };

    timed_run_loop* run_loop_;
};

auto timed_run_loop::scheduler() noexcept -> timed_run_loop_scheduler {
    return timed_run_loop_scheduler(*this);
}

using namespace std::chrono_literals;

auto async_main(timed_run_loop_scheduler scheduler) -> stdexec::sender auto {
    auto t0 = std::chrono::system_clock::now();
    auto print_tid = [t0](std::string prefix, stdexec::sender auto schedule) {
        return stdexec::then(schedule, [t0, prefix] {
            auto duration = std::chrono::system_clock::now() - t0;
            std::cout << prefix
                      << ": This thread id: " << std::this_thread::get_id()
                      << ", duration: " << duration.count() << "ns\n";
        });
    };

    auto print_after_500ms = print_tid("A", scheduler.schedule_after(500ms));
    auto print_after_1s = print_tid("B", scheduler.schedule_after(1s));
    return print_tid("C", exec::when_any(print_after_500ms, print_after_1s));
}

int main() {
    using namespace std::chrono_literals;
    timed_run_loop run_loop{};
    auto scheduler = run_loop.scheduler();

    stdexec::sender auto drive = stdexec::then(
        stdexec::get_stop_token(), [&](auto token) { run_loop.run(token); });

    stdexec::sync_wait(exec::when_any(async_main(scheduler), drive));
}
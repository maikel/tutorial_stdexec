# Introduction

I have been closely following the development of Senders and Receivers for years now, starting with the libunifex project. During that time, I have applied the concepts in various contexts, ranging from numerical simulations to networking services and clients. One could say that I am quite deep into the topic and motivated to share my experience.

Although `std::execution` may still fail to fully land in C++26, it was finally voted into the standard, and I believe we still lack a good amount of resources to learn about it. This article is my attempt to make some of its core concepts, such as cancellation, more accessible.

Throughout the article, I sometimes use the namespace `stdexec::` instead of `std::execution`. This is because I use the reference implementation for the godbolt links. All the code I provide in this article is Unlicensed and in the Public Domain. I will try to keep it simple and focused on the properties of Senders and Receivers, although I feel that I haven't fully succeeded in doing so. The resulting code is not optimized for performance, compile times, or similar goals.

`std::execution` ships with three kinds of sender algorithms:

1. *Sender factories*, which return a sender given some non-sender arguments.
2. *Sender adaptors*, which take one or multiple senders and produce a new sender.
3. *Sender consumers*, which take a sender and return a non-sender result.

In this article, I want to focus on sender factories because I like them the most. I plan to write some follow-up articles about other topics related to `std::execution` as well. In fact, I personally think there are many topics worth exploring in articles, such as customization, the relationship between senders and awaitables, how to type-erase them to improve compile times, how to make async resources, or how to extend senders (and receivers) to create asynchronous observable streams, similar to how it is done in the ReactiveX frameworks.

# Sender Factories

## A Very Basic Description of the Concepts

A sender describes asynchronous work. It will only produce an operation state that can be used to *start* some work if it is connected to a receiver, which plays the role of a callback. This laziness allows us to chain continuations without worrying about the synchronization of ongoing work or type-erasing the continuation. It provides us with an allocation-free framework to compose asynchronous algorithms, as the entire async call graph can be statically known. Another underlying principle is that of *structured concurrency*. 

Structured concurrency means that an operation is completed only once all its started child operations have been completed. One implication of structured concurrency is that of nested lifetimes of operation states, thereby eliminating the need to rely on shared pointers or other garbage collection methods to manage the involved lifetimes. Practically, it also means that you want inherent support for cancellation, i.e., to stop an already started operation by requesting a faster completion path.

To create a sender, you basically have to do two things:

1. Given a receiver type, define an operation state that is used to start the completion of the operation.
2. Describe the set of all possible completions that your operation state may potentially complete with. This set might depend on an *environment* type.

The second bullet point indicates that there is often some meta-programming involved if you are the author of a sender adaptor algorithm because you have to make type transformations that match your implementation.

Let’s start with a simple example by defining a sender that completes with the integer value 42.

```cpp
// We define an operation state that completes synchronously with
// the value 42 after it is started
template <class Receiver>
struct just_42_operation_state {
 // This tag is used to allow `stdexec::start` to use our member method
  using operation_concept = stdexec::operation_state_t;

 // This is our handle for the continuation
 Receiver receiver_;

 // calling start will immediately complete the receiver
  void start() noexcept {
    stdexec::set_value(std::move(receiver_), 42);
  }
};

// Here we define the sender, the description of work
struct just_42_t {
 // This tag is used to tell the sender CPOs that our member methods
 // are allowed to be used by them
  using sender_concept = stdexec::sender_t;

 // Our completion signatures are independent of the environment
 // Here we use a typedef to describe all the possible ways
 // that are used to complete a given receiver
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(int)>;

 // Here we 'connect' with a receiver and produce an operation state
 // that can be started
 //
 // NB. I assume that receivers are nothrow-move-constructible
  template <stdexec::receiver_of<completion_signatures> Receiver>
  auto connect(Receiver receiver) const noexcept
      -> just_42_operation_state<Receiver> {
    return {std::move(receiver)};
  }
};

int main()
{
    just_42_t just{};
    auto [value] = stdexec::sync_wait(just).value();
    return value;
}
```

[Link to godbolt](https://godbolt.org/z/eq76dexdr)

Providing a type list (`stdexec::completion_signatures<[...]>`) as a sender is necessary for some algorithms that, for example, want to store the completion results of an operation. 
Ideally, one would like to make those signatures depend on a receiver's type to access the complete type information available when we want to describe all the possible signatures of a concrete operation state.
Unfortunately, this has caused problems with recursive type checks in the past, thus we assume that completion signatures depend only on a so-called environment.

The mental model for an environment type is a bag of properties that can be queried, and these are part of every receiver type.
The empty class `struct empty_env {};` is a special case of an environment that provides no properties at all.
`std::execution` ships with several standardized queries, which include:

- `std::get_allocator`
- `std::get_stop_token`
- `stdexec::get_scheduler`
- `stdexec::get_completion_scheduler<stdexec::set_value_t>`
- and more...

The queries provided in the standard will also act as sender factories. This means that you can use them to access properties in the receiver's environment when composing algorithms. For example:

```cpp
auto was_stopped = stdexec::then(std::get_stop_token(), [](auto token) {
    return token.stop_requested();
});
```

We will utilize this later in the article.

The sender `just_42_t` is a fully-fledged `std::execution` sender and can be employed with all algorithms from `std::execution`.
The godbolt example uses the `sync_wait` algorithm, which connects the input sender with a receiver, starts the resulting operation, and blockingly waits for its completion.
The `sync_wait` algorithm does even more: it provides a scheduler via the receiver's environment, which can be used to post additional work if needed.
The scheduler provided by `sync_wait` uses the calling thread as an execution resource.

For educational purposes, let's create a hand-written receiver that writes the completion value of the sender to a variable. This will help us learn the basic structure of `std::execution` receivers.

```cpp
struct empty_env {};

struct just_int_receiver {
    // mark this class as a receiver
    using receiver_concept = stdexec::receiver_t;

    // our destination value
    int& value;

    // we only support one completion function
    void set_value(int val) const noexcept { value = val; }

    // and have no properties in our environment
    auto get_env() const noexcept -> empty_env { return {}; }
};

int main()
{
    just_42_t just{};
    int value = 0;
    // here we connect a sender and a receiver and make an operation state
    auto op = stdexec::connect(just, just_int_receiver{value});
    stdexec::start(op);
    // We know that our implementation here completes when start returns.
    // It is safe to just not wait for anything. Usually its not fine to do so.
    return value;
}
```
[Link to godbolt](https://godbolt.org/z/crqr5dG8K)

`just_int_receiver` is inadequate for general use.
It does not synchronize the completion value with the current thread in any way, and the above example works only because we know that the resulting operation state `op` completes inline when we call the operation's start method. However, we are utilizing all the information available to us, which is beneficial! This (unrealistic) example compiles to:

```assembly
main:
    mov     eax, 42
    ret
```

So the compiler is able to "see" through all the types, recognizing that we are not doing any real work.
Writing a sender factory algorithm typically does not require you to write your own receiver, so I won't dwell on them any longer.

# Schedulers

One of the goals of `std::execution` is to control where work is executed.
To achieve this, it introduces the notion of a scheduler, which is a handle to some execution resource, similar to how an allocator serves as a handle to memory resources.

A scheduler provides a schedule member method that returns a sender, which completes on the execution resource to which the scheduler refers.
Furthermore, the schedule-sender must provide the query `stdexec::get_completion_scheduler<stdexec::set_value_t>` and return a scheduler object for the targeted execution context.

This time, let’s create a very simple scheduler that creates a new thread each time it starts a new operation

```cpp
// This scheduler doesn't really point to any real context
// Calling the schedule() member method will call a sender which will spawn
// a new thread upon starting its operation.
struct jthread_scheduler {
    // using scheduler_concept = stdexec::scheduler_t;
    
    // The operation state owns a thread that will be started when calling the
    // start method
    template <class Receiver>
    struct operation {
        using operation_concept = stdexec::operation_state_t;

        Receiver receiver_;
        std::jthread thread_;

        // creating a jthread might throw an exception and we need to handle
        // that
        // note how the exceptional path does not complete on a new thread
        void start() noexcept try {
            thread_ = std::jthread(
                [this] { stdexec::set_value(std::move(receiver_)); });
        } catch (...) {
            stdexec::set_error(std::move(receiver_), std::current_exception());
        }
    };

    // This sender has no state at all since there is no real context-object
    // that it can refer to
    struct sender {
        using sender_concept = stdexec::sender_t;

        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_error_t(std::exception_ptr)>;

        template <std::same_as<stdexec::set_value_t> Tag>
        auto get_completion_scheduler() const noexcept -> jthread_scheduler {
            return {};
        }

        template <stdexec::receiver_of<completion_signatures> Receiver>
        auto connect(Receiver receiver) const noexcept -> operation<Receiver> {
            return {std::move(receiver)};
        }
    };

    // stdexec::schedule will look for this member method
    auto schedule() const noexcept -> sender { return {}; }

    // All schedulers are equal since there is no state to compare with
    friend bool operator==(const jthread_scheduler&,
                           const jthread_scheduler&) noexcept = default;
};
```

[Link to godbolt](https://godbolt.org/z/7Ynbqcdv6)

This example serves an educational purpose, but launching separate threads for each work item is rarely useful in practice.
Instead, you typically have some kind of multiplexer that interleaves the execution of multiple operations, and we will explore that next.
Before doing so, I want to extend the above scheduler with the capability of launching timers that complete after a specified deadline has expired. 

Let’s introduce a new tag `struct timed_scheduler_t : stdexec::scheduler_t {};` and implement a `schedule_at(deadline)` method that schedules a completion function to be executed at the specified time point.

```cpp
struct jthread_scheduler {
    using scheduler_concept = timed_scheduler_t;

    template <class Receiver>
    struct timed_operation {
        using operation_concept = stdexec::operation_state_t;

        Receiver receiver_;
        // now we have an additional state: the deadline!
        std::chrono::system_clock::time_point deadline_;
        std::jthread thread_;

        // the start method creates a thread that sleeps until the deadline has
        // expired
        void start() noexcept try {
            thread_ = std::jthread([this] {
                std::this_thread::sleep_until(deadline_);
                stdexec::set_value(std::move(receiver_));
            });
        } catch (...) {
            stdexec::set_error(std::move(receiver_), std::current_exception());
        }
    };

    struct timed_sender {
        using sender_concept = stdexec::sender_t;

        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(), stdexec::set_error_t(std::exception_ptr)>;

        // now we have an additional state: the deadline!
        std::chrono::system_clock::time_point deadline_;

        template <std::same_as<stdexec::set_value_t> Tag>
        auto get_completion_scheduler() const noexcept -> jthread_scheduler {
            return {};
        }

        template <stdexec::receiver_of<completion_signatures> Receiver>
        auto connect(Receiver receiver) const noexcept
            -> timed_operation<Receiver> {
            return {std::move(receiver), deadline_};
        }
    };

    auto now() const noexcept -> std::chrono::system_clock::time_point {
        return std::chrono::system_clock::now();
    }

    auto schedule_at(std::chrono::system_clock::time_point deadline)
        const noexcept -> timed_sender {
        return {deadline};
    }

    auto schedule_after(std::chrono::nanoseconds duration) const noexcept {
        // note, let_value does not propogate the get_completion_scheduler
        // but let's not care for the moment
        return stdexec::let_value(stdexec::just(), [this, duration] {
            return schedule_at(now() + duration);
        });
    }

    auto schedule() const noexcept -> timed_sender {
        return schedule_at(now());
    }

    friend bool operator==(const jthread_scheduler&,
                           const jthread_scheduler&) noexcept = default;
};
```

[Link to godbolt](https://godbolt.org/z/4ba1a8jW3)

The above example now allows us to schedule work that actually takes some time to complete.
While it may seem basic, there is a family of time-related algorithms—like setting timeouts for other operations where the notion of a `timed_scheduler` helps us to implement them.

Let's consider for a moment a generic timeout algorithm that takes any input sender and a timeout duration and it will complete with a timeout error if the timer expires before the input sender can complete.
The generic version of algorithm will have two child operations:

1. the operation associated with the input sender
2. and another timer operation

When one of the two operations complete a stop request for the second operation will be made.
In the case where the input sender completes in-time we need to stop the timer operation early. Remember, that we embrace structured concurrency and we need to wait for all child operations to complete before we can complete the whole timeout operation.


## Cancellation with Stoppable Tokens

Although we have `std::stop_token` available since C++20, `std::execution` generalizes this concept to accommodate more token types, such as `std::never_stop_token`, which never triggers a stop request. Stoppable tokens typically refer to a stop source and allow us to install a callback that is invoked whenever a cancellation is requested. This stop request, along with the subsequent invocation of the callback, can occur from any thread, making cancellation inherently racy and requiring extra caution.

One of the key advantages of stoppable tokens is the guarantee that the invocation of a stop callback does not race with its destruction. Specifically, destroying a callback object synchronizes with any concurrent invocation of the callback. This property is essential for implementing cancellation.

In the following snippet, we will replace the call to `std::this_thread::sleep_until` with a call to `std::condition_variable::wait_until`, which we will interrupt from a callback whenever a stop request is received.

```cpp
template <class Receiver>
struct timed_operation {
    using operation_concept = stdexec::operation_state_t;

    using stop_token_type =
        stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

    Receiver receiver_;
    std::chrono::system_clock::time_point deadline_;
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    struct on_stop {
        timed_operation* self;
        void operator()() const noexcept {
            {
                std::lock_guard lock{self->mutex_};
            }
            self->cv_.notify_one();
        }
    };

    // since the stop callback is the last member variable it will be
    // destroyed first and thus it is safe to reference any other member
    // variable
    using stop_callback =
        typename stop_token_type::template callback_type<on_stop>;
    std::optional<stop_callback> stop_callback_;

    // the start method creates a thread that sleeps until the deadline has
    // expired
    void start() noexcept try {
        auto stop_token =
            stdexec::get_stop_token(stdexec::get_env(receiver_));
        stop_callback_.emplace(stop_token, on_stop{this});
        thread_ = std::jthread([this, stop_token] {
            std::unique_lock lock{mutex_};
            bool stop_requested = cv_.wait_until(
                lock, deadline_,
                [stop_token] { return stop_token.stop_requested(); });
            lock.unlock();
            if (stop_requested) {
                stdexec::set_stopped(std::move(receiver_));
            } else {
                stdexec::set_value(std::move(receiver_));
            }
        });
    } catch (...) {
        stdexec::set_error(std::move(receiver_), std::current_exception());
    }
};

```

[Link to godbolt](https://godbolt.org/z/fobxETPWe)

I encourage you to try the godbolt link and play around with this scheduler.
The example in the godbolt link uses an algorithm that is not standardized but implemented in stdexec: `exec::when_any(senders...)`.
This algorithm starts all input senders and completes with the first completion value it receives.
Once one sender completes it issues a stop request for all the others and waits for all senders to complete.

Concluding the last example, we implemented basic timers with the capability to cancel them once they have been started.
Cancellation is implemented via a stop-callback that merely notifies the async operation to take a faster completion path.
Sometimes it is possible to do *synchronous cancellation* where an operation will be completed before the stop-callback returns.
In my experience, it is hard to make synchronous cancellation correct and I try to avoid it.

The next section focuses on getting rid of all the threads that we are starting and we implement an execution context that multiplexes multiple operations in one driving thread.

## A Run Loop with Support for Delayed Schedules

In the following section, we will develop a `timed_run_loop`

```cpp
class timed_run_loop {
public:
  friend class timed_run_loop_scheduler;

  template <stdexec::stoppable_token StopToken>
  auto run(StopToken stop_token) -> void;

  auto scheduler() noexcept -> timed_run_loop_scheduler;

private:
  // [...]
};
```

that has two public member methods:

1. `run()`, which blocks the calling thread, drives this event loop until someone requests to stop via the passed stop token and

2. `scheduler()` to access a scheduler object which can be used to post operations on this event loop

The implementation of our run loop uses one submission queue that contains pointers to operation states. 
Note that, once started, operation states are required to be immovable and their lifetime is maintained until any completion function of the connected receiver is called.
Thus, it is safe to hold and access pointers to operation states until they have been completed.
The submission queue is shared with every thread that schedules work onto the execution context and we simply synchronize the access with a mutex.

Within the run method, we allocate a local timer queue using a priority queue that is only accessed by the thread that drives the context.


```cpp
class timed_run_loop {
public:
  // [...]
private:
  enum class operation_type {
    timer, cancel
  };

  struct operation_base {
    void (*set_value_)(void* self) noexcept;
    operation_type type_;
    bool run_loop_stopped_;
  };

  struct timer_op_base : operation_base {
    std::chrono::system_clock::time_point deadline_;
  };

  struct cancellation_op_base : operation_base {
    timer_op_base* target;
  };

  // this mutex is used to synchronize with all remote threads
  std::mutex mutex_;
  // the condition variable will be used to wait for events to happen
  std::condition_variable cv_;
  // A queue for newly scheduled operations, written to by remote threads
  // and emptied by the driving thread
  std::vector<operation_base*> submission_queue_;
};
```

Note, how we type-erase two different types of operations in the submission queue.
Since the timer queue is local to the driving thread, we implement cancellation by submitting an operation of type `operation_type::cancel` to the queue.
The driving thread will then remove the target from its local time queue, if possible. 
This leads, again, to an asynchronous cancellation scheme.

As the first step of the run method we install a stop-callback, which sets our stopping flag for the run method once a stop request is issued.

```cpp
template <stdexec::stoppable_token StopToken>
auto timed_run_loop::run(StopToken stop_token) -> void {
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

        void operator()() const noexcept try {
            {
                std::lock_guard lock{self->mutex_};
                stopped = true;
            }
            self->cv_.notify_one();
        } catch (...) {
        }
    };

    using CallbackT = typename StopToken::template callback_type<on_stop>;
    CallbackT callback(stop_token, on_stop{this, stop_requested});
```

After installing the callback we enter a while loop that drives the progress.
Each iteration of the loop processes newly scheduled tasks and checks whether some timers are expired.
If nothing is to be done the method waits for the smallest deadline to expire.
In this first attempt of our implementation we use a `std::priority_queue` to manage our timers but we need to change that to implement efficient cancellation.

The first step in each event loop iteration is to process the submission queue and to fill the timer queue

```cpp
    // we create a local copy of a submission queue that will be processsed
    // and use a priority queue to manage timers sorted by their deadline
    std::vector<operation_base*> submission_queue{};
    std::priority_queue<timer_op_handle> timers{};
    while (true) {
        // in every iteration of the run loop we will empty the local submission
        // queue. We immediately complete ready ops and push all others into the 
        // timer queue
        auto now = std::chrono::system_clock::now();
        for (operation_base* op : submission_queue) {
            if (op->type_ == operation_type::timer) {
                auto timer = static_cast<timer_op_base*>(op);
                if (timer->deadline_ <= now) {
                    timer->set_value_(timer);
                } else {
                    timers.push(timer_op_handle{timer});
                }
            } else if (op->type_ == operation_type::cancel) {
                // 1. TODO: remove target from priority queue and complete that timer
                // 2. complete the cancellation request
                op->set_value_(op);
            }
        }
        submission_queue.clear();
```

We do this on a local copy of the submission queue because I don't want to lock the mutex while the queue is being processed.
It is especially a bad idea to lock the mutex when you complete any operations because those completions might want to submit follow up tasks or issue a stop request, which would easily deadlock.


`std::priority_queue` does not support the removal single elements from the middle of the queue and thus we have to implement our own queue data structure.
We will address that after completing our first implementation.

After processing the submission queue we also have to check in each iteration whether some pending timers are now ready to be completed.

```cpp
        // After we emptied the submission queue we check whether any ready
        // timers are waiting for its completion in the timer queue
        while (!timers.empty()) {
            timer_op_handle next = timers.top();
            if (next.pointer_->deadline_ <= now) {
                timers.pop();
                next.pointer_->set_value_(next.pointer_);
            } else {
                break;
            }
        }
```

This completes the local work that our multiplexer needs to do.
The next stop of the event loop is to enter the critical section and to decide whether to wait for new submissions or quit the algorithm.

```cpp
        // from here on we take a lock. no mutations from remote are possible
        std::unique_lock lock{mutex_};
        if (!submission_queue_.empty()) {
            // new submissions are available lets swap the queues and proceed from
            // the beginning
            std::swap(submission_queue, submission_queue_);
            continue;
        }
        // no new submission... lets see whether we need to wait or quit
        if (stop_requested && timers.empty()) {
            // stop requested and no timers to wait for? Let's quit.
            return;
        } else if (stop_requested && !timers.empty()) {
            // stop requested and we need to cancel all the remaining timers
            while (!timers.empty()) {
                auto next = timers.top();
                timers.pop();
                next.pointer_->run_loop_stopped_ = true;
                next.pointer_->set_value_(next.pointer_);
            }
        } else if (!stop_requested && timers.empty()) {
            // no stop but there are no timers? Let's wait.
            cv_.wait(lock);
        } else if (!stop_requested && !timers.empty()) {
            // timers available? Let's wait for the next deadline.
            auto deadline = timers.top().pointer_->deadline_;
            cv_.wait_until(lock, deadline);
        }
    } // while(true)
}
```

Done! We implemented the core logic for our multiplexer, without proper cancellation of timers, yet. Let's do that now. `std::priority_queue` does not support element-wise removal.
Lets make a very naive implementation that internally uses `std::multiset`.

```cpp
struct timer_op_handle {
    timer_op_base* pointer_;

    friend auto operator<(timer_op_handle lhs, timer_op_handle rhs) noexcept -> bool {
        return lhs.pointer_->deadline_ < rhs.pointer_->deadline_;
    }
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
        auto pos = timers_.find(timer);
        if (pos == timers_.end()) {
            return false;
        }
        timers_.erase(pos);
        return true;
    }

    auto empty() const noexcept -> bool { return timers_.empty(); }

  private:
    std::multiset<timer_op_handle> timers_;
};
```

A "relatively simple" alternative to the naive implementation is to use a `std::vector` as storage and to maintain an additional index as a data member in each timer operation state.
This index can be used to efficiently find an element within the array-based heap and consequently implement an efficient element-wise removal with contiguous storage.
The index must be maintained when a heap algorithm such as `std::push_heap` or `std::pop_heap` is being applied.

Next, we apply this `timer_queue` to our `run()` algorithm

```cpp
    // we create a local copy of a submission queue that will be processsed
    // and use a timer_queue to manage timers sorted by their deadline
    std::vector<operation_base*> submission_queue{};
    timer_queue timers{};
    while (true) {
        // in every iteration of the run loop we will empty the local submission
        // queue. We immediately complete ready ops and push all others into the 
        // timer queue
        auto now = std::chrono::system_clock::now();
        for (operation_base* op : submission_queue) {
            if (op->type_ == operation_type::timer) {
                auto timer = static_cast<timer_op_base*>(op);
                if (timer->deadline_ <= now) {
                    timer->set_value_(timer);
                } else {
                    timers.push(timer_op_handle{timer});
                }
            } else if (op->type_ == operation_type::cancel) {
                auto cancellation = static_cast<cancellation_op_base*>(op);
                // if the target submission has not completed yet we make it complete
                // now. if the target is not in the timer queue then it has already
                // been completed.
                // note, the cancellation submission always completes after the
                // target submission completes.
                if (cancellation->target && timers.remove(cancellation->target)) {
                    cancellation->target->set_value_(cancellation->target);
                }
                cancellation->set_value_(cancellation);
            }
        }
        submission_queue.clear();
```

Next, we need to implement the scheduler API.
We copy the public API from `jthread_scheduler` example before and have

```cpp
class timed_run_loop_scheduler {
    template <class Receiver>
    struct operation;
    
    // This sender schedules a timer that completes on the timed_run_loop
    // after the specified deadline has expired.
    struct sender {
        using sender_concept = stdexec::sender_t;

        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_error_t(std::exception_ptr),
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

    // The only data member of the scheduler
    timed_run_loop* run_loop_;

   public:
    using scheduler_concept = timed_scheduler_t;

    timed_run_loop_scheduler(timed_run_loop& run_loop) noexcept
      : run_loop_{&run_loop} {}

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

    friend bool operator==(
        timed_run_loop_scheduler, timed_run_loop_scheduler) noexcept = default;

   private:
    // [...]
};
```

I think cancellation is the most interesting and most difficult aspect of the implementation.
Upon an incoming stop request a stop-callback will schedule another cancellation operation to the submission queue of the context.
If this happens the whole schedule-operation will wait for both submissions, timer and its cancellation, to complete before it completes itself via the connected receiver.
In fact, our implementation guarantees that the cancellation operation completes last as long as there is only one driving thread.
I want to make this assumption here and leave it as an exercise to generalize cancellation to support multiple driving threads.

We implement this behaviour with a "reference counter", which we call `op_count_`.
This integer is initialized with `1` and will be set to `2` when the stop request happens.
If the stop-callback is invoked, we delegate the responsibility to complete the connected receiver to the cancel operation because it is guaranteed to be completed second.
Subsequent access to the reference count will be synchronized by destroying the stop-callback.

The cancellation and the timer share some common state:

1. the connected receiver
2. the `op_count_`
3. and a reference to the execution context.

We place those shared objects into the operation state of the cancellation and have this as an inner object of the schedule operation state.

```cpp
template <class Receiver>
struct cancel_operation : timed_run_loop::cancellation_op_base {
    Receiver receiver_;
    timed_run_loop& run_loop_;
    // the op count is only increased within the stop-callback
    // and it is decreased when the timer operation completes.
    int op_count_{1};

    static void do_set_value(void* self) noexcept {
        static_cast<cancel_operation*>(self)->complete();
    }

    cancel_operation(Receiver receiver, timed_run_loop& run_loop,
                      timed_run_loop::timer_op_base* target) noexcept
        : cancellation_op_base{{do_set_value,
                                timed_run_loop::operation_type::cancel},
                                target},
          receiver_{std::move(receiver)},
          run_loop_{run_loop} {}

    // this is called from within the stop-callback
    void submit() noexcept try {
        assert(op_count_ == 1);
        op_count_ = 2;
        {
            std::lock_guard lock{run_loop_.mutex_};
            run_loop_.submission_queue_.push_back(this);
        }
        // notify_one is noexcept
        run_loop_.cv_.notify_one();
    } catch (...) {
        op_count_ = 1;
    }

    // this is called from within the timed_run_loop
    void complete() noexcept {
        // here we assume that there is only one driving thread
        // and in that case it is guaranteed that the timer operation
        // completes first
        assert(op_count_ == 1);
        stdexec::set_stopped(std::move(receiver_));
    }
};

template <class Receiver>
struct operation : timed_run_loop::timer_op_base {
    using operation_concept = stdexec::operation_state_t;

    using stop_token_type =
        stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

    struct on_stop {
        operation* self;
        void operator()() const noexcept { self->cancellation_.submit(); }
    };

    using stop_callback =
        typename stop_token_type::template callback_type<on_stop>;

    cancel_operation<Receiver> cancellation_;
    std::optional<stop_callback> stop_callback_;

    static void do_set_value(void* pointer) noexcept {
        auto self = static_cast<operation*>(pointer);
        // here we desroy the stop callback, which synchronizes with a
        // parallel call to it
        self->stop_callback_.reset();
        // if we reach this line then there can be no other threads that
        // change the op_count_. 
        // any potential modification on op_count_ happens-before we
        // destroy the stop_callback
        self->cancellation_.op_count_ -= 1;
        if (self->cancellation_.op_count_ == 0) {
            // op_count_ hits zero, we are responsible for completing the
            // whole operation
            if (self->run_loop_stopped_) {
                stdexec::set_stopped(
                    std::move(self->cancellation_.receiver_));
            } else {
                stdexec::set_value(
                    std::move(self->cancellation_.receiver_));
            }
        }
    }

    operation(Receiver receiver,
              std::chrono::system_clock::time_point deadline,
              timed_run_loop& run_loop)
        : timer_op_base{{do_set_value,
                          timed_run_loop::operation_type::timer},
                        deadline},
          cancellation_(std::move(receiver), run_loop, this)
        {}

    void start() noexcept try {
        {
            std::lock_guard lock{cancellation_.run_loop_.mutex_};
            cancellation_.run_loop_.submission_queue_.push_back(this);
        }
        // everything below is guaranteed to be noexcept
        cancellation_.run_loop_.cv_.notify_one();
        auto stop_token = stdexec::get_stop_token(
              stdexec::get_env(cancellation_.receiver_));
        stop_callback_.emplace(stop_token, on_stop{this});
    } catch (...) {
        stdexec::set_error(std::move(cancellation_.receiver_),
                            std::current_exception());
    }
};
```

This concludes our implementation of cancellable timers for the single-threaded execution context.
Although this is much code to cope with I wanted to share this specific idiom for cancellation because it tremendously helped me out in many occasions once I knew of it.
It also teaches many quirks about the involved types from `std::execution`.

After putting that much effort into this, we also want to put this into action.
Let's start with an example that uses only the `timed_run_loop`.


```cpp
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
    // Possible output:
    // A: This thread id: 125207094740800, duration: 500115696ns
    // C: This thread id: 125207094740800, duration: 500217048ns
}

int main() {
    using namespace std::chrono_literals;
    timed_run_loop run_loop{};
    auto scheduler = run_loop.scheduler();

    // drive the run_loop until we get a stop request
    stdexec::sender auto drive = stdexec::then(
        stdexec::get_stop_token(), [&](auto token) { run_loop.run(token); });

    stdexec::sync_wait(exec::when_any(async_main(scheduler), drive));
}
```

[Link to godbolt](https://godbolt.org/z/5ETKojE1T)

We can extend this example and use two different schedulers to have multiple threads that interact with each other through stop requests

```cpp
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
    jthread_scheduler jt_scheduler{};

    auto print_after_500ms = print_tid("A", scheduler.schedule_after(500ms));
    auto print_after_1s = print_tid("B", scheduler.schedule_after(1s));
    auto jt_scheduler_100ms =
        print_tid("C", jt_scheduler.schedule_after(100ms));
    return print_tid("D", exec::when_any(print_after_500ms, print_after_1s,
                                         jt_scheduler_100ms));
    // Possible output:
    // C: This thread id: 134368215430720, duration: 100084404ns
    // D: This thread id: 134368219748160, duration: 100234216ns
```

[Link to godbolt](https://godbolt.org/z/MEEvvjrv5)

Finally, I want to make some notes.
There are many ways to implement cancellation for an event loop like this.
I also prepared a version, where the cancellation is not an operation that can be completed like presented.
You can make it a simple command for the driving thread instead.
The upside of making cancellation a command versus being a completable operation is that the timer operation doesn't need to wait for the completion of both, timer operation and stop command.
The downside of that strategy is that there is potentially some left-over command in the run loop after the timer has been completed.
[I prepared that solution here](https://godbolt.org/z/jGx8qab5o) but decided to present you with the solution above.

For anyone who wants to go further from here, you could take the next step towards your custom `io_run_loop`. 
To do this, replace the condition variable with a call to `::poll` or something similar and put the appropriate deadline there.
The cancellation logic for operations on file descriptors follows the same patterns.
A good exercise is also to wrap an existing IO multiplexer such as Boost.asio, libevent, Glib, Qt, etc.

The article ends here. The next article will be a walkthrough about creating your own *sender adaptors*.
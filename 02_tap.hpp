#include <print>
#include <stdexec/execution.hpp>
#include <type_traits>

template <class From, class To>
struct copy_const
    : std::conditional<std::is_const_v<From>, std::add_const_t<To>, To> {};

template <class From, class To>
struct copy_volatile
    : std::conditional<std::is_volatile_v<From>, std::add_volatile_t<To>, To> {
};

template <class From, class To>
using copy_volatile_t = typename copy_volatile<From, To>::type;

template <class From, class To>
struct copy_cv : copy_const<From, copy_volatile_t<From, To>> {};

template <class From, class To>
using copy_cv_t = typename copy_cv<From, To>::type;

template <class From, class To>
struct copy_reference
    : std::conditional<
          std::is_rvalue_reference_v<From>, std::add_rvalue_reference_t<To>,
          std::conditional_t<std::is_lvalue_reference_v<From>,
                             std::add_lvalue_reference_t<To>, To>> {};

template <class From, class To>
struct copy_cvref : copy_reference<From, copy_cv_t<From, To>> {};

template <class From, class To>
using copy_cvref_t = typename copy_cvref<From, To>::type;

template <class Fn, class Receiver>
struct tap_operation_base {
    Fn fun_;
    Receiver receiver_;

    template <class... Args>
    void receive(Args&&... args) noexcept try {
        std::invoke(std::move(fun_), std::as_const(args)...);
        stdexec::set_value(std::move(receiver_), std::forward<Args>(args)...);
    } catch (...) {
        stdexec::set_error(std::move(receiver_), std::current_exception());
    }
};

template <class Fn, class Receiver>
struct tap_receiver {
    using receiver_concept = stdexec::receiver_t;

    tap_operation_base<Fn, Receiver>* parent_;

    template <class... Args>
    void set_value(Args&&... args) && noexcept {
        parent_->receive(std::forward<Args>(args)...);
    }

    template <class Error>
    void set_error(Error&& error) && noexcept {
        stdexec::set_error(std::move(parent_->receiver_),
                           std::forward<Error>(error));
    }

    void set_stopped() && noexcept {
        stdexec::set_stopped(std::move(parent_->receiver_));
    }

    auto get_env() const noexcept -> stdexec::env_of_t<Receiver> {
        return stdexec::get_env(parent_->receiver_);
    }
};

template <class Sender, class Fn, class Receiver>
struct tap_operation : tap_operation_base<Fn, Receiver> {
    using operation_concept = stdexec::operation_state_t;

    stdexec::connect_result_t<Sender, tap_receiver<Fn, Receiver>> child_;

    tap_operation(Sender sndr, Fn fun, Receiver rcvr)
        : tap_operation_base<Fn, Receiver>{std::move(fun), std::move(rcvr)},
          child_{stdexec::connect(std::forward<Sender>(sndr),
                                  tap_receiver<Fn, Receiver>{this})} {}

    tap_operation(const tap_operation&) = delete;
    tap_operation& operator=(const tap_operation&) = delete;

    tap_operation(tap_operation&&) = delete;
    tap_operation& operator=(tap_operation&&) = delete;

    void start() noexcept { stdexec::start(child_); }
};

template <class Sender, class Fn>
struct tap_sender {
    using sender_concept = stdexec::sender_t;

    Sender sndr_;
    Fn fun_;

    using with_exception = stdexec::completion_signatures<stdexec::set_error_t(
        std::exception_ptr)>;

    template <class Self, class Env>
    auto get_completion_signatures(this Self&& self, Env&& env) noexcept
        -> stdexec::transform_completion_signatures_of<
            copy_cvref_t<Self, Sender>, Env, with_exception> {
        return {};
    }

    template <class Self, class Rcvr>
    auto connect(this Self&& self, Rcvr receiver)
        -> tap_operation<copy_cvref_t<Self, Sender>, Fn, Rcvr> {
        return {std::forward<Self>(self).sndr_, std::forward<Self>(self).fun_,
                std::move(receiver)};
    }
};

struct tap_t {
    template <class Sender, class Fn>
    constexpr auto operator()(Sender sndr, Fn fun) const
        -> tap_sender<Sender, Fn> {
        return {std::move(sndr), std::move(fun)};
    }
};

inline constexpr tap_t tap{};

int main() {
    auto sndr = tap(stdexec::just(42),
                    [](int value) { std::print("value: {}\n", value); });
    auto [value] = stdexec::sync_wait(sndr).value();
    return value;
}
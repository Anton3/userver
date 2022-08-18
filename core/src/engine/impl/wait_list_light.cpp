#include "wait_list_light.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <fmt/format.h>
#include <boost/atomic/atomic.hpp>

#include <engine/task/sleep_state.hpp>
#include <engine/task/task_context.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/underlying_value.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine::impl {
namespace {

struct alignas(8) Waiter32 final {
  TaskContext* context{nullptr};
  SleepState::Epoch epoch{0};
};

struct alignas(16) Waiter64 final {
  TaskContext* context{nullptr};
  SleepState::Epoch epoch{0};
  [[maybe_unused]] std::uint32_t padding_dont_use{0};
};

using Waiter = std::conditional_t<sizeof(void*) == 8, Waiter64, Waiter32>;

// The type used in boost::atomic must have no padding to perform CAS safely.
static_assert(std::has_unique_object_representations_v<Waiter>);

}  // namespace
}  // namespace engine::impl

USERVER_NAMESPACE_END

template <>
struct fmt::formatter<USERVER_NAMESPACE::engine::impl::Waiter> {
  static constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(USERVER_NAMESPACE::engine::impl::Waiter waiter,
              FormatContext& ctx) const {
    return fmt::format_to(
        ctx.out(), "({}, {})", fmt::ptr(waiter.context),
        USERVER_NAMESPACE::utils::UnderlyingValue(waiter.epoch));
  }
};

USERVER_NAMESPACE_BEGIN

namespace engine::impl {

struct WaitListLight::Impl final {
  // We use boost::atomic, because std::atomic refuses to produce double-width
  // compare-and-swap instruction (DWCAS) under x86_64 on some compilers.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80878
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=84522
  boost::atomic<Waiter> waiter{Waiter{}};
};

WaitListLight::WaitListLight() noexcept = default;

WaitListLight::~WaitListLight() {
  UASSERT_MSG(IsEmptyRelaxed(),
              "Someone is waiting on WaitListLight while it's being destroyed");
}

void WaitListLight::Append(boost::intrusive_ptr<TaskContext> context) noexcept {
  UASSERT(context);
  UASSERT(context->IsCurrent());

  const Waiter new_waiter{context.get(), context->GetEpoch()};
  LOG_TRACE() << "Append waiter=" << fmt::to_string(new_waiter)
              << " use_count=" << context->use_count();

  // Keep a reference logically stored in the WaitListLight to ensure that
  // WakeupOne can complete safely in parallel with the waiting task being
  // cancelled, Remove-d and stopped.
  context.detach();

  Waiter expected{};
  // seq_cst is important for the "Append-Check-Wakeup" sequence.
  const bool success = impl_->waiter.compare_exchange_strong(
      expected, new_waiter, boost::memory_order_seq_cst,
      boost::memory_order_relaxed);
  UASSERT_MSG(success,
              fmt::format("Attempting to wait in a single AtomicWaiter "
                          "from multiple coroutines: new={} existing={}",
                          new_waiter, expected));
}

void WaitListLight::WakeupOne() {
  // Of all 2^N possible values of Waiter, 'empty' is the most frequent one.
  // There is a good chance the first CAS will succeed.
  Waiter old_waiter{};
  while (true) {
    const bool success = impl_->waiter.compare_exchange_weak(
        old_waiter, Waiter{}, boost::memory_order_acq_rel,
        boost::memory_order_acquire);
    if (success || !old_waiter.context) break;
  }

  if (!old_waiter.context) return;
  const boost::intrusive_ptr<TaskContext> context{old_waiter.context,
                                                  /*add_ref=*/false};

  LOG_TRACE() << "WakeupOne waiter=" << fmt::to_string(old_waiter)
              << " use_count=" << context->use_count();
  context->Wakeup(TaskContext::WakeupSource::kWaitList, old_waiter.epoch);
}

void WaitListLight::Remove(TaskContext& context) noexcept {
  UASSERT(context.IsCurrent());
  const Waiter expected{&context, context.GetEpoch()};

  auto old_waiter = expected;
  const bool success = impl_->waiter.compare_exchange_strong(
      old_waiter, Waiter{}, boost::memory_order_release,
      boost::memory_order_relaxed);

  if (!success) {
    UASSERT_MSG(!old_waiter.context,
                fmt::format("An unexpected context is occupying the "
                            "AtomicWaiter: expected={} actual={}",
                            expected, old_waiter));
    return;
  }

  LOG_TRACE() << "Remove waiter=" << fmt::to_string(expected)
              << " use_count=" << context.use_count();
  intrusive_ptr_release(&context);
}

bool WaitListLight::IsEmptyRelaxed() noexcept {
  // We have to use 'compare_exchange_strong' instead of 'load', because old
  // Boost.Atomic only provides 'compare_exchange_*' for x86_64.
  Waiter expected{};
  impl_->waiter.compare_exchange_strong(expected, expected,
                                        boost::memory_order_relaxed,
                                        boost::memory_order_relaxed);
  return expected.context == nullptr;
}

}  // namespace engine::impl

USERVER_NAMESPACE_END

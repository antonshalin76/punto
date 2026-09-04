#include "punto/runtime_health.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace std::chrono_literals;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void expect(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void test_inactivity_never_degrades() {
  punto::StallHealthPolicy::Clock::time_point now{};
  punto::StallHealthPolicy policy{[&] { return now; }};
  now += 24h;
  const auto snapshot = policy.snapshot();
  expect(snapshot.health == punto::ComponentHealth::Ready,
         "ordinary inactivity stays ready");
  expect(!snapshot.in_flight, "inactivity has no in-flight work");
  expect(snapshot.last_progress_ms == 0, "initial progress starts at zero");
}

void test_exact_stall_boundary_and_recovery() {
  punto::StallHealthPolicy::Clock::time_point now{};
  punto::StallHealthPolicy policy{[&] { return now; }};
  policy.begin(now);
  now += 1999ms;
  expect(policy.snapshot().health == punto::ComponentHealth::Ready,
         "in-flight work is ready before two seconds");
  now += 1ms;
  auto snapshot = policy.snapshot();
  expect(snapshot.health == punto::ComponentHealth::Degraded,
         "in-flight work degrades at two seconds");
  expect(snapshot.in_flight, "degraded snapshot identifies in-flight work");

  policy.clear_in_flight();
  policy.mark_progress();
  snapshot = policy.snapshot();
  expect(snapshot.health == punto::ComponentHealth::Ready,
         "completion recovers degraded health");
  expect(!snapshot.in_flight, "completion clears in-flight work");
  expect(snapshot.last_progress_ms == 2000,
         "completion advances monotonic progress timestamp");
}

void test_old_next_head_can_degrade_immediately() {
  punto::StallHealthPolicy::Clock::time_point now{};
  punto::StallHealthPolicy policy{[&] { return now; }};
  const auto second_head_accepted = now;
  now += 2500ms;
  policy.mark_progress();
  policy.begin(second_head_accepted);
  const auto snapshot = policy.snapshot();
  expect(snapshot.health == punto::ComponentHealth::Degraded,
         "an old newly-exposed sequence head is already degraded");
  expect(snapshot.last_progress_ms == 2500,
         "head advance remains observable progress");
}

void test_failure_is_absorbing() {
  punto::StallHealthPolicy::Clock::time_point now{};
  punto::StallHealthPolicy policy{[&] { return now; }};
  policy.fail();
  policy.begin(now);
  now += 5s;
  policy.clear_in_flight();
  policy.mark_progress();
  expect(policy.snapshot().health == punto::ComponentHealth::Failed,
         "failed health cannot recover through normal progress");
}

void test_health_names_are_exact() {
  expect(punto::component_health_name(punto::ComponentHealth::Ready) == "ready",
         "ready grammar");
  expect(punto::component_health_name(punto::ComponentHealth::Degraded) ==
             "degraded",
         "degraded grammar");
  expect(punto::component_health_name(punto::ComponentHealth::Failed) ==
             "failed",
         "failed grammar");
}

void test_explicit_health_transitions_and_progress() {
  punto::ExplicitHealthPolicy::Clock::time_point now{};
  punto::ExplicitHealthPolicy policy{punto::ComponentHealth::Degraded,
                                     [&] { return now; }};
  expect(policy.snapshot().health == punto::ComponentHealth::Degraded,
         "explicit policy preserves initial degraded state");
  now += 250ms;
  policy.ready();
  policy.mark_progress();
  auto snapshot = policy.snapshot();
  expect(snapshot.health == punto::ComponentHealth::Ready,
         "healthy commit recovers explicit component");
  expect(snapshot.last_progress_ms == 250,
         "healthy commit advances explicit progress");
  policy.fail();
  policy.ready();
  policy.degrade();
  now += 1s;
  policy.mark_progress();
  snapshot = policy.snapshot();
  expect(snapshot.health == punto::ComponentHealth::Failed,
         "explicit failure is absorbing");
  expect(snapshot.last_progress_ms == 250,
         "failed component cannot claim later progress");
}

} // namespace

int main() {
  test_inactivity_never_degrades();
  test_exact_stall_boundary_and_recovery();
  test_old_next_head_can_degrade_immediately();
  test_failure_is_absorbing();
  test_health_names_are_exact();
  test_explicit_health_transitions_and_progress();
  std::cout << "punto-runtime-health-contract: OK\n";
}

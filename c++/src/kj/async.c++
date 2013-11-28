// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "async.h"
#include "debug.h"
#include "vector.h"
#include <exception>
#include <map>
#include <typeinfo>

#if KJ_USE_FUTEX
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#endif

#if __GNUC__
#include <cxxabi.h>
#endif

namespace kj {

namespace {

static __thread EventLoop* threadLocalEventLoop = nullptr;

#define _kJ_ALREADY_READY reinterpret_cast< ::kj::EventLoop::Event*>(1)

class BoolEvent: public EventLoop::Event {
public:
  bool fired = false;

  Maybe<Own<Event>> fire() override {
    fired = true;
    return nullptr;
  }
};

class YieldPromiseNode final: public _::PromiseNode {
public:
  bool onReady(EventLoop::Event& event) noexcept override {
    event.armBreadthFirst();
    return false;
  }
  void get(_::ExceptionOrValue& output) noexcept override {
    output.as<_::Void>().value = _::Void();
  }
};

}  // namespace

namespace _ {  // private

class TaskSetImpl {
public:
  inline TaskSetImpl(TaskSet::ErrorHandler& errorHandler)
    : errorHandler(errorHandler) {}

  ~TaskSetImpl() noexcept(false) {
    // std::map doesn't like it when elements' destructors throw, so carefully disassemble it.
    if (!tasks.empty()) {
      Vector<Own<Task>> deleteMe(tasks.size());
      for (auto& entry: tasks) {
        deleteMe.add(kj::mv(entry.second));
      }
    }
  }

  class Task final: public EventLoop::Event {
  public:
    Task(TaskSetImpl& taskSet, Own<_::PromiseNode>&& nodeParam)
        : taskSet(taskSet), node(kj::mv(nodeParam)) {
      if (node->onReady(*this)) {
        armDepthFirst();
      }
    }

  protected:
    Maybe<Own<Event>> fire() override {
      // Get the result.
      _::ExceptionOr<_::Void> result;
      node->get(result);

      // Delete the node, catching any exceptions.
      KJ_IF_MAYBE(exception, kj::runCatchingExceptions([this]() {
        node = nullptr;
      })) {
        result.addException(kj::mv(*exception));
      }

      // Call the error handler if there was an exception.
      KJ_IF_MAYBE(e, result.exception) {
        taskSet.errorHandler.taskFailed(kj::mv(*e));
      }

      // Remove from the task map.
      auto iter = taskSet.tasks.find(this);
      KJ_ASSERT(iter != taskSet.tasks.end());
      Own<Event> self = kj::mv(iter->second);
      taskSet.tasks.erase(iter);
      return mv(self);
    }

    _::PromiseNode* getInnerForTrace() override {
      return node;
    }

  private:
    TaskSetImpl& taskSet;
    kj::Own<_::PromiseNode> node;
  };

  void add(Promise<void>&& promise) {
    auto task = heap<Task>(*this, kj::mv(promise.node));
    Task* ptr = task;
    tasks.insert(std::make_pair(ptr, kj::mv(task)));
  }

  kj::String trace() {
    kj::Vector<kj::String> traces;
    for (auto& entry: tasks) {
      traces.add(entry.second->trace());
    }
    return kj::strArray(traces, "\n============================================\n");
  }

private:
  TaskSet::ErrorHandler& errorHandler;

  // TODO(soon):  Use a linked list instead.
  std::map<Task*, Own<Task>> tasks;
};

class LoggingErrorHandler: public TaskSet::ErrorHandler {
public:
  static LoggingErrorHandler instance;

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, "Uncaught exception in daemonized task.", exception);
  }
};

LoggingErrorHandler LoggingErrorHandler::instance = LoggingErrorHandler();

}  // namespace _ (private)

// =======================================================================================

EventLoop& EventLoop::current() {
  EventLoop* result = threadLocalEventLoop;
  KJ_REQUIRE(result != nullptr, "No event loop is running on this thread.");
  return *result;
}

bool EventLoop::isCurrent() const {
  return threadLocalEventLoop == this;
}

EventLoop::EventLoop()
    : daemons(kj::heap<_::TaskSetImpl>(_::LoggingErrorHandler::instance)) {
  KJ_REQUIRE(threadLocalEventLoop == nullptr, "This thread already has an EventLoop.");
  threadLocalEventLoop = this;
}

EventLoop::~EventLoop() noexcept(false) {
  KJ_REQUIRE(threadLocalEventLoop == this,
             "EventLoop being destroyed in a different thread than it was created.") {
    break;
  }

  KJ_DEFER(threadLocalEventLoop = nullptr);

  // Destroy all "daemon" tasks, noting that their destructors might try to access the EventLoop
  // some more.
  daemons = nullptr;

  // The application _should_ destroy everything using the EventLoop before destroying the
  // EventLoop itself, so if there are events on the loop, this indicates a memory leak.
  KJ_REQUIRE(head == nullptr, "EventLoop destroyed with events still in the queue.  Memory leak?",
             head->trace()) {
    // Unlink all the events and hope that no one ever fires them...
    Event* event = head;
    while (event != nullptr) {
      Event* next = event->next;
      event->next = nullptr;
      event->prev = nullptr;
      event = next;
    }
    break;
  }
}

void EventLoop::waitImpl(Own<_::PromiseNode>&& node, _::ExceptionOrValue& result) {
  KJ_REQUIRE(threadLocalEventLoop == this,
             "Can only call wait() in the thread that created this EventLoop.");
  KJ_REQUIRE(!running, "wait() is not allowed from within event callbacks.");

  BoolEvent doneEvent;
  doneEvent.fired = node->onReady(doneEvent);

  running = true;
  KJ_DEFER(running = false);

  while (!doneEvent.fired) {
    if (head == nullptr) {
      // No events in the queue.  Wait for callback.
      prepareToSleep();
      if (head != nullptr) {
        // Whoa, new job was just added.
        // TODO(now):  Can't happen anymore?
        wake();
      }
      sleep();
    } else {
      Event* event = head;

      head = event->next;
      depthFirstInsertPoint = &head;
      if (tail == &event->next) {
        tail = &head;
      }

      event->next = nullptr;
      event->prev = nullptr;

      Maybe<Own<Event>> eventToDestroy;
      {
        event->firing = true;
        KJ_DEFER(event->firing = false);
        eventToDestroy = event->fire();
      }
    }

    depthFirstInsertPoint = &head;
  }

  node->get(result);
  KJ_IF_MAYBE(exception, runCatchingExceptions([&]() {
    node = nullptr;
  })) {
    result.addException(kj::mv(*exception));
  }
}

Promise<void> EventLoop::yield() {
  return Promise<void>(false, kj::heap<YieldPromiseNode>());
}

void EventLoop::daemonize(kj::Promise<void>&& promise) {
  KJ_REQUIRE(daemons.get() != nullptr, "EventLoop is shutting down.") { return; }
  daemons->add(kj::mv(promise));
}

EventLoop::Event::Event()
    : loop(EventLoop::current()), next(nullptr), prev(nullptr) {}

EventLoop::Event::~Event() noexcept(false) {
  if (prev != nullptr) {
    if (loop.head == this) {
      loop.head = next;
    }
    if (loop.tail == &next) {
      loop.tail = prev;
    }
    if (loop.depthFirstInsertPoint == &next) {
      loop.depthFirstInsertPoint = prev;
    }

    *prev = next;
    if (next != nullptr) {
      next->prev = prev;
    }
  }

  KJ_REQUIRE(!firing, "Promise callback destroyed itself.");
  KJ_REQUIRE(threadLocalEventLoop == &loop || threadLocalEventLoop == nullptr,
             "Promise destroyed from a different thread than it was created in.");
}

void EventLoop::Event::armDepthFirst() {
  KJ_REQUIRE(threadLocalEventLoop == &loop || threadLocalEventLoop == nullptr,
             "Event armed from different thread than it was created in.  You must use "
             "the thread-safe work queue to queue events cross-thread.");

  if (prev == nullptr) {
    next = *loop.depthFirstInsertPoint;
    prev = loop.depthFirstInsertPoint;
    *prev = this;
    if (next != nullptr) {
      next->prev = &next;
    }

    loop.depthFirstInsertPoint = &next;

    if (loop.tail == prev) {
      loop.tail = &next;
    }
  }
}

void EventLoop::Event::armBreadthFirst() {
  KJ_REQUIRE(threadLocalEventLoop == &loop || threadLocalEventLoop == nullptr,
             "Event armed from different thread than it was created in.  You must use "
             "the thread-safe work queue to queue events cross-thread.");

  if (prev == nullptr) {
    next = *loop.tail;
    prev = loop.tail;
    *prev = this;
    if (next != nullptr) {
      next->prev = &next;
    }

    loop.tail = &next;
  }
}

_::PromiseNode* EventLoop::Event::getInnerForTrace() {
  return nullptr;
}

#if __GNUC__
static kj::String demangleTypeName(const char* name) {
  int status;
  char* buf = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  kj::String result = kj::heapString(buf);
  free(buf);
  return kj::mv(result);
}
#else
static kj::String demangleTypeName(const char* name) {
  return kj::heapString(name);
}
#endif

static kj::String traceImpl(EventLoop::Event* event, _::PromiseNode* node) {
  kj::Vector<kj::String> trace;

  if (event != nullptr) {
    trace.add(demangleTypeName(typeid(*event).name()));
  }

  while (node != nullptr) {
    trace.add(demangleTypeName(typeid(*node).name()));
    node = node->getInnerForTrace();
  }

  return strArray(trace, "\n");
}

kj::String EventLoop::Event::trace() {
  return traceImpl(this, getInnerForTrace());
}

// =======================================================================================

#if KJ_USE_FUTEX

SimpleEventLoop::SimpleEventLoop() {}
SimpleEventLoop::~SimpleEventLoop() noexcept(false) {}

void SimpleEventLoop::prepareToSleep() noexcept {
  __atomic_store_n(&preparedToSleep, 1, __ATOMIC_RELAXED);
}

void SimpleEventLoop::sleep() {
  while (__atomic_load_n(&preparedToSleep, __ATOMIC_RELAXED) == 1) {
    syscall(SYS_futex, &preparedToSleep, FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0);
  }
}

void SimpleEventLoop::wake() const {
  if (__atomic_exchange_n(&preparedToSleep, 0, __ATOMIC_RELAXED) != 0) {
    // preparedToSleep was 1 before the exchange, so a sleep must be in progress in another thread.
    syscall(SYS_futex, &preparedToSleep, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  }
}

#else

#define KJ_PTHREAD_CALL(code) \
  { \
    int pthreadError = code; \
    if (pthreadError != 0) { \
      KJ_FAIL_SYSCALL(#code, pthreadError); \
    } \
  }

#define KJ_PTHREAD_CLEANUP(code) \
  { \
    int pthreadError = code; \
    if (pthreadError != 0) { \
      KJ_LOG(ERROR, #code, strerror(pthreadError)); \
    } \
  }

SimpleEventLoop::SimpleEventLoop() {
  KJ_PTHREAD_CALL(pthread_mutex_init(&mutex, nullptr));
  KJ_PTHREAD_CALL(pthread_cond_init(&condvar, nullptr));
}
SimpleEventLoop::~SimpleEventLoop() noexcept(false) {
  KJ_PTHREAD_CLEANUP(pthread_cond_destroy(&condvar));
  KJ_PTHREAD_CLEANUP(pthread_mutex_destroy(&mutex));
}

void SimpleEventLoop::prepareToSleep() noexcept {
  pthread_mutex_lock(&mutex);
  preparedToSleep = 1;
}

void SimpleEventLoop::sleep() {
  while (preparedToSleep == 1) {
    pthread_cond_wait(&condvar, &mutex);
  }
  pthread_mutex_unlock(&mutex);
}

void SimpleEventLoop::wake() const {
  pthread_mutex_lock(&mutex);
  if (preparedToSleep != 0) {
    // preparedToSleep was 1 before the exchange, so a sleep must be in progress in another thread.
    preparedToSleep = 0;
    pthread_cond_signal(&condvar);
  }
  pthread_mutex_unlock(&mutex);
}

#endif

// =======================================================================================

void PromiseBase::absolve() {
  runCatchingExceptions([this]() { node = nullptr; });
}

kj::String PromiseBase::trace() {
  return traceImpl(nullptr, node);
}

TaskSet::TaskSet(ErrorHandler& errorHandler)
    : impl(heap<_::TaskSetImpl>(errorHandler)) {}

TaskSet::~TaskSet() noexcept(false) {}

void TaskSet::add(Promise<void>&& promise) {
  impl->add(kj::mv(promise));
}

kj::String TaskSet::trace() {
  return impl->trace();
}

namespace _ {  // private

PromiseNode* PromiseNode::getInnerForTrace() { return nullptr; }

bool PromiseNode::OnReadyEvent::init(EventLoop::Event& newEvent) {
  if (event == _kJ_ALREADY_READY) {
    return true;
  } else {
    event = &newEvent;
    return false;
  }
}

void PromiseNode::OnReadyEvent::arm() {
  if (event == nullptr) {
    event = _kJ_ALREADY_READY;
  } else {
    event->armDepthFirst();
  }
}

// -------------------------------------------------------------------

bool ImmediatePromiseNodeBase::onReady(EventLoop::Event& event) noexcept { return true; }

ImmediateBrokenPromiseNode::ImmediateBrokenPromiseNode(Exception&& exception)
    : exception(kj::mv(exception)) {}

void ImmediateBrokenPromiseNode::get(ExceptionOrValue& output) noexcept {
  output.exception = kj::mv(exception);
}

// -------------------------------------------------------------------

AttachmentPromiseNodeBase::AttachmentPromiseNodeBase(Own<PromiseNode>&& dependency)
    : dependency(kj::mv(dependency)) {}

bool AttachmentPromiseNodeBase::onReady(EventLoop::Event& event) noexcept {
  return dependency->onReady(event);
}

void AttachmentPromiseNodeBase::get(ExceptionOrValue& output) noexcept {
  dependency->get(output);
}

PromiseNode* AttachmentPromiseNodeBase::getInnerForTrace() {
  return dependency;
}

void AttachmentPromiseNodeBase::dropDependency() {
  dependency = nullptr;
}

// -------------------------------------------------------------------

TransformPromiseNodeBase::TransformPromiseNodeBase(Own<PromiseNode>&& dependency)
    : dependency(kj::mv(dependency)) {}

bool TransformPromiseNodeBase::onReady(EventLoop::Event& event) noexcept {
  return dependency->onReady(event);
}

void TransformPromiseNodeBase::get(ExceptionOrValue& output) noexcept {
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    getImpl(output);
    dropDependency();
  })) {
    output.addException(kj::mv(*exception));
  }
}

PromiseNode* TransformPromiseNodeBase::getInnerForTrace() {
  return dependency;
}

void TransformPromiseNodeBase::dropDependency() {
  dependency = nullptr;
}

void TransformPromiseNodeBase::getDepResult(ExceptionOrValue& output) {
  dependency->get(output);
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    dependency = nullptr;
  })) {
    output.addException(kj::mv(*exception));
  }
}

// -------------------------------------------------------------------

ForkBranchBase::ForkBranchBase(Own<ForkHubBase>&& hubParam): hub(kj::mv(hubParam)) {
  auto lock = hub->branchList.lockExclusive();

  if (lock->lastPtr == nullptr) {
    onReadyEvent.arm();
  } else {
    // Insert into hub's linked list of branches.
    prevPtr = lock->lastPtr;
    *prevPtr = this;
    next = nullptr;
    lock->lastPtr = &next;
  }
}

ForkBranchBase::~ForkBranchBase() noexcept(false) {
  if (prevPtr != nullptr) {
    // Remove from hub's linked list of branches.
    auto lock = hub->branchList.lockExclusive();
    *prevPtr = next;
    (next == nullptr ? lock->lastPtr : next->prevPtr) = prevPtr;
  }
}

void ForkBranchBase::hubReady() noexcept {
  onReadyEvent.arm();
}

void ForkBranchBase::releaseHub(ExceptionOrValue& output) {
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([this]() {
    hub = nullptr;
  })) {
    output.addException(kj::mv(*exception));
  }
}

bool ForkBranchBase::onReady(EventLoop::Event& event) noexcept {
  return onReadyEvent.init(event);
}

PromiseNode* ForkBranchBase::getInnerForTrace() {
  return hub->getInnerForTrace();
}

// -------------------------------------------------------------------

ForkHubBase::ForkHubBase(Own<PromiseNode>&& innerParam, ExceptionOrValue& resultRef)
    : inner(kj::mv(innerParam)), resultRef(resultRef) {
  if (inner->onReady(*this)) armDepthFirst();
}

Maybe<Own<EventLoop::Event>> ForkHubBase::fire() {
  // Dependency is ready.  Fetch its result and then delete the node.
  inner->get(resultRef);
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([this]() {
    inner = nullptr;
  })) {
    resultRef.addException(kj::mv(*exception));
  }

  auto lock = branchList.lockExclusive();
  for (auto branch = lock->first; branch != nullptr; branch = branch->next) {
    branch->hubReady();
    *branch->prevPtr = nullptr;
    branch->prevPtr = nullptr;
  }
  *lock->lastPtr = nullptr;

  // Indicate that the list is no longer active.
  lock->lastPtr = nullptr;

  return nullptr;
}

_::PromiseNode* ForkHubBase::getInnerForTrace() {
  return inner;
}

// -------------------------------------------------------------------

ChainPromiseNode::ChainPromiseNode(Own<PromiseNode> innerParam)
    : state(STEP1), inner(kj::mv(innerParam)) {
  if (inner->onReady(*this)) armDepthFirst();
}

ChainPromiseNode::~ChainPromiseNode() noexcept(false) {}

bool ChainPromiseNode::onReady(EventLoop::Event& event) noexcept {
  switch (state) {
    case STEP1:
      KJ_REQUIRE(onReadyEvent == nullptr, "onReady() can only be called once.");
      onReadyEvent = &event;
      return false;
    case STEP2:
      return inner->onReady(event);
  }
  KJ_UNREACHABLE;
}

void ChainPromiseNode::get(ExceptionOrValue& output) noexcept {
  KJ_REQUIRE(state == STEP2);
  return inner->get(output);
}

PromiseNode* ChainPromiseNode::getInnerForTrace() {
  return inner;
}

Maybe<Own<EventLoop::Event>> ChainPromiseNode::fire() {
  KJ_REQUIRE(state != STEP2);

  static_assert(sizeof(Promise<int>) == sizeof(PromiseBase),
      "This code assumes Promise<T> does not add any new members to PromiseBase.");

  ExceptionOr<PromiseBase> intermediate;
  inner->get(intermediate);

  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([this]() {
    inner = nullptr;
  })) {
    intermediate.addException(kj::mv(*exception));
  }

  KJ_IF_MAYBE(exception, intermediate.exception) {
    // There is an exception.  If there is also a value, delete it.
    kj::runCatchingExceptions([&,this]() { intermediate.value = nullptr; });
    // Now set step2 to a rejected promise.
    inner = heap<ImmediateBrokenPromiseNode>(kj::mv(*exception));
  } else KJ_IF_MAYBE(value, intermediate.value) {
    // There is a value and no exception.  The value is itself a promise.  Adopt it as our
    // step2.
    inner = kj::mv(value->node);
  } else {
    // We can only get here if inner->get() returned neither an exception nor a
    // value, which never actually happens.
    KJ_FAIL_ASSERT("Inner node returned empty value.");
  }
  state = STEP2;

  if (onReadyEvent != nullptr) {
    if (inner->onReady(*onReadyEvent)) {
      onReadyEvent->armDepthFirst();
    }
  }

  return nullptr;
}

// -------------------------------------------------------------------

ExclusiveJoinPromiseNode::ExclusiveJoinPromiseNode(Own<PromiseNode> left, Own<PromiseNode> right)
    : left(*this, kj::mv(left)), right(*this, kj::mv(right)) {}

ExclusiveJoinPromiseNode::~ExclusiveJoinPromiseNode() noexcept(false) {}

bool ExclusiveJoinPromiseNode::onReady(EventLoop::Event& event) noexcept {
  return onReadyEvent.init(event);
}

void ExclusiveJoinPromiseNode::get(ExceptionOrValue& output) noexcept {
  KJ_REQUIRE(left.get(output) || right.get(output), "get() called before ready.");
}

PromiseNode* ExclusiveJoinPromiseNode::getInnerForTrace() {
  auto result = left.getInnerForTrace();
  if (result == nullptr) {
    result = right.getInnerForTrace();
  }
  return result;
}

ExclusiveJoinPromiseNode::Branch::Branch(
    ExclusiveJoinPromiseNode& joinNode, Own<PromiseNode> dependencyParam)
    : joinNode(joinNode), dependency(kj::mv(dependencyParam)) {
  if (dependency->onReady(*this)) armDepthFirst();
}

ExclusiveJoinPromiseNode::Branch::~Branch() noexcept(false) {}

bool ExclusiveJoinPromiseNode::Branch::get(ExceptionOrValue& output) {
  if (dependency) {
    dependency->get(output);
    return true;
  } else {
    return false;
  }
}

Maybe<Own<EventLoop::Event>> ExclusiveJoinPromiseNode::Branch::fire() {
  // Cancel the branch that didn't return first.  Ignore exceptions caused by cancellation.
  if (this == &joinNode.left) {
    kj::runCatchingExceptions([&]() { joinNode.right.dependency = nullptr; });
  } else {
    kj::runCatchingExceptions([&]() { joinNode.left.dependency = nullptr; });
  }

  joinNode.onReadyEvent.arm();
  return nullptr;
}

PromiseNode* ExclusiveJoinPromiseNode::Branch::getInnerForTrace() {
  return dependency;
}

// -------------------------------------------------------------------

EagerPromiseNodeBase::EagerPromiseNodeBase(
    Own<PromiseNode>&& dependencyParam, ExceptionOrValue& resultRef)
    : dependency(kj::mv(dependencyParam)), resultRef(resultRef) {
  if (dependency->onReady(*this)) armDepthFirst();
}

bool EagerPromiseNodeBase::onReady(EventLoop::Event& event) noexcept {
  return onReadyEvent.init(event);
}

PromiseNode* EagerPromiseNodeBase::getInnerForTrace() {
  return dependency;
}

Maybe<Own<EventLoop::Event>> EagerPromiseNodeBase::fire() {
  dependency->get(resultRef);
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([this]() {
    dependency = nullptr;
  })) {
    resultRef.addException(kj::mv(*exception));
  }

  onReadyEvent.arm();
  return nullptr;
}

// -------------------------------------------------------------------

bool AdapterPromiseNodeBase::onReady(EventLoop::Event& event) noexcept {
  return onReadyEvent.init(event);
}

}  // namespace _ (private)
}  // namespace kj

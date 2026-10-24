// refs: ADR-3.D4, F-SRC-coderoast-ipc:consumer/CMakeLists.txt
// invariant: the package is one module with nothing sealed, so this aggregate re-exports the whole
// facade and no partition is declared.
export module coderoast.ipc.consumer.test;
export import std;
export import coderoast.ipc.consumer;
export import coderoast.ipc.core;

export namespace coderoast::ipc::test
{
// note: clang-21.1.8 drops libc++'s jthread lock lambda below gtest (llvm-project#172241).
// post: one jthread per task, each running it; destruction requests stop on and joins every one.
class ThreadGroup
{
  public:
    explicit ThreadGroup(std::vector<std::function<void()>> tasks);
    ThreadGroup(const ThreadGroup&) = delete;
    ThreadGroup& operator=(const ThreadGroup&) = delete;
    ThreadGroup(ThreadGroup&&) = delete;
    ThreadGroup& operator=(ThreadGroup&&) = delete;
    ~ThreadGroup();

  private:
    std::vector<std::jthread> threads_;
};

ThreadGroup::ThreadGroup(std::vector<std::function<void()>> tasks)
{
    threads_.reserve(tasks.size());
    for (auto& task : tasks)
        threads_.emplace_back(std::move(task));
}

ThreadGroup::~ThreadGroup() = default;
} // namespace coderoast::ipc::test

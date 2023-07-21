18#! https://zhuanlan.zhihu.com/p/462702955
# workflow 源码解析 : SubTask

更加详细的源码注释可看 : https://github.com/chanchann/workflow_annotation

## WFCounterTask
在并行编程中，计数任务（Counting Task）是一种常见的同步工具，主要用于处理多个并行任务之间的依赖关系。具体来说，它可以用于等待多个子任务完成后再进行下一步操作。

在这个头文件中的 `WFCounterTask` 类的实现中，每个 `WFCounterTask` 对象都有一个原子计数器 `value`。这个计数器初始化时被设置为需要等待完成的子任务的数量（加一是因为 `WFCounterTask` 自身也是一个任务）。每当有一个子任务完成时，都会调用 `count()` 方法使计数器减一。当计数器的值变为零时，表示所有的子任务都已完成，此时 `WFCounterTask` 任务自身也被标记为完成，然后调用 `subtask_done()` 方法通知父任务。

此外，`WFCounterTask` 也可以设置一个回调函数 `callback`，这个回调函数会在 `WFCounterTask` 任务完成（即所有子任务都完成）时被调用。这样，开发者可以在所有子任务完成后执行一些额外的操作，例如清理资源、更新状态等。

总的来说，计数任务是一种等待一组并行任务全部完成的同步工具，提供了一种简洁而有效的方式来处理并行任务之间的依赖关系。

假设我们有一个大型计算任务，这个任务可以被拆分为三个独立的子任务 A、B 和 C，这三个子任务可以并行执行以提高效率。同时，我们还有一个任务 D，它需要在 A、B 和 C 都完成后才能开始执行，因为它需要使用 A、B 和 C 的结果。

在这种情况下，我们可以创建一个计数任务 `WFCounterTask`，并将计数器 `value` 设置为 4（子任务 A、B、C 和计数任务本身）。然后，我们启动子任务 A、B 和 C。每当一个子任务完成时，我们都调用 `count()` 方法，使计数器 `value` 减一。

当所有的子任务都完成时，计数器 `value` 的值将变为零，此时 `WFCounterTask` 任务自身也被标记为完成，然后调用 `subtask_done()` 方法通知父任务。接下来，我们就可以开始执行任务 D 了。

在这个过程中，计数任务 `WFCounterTask` 起到了同步的作用：它保证了任务 D 只有在所有子任务都完成后才会开始执行，即使子任务是并行执行的。

这就是计数任务的基本工作方式。通过使用计数任务，我们可以方便地管理和同步一组并行任务，使我们的代码更加简洁和易于理解。
当然，以下是一个使用`WFCounterTask`的示例。这个例子使用了`WFCounterTask`来同步三个并行任务（A、B 和 C），在这三个任务都完成之后，才执行任务 D。

假设我们有以下函数，代表我们的任务：

```cpp
void taskA() { /* do something */ }
void taskB() { /* do something */ }
void taskC() { /* do something */ }
void taskD() { /* do something */ }
```

我们将它们封装在`std::thread`中，以便并行执行，如下所示：

```cpp
std::thread threadA(taskA);
std::thread threadB(taskB);
std::thread threadC(taskC);
```

现在，我们创建一个`WFCounterTask`，并将其计数器设置为 4（3个子任务 + 计数任务自身）：

```cpp
WFCounterTask counterTask(3, [] (WFCounterTask *task) {
    // 当所有的任务都完成，执行任务 D。
    taskD();
});
```

在每个任务完成时，我们都会调用 `count()` 方法：

```cpp
threadA.join();
counterTask.count();

threadB.join();
counterTask.count();

threadC.join();
counterTask.count();
```

在所有的子任务都完成之后，`WFCounterTask` 的 `count()` 方法将标记任务为完成，然后自动调用我们定义的回调函数，即执行任务 D。

这是一个基本的例子，实际使用中可能需要根据具体的并行库（比如C++11的`std::thread`，或者其他并行/异步编程库，比如Boost.Asio或Intel TBB等）和编程模型进行修改。

请注意，由于我这里使用了`std::thread`，因此实际情况中你需要处理可能的线程同步问题，比如通过使用`std::mutex`和`std::lock_guard`等来确保线程安全。而且，上述示例是简化版的，实际在使用过程中，你可能需要处理线程的启动、连接、异常处理等更多复杂的情况。
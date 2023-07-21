# workflow 源码解析: ThreadPool

项目源码 : https://github.com/sogou/workflow

更加详细的源码注释可看 : https://github.com/chanchann/workflow_annotation

## 线程任务销毁其线程所在的线程池
线程任务销毁其线程所在的线程池的情况是非常罕见的，通常不建议在线程任务中销毁它所在的线程池，因为这可能导致并发问题或其他线程意外地被终止。不过，仍然有一些极端情况可能需要线程任务销毁其线程所在的线程池：

1. 遇到无法恢复的错误：如果线程任务遇到了无法恢复的错误，并且这个错误会导致线程池中的其他线程无法继续执行，这时可能需要该线程任务销毁线程池。

2. 动态资源管理：在某些复杂的场景中，如果线程任务能够感知到系统的资源压力（例如，内存不足或CPU过载），且确定销毁线程池是解决问题的最佳方案，可能会需要线程任务销毁线程池。

3. 线程池的使用者与管理者：在某些特定设计中，线程任务可能同时扮演线程池的使用者与管理者的角色。在这种情况下，线程任务可能需要在满足某些特定条件（如：任务已全部完成，线程池不再需要等）时销毁线程池。

请注意，尽管有这些可能的情况，但在实际操作中，我们通常会避免在线程任务中销毁线程池，因为这可能引发并发问题或导致其他线程的意外终止。我们更倾向于在主线程或其他控制线程中进行线程池的销毁。

## linux内核
在 Linux 内核中，通过 `struct list_head` 和 `container_of` 宏实现的解耦主要体现了设计原则中的“单一职责原则”和“开放封闭原则”。

1. 单一职责原则：该原则指出每个类或者模块应该有一个单一的功能。在这个例子中，`struct list_head` 仅负责链表的链接功能，而 `struct thrdpool_task` 负责存储任务数据。这两个结构体的功能互不干扰，各自完成各自的职责，符合了单一职责原则。

2. 开放封闭原则：该原则指出软件实体应当对扩展开放，对修改封闭。`struct list_head` 的设计使其成为了一种通用的双向链表节点，它可以用于任何需要链表结构的场景。这种设计使得链表结构的使用非常灵活，可以轻易地扩展到新的场景中去。而当我们需要添加新的数据类型时，只需要定义一个新的结构体，将 `struct list_head` 作为其成员即可，无需修改 `struct list_head` 或者链表操作函数的代码。这种设计很好地遵守了开放封闭原则。

`container_of` 宏的使用则体现了“抽象数据类型”的设计思想。通过 `container_of`，我们可以从链表节点得到包含该节点的结构体实例，从而隐藏了数据存储和链表链接的具体实现细节，使得代码更为抽象和通用。这种设计有助于提高代码的复用性和维护性。

这种设计并没有直接对应到一个特定的设计模式。但如果非要把它归类到设计模式的话，它最接近的可能是"适配器模式"。

在这里，`struct list_head` 提供了一个通用的、适用于所有需要链表的场景的接口，而 `container_of` 宏则充当一个“适配器”，使得任何包含 `struct list_head` 的结构体都可以适应这个通用的链表接口。

适配器模式用于将一个类的接口转换成客户期望的另一个接口。适配器模式使得原本由于接口不兼容不能在一起工作的那些类可以在一起工作。

在这个例子中，不同类型的数据（通过各种不同的结构体表示）可以被看作是“客户期望的接口”，而链表结构（由 `struct list_head` 提供）则可以被看作是“一个类的接口”。`container_of` 宏则充当了一个“适配器”，使得这些不同类型的数据可以使用通用的链表接口，从而可以被组织成一个链表。

然而，这个例子并没有严格地遵循适配器模式。因为在适配器模式中，通常是通过创建一个新的适配器类来完成接口的转换，而在这个例子中，并没有创建新的类或者结构体，而是通过 `container_of` 宏直接操作指针来实现的。所以，虽然这个例子在一定程度上体现了适配器模式的思想，但并不能算是一个标准的适配器模式。


在 C++ 中，我们可以使用模板和类继承来实现类似于 `struct list_head` 和 `container_of` 的设计。以下是一个可能的实现：

```cpp
#include <iostream>

// 模拟Linux内核中的list_head结构体
template <typename T>
class ListHead {
public:
    ListHead() : prev(this), next(this) {}

    void insert(T* t) {
        t->next = this;
        t->prev = prev;
        prev->next = t;
        prev = t;
    }

    T* remove(T* t) {
        if(t->next) t->next->prev = t->prev;
        if(t->prev) t->prev->next = t->next;
        t->next = t->prev = nullptr;
        return t;
    }

    T *prev, *next;
};

// 任务类
class Task {
public:
    explicit Task(int id) : id(id) {}

    int getID() const {
        return id;
    }
private:
    int id;
};

// __thrdpool_task_entry 类，继承了ListHead，并包含了一个Task对象
class ThrdpoolTaskEntry : public ListHead<ThrdpoolTaskEntry> {
public:
    ThrdpoolTaskEntry(Task* task) : task(task) {}

    Task* getTask() const {
        return task;
    }
private:
    Task* task;
};

int main() {
    ThrdpoolTaskEntry head;
    ThrdpoolTaskEntry t1(new Task(1));
    ThrdpoolTaskEntry t2(new Task(2));
    ThrdpoolTaskEntry t3(new Task(3));

    head.insert(&t1);
    head.insert(&t2);
    head.insert(&t3);

    std::cout << t1.getTask()->getID() << std::endl;  // 输出：1
    std::cout << t2.getTask()->getID() << std::endl;  // 输出：2
    std::cout << t3.getTask()->getID() << std::endl;  // 输出：3

    return 0;
}
```

在这个例子中，`ListHead` 是一个模板类，它代表了双向链表中的一个节点，可以插入和移除节点。`ThrdpoolTaskEntry` 类继承了 `ListHead`，并且包含了一个 `Task` 对象，代表了一个任务。

注意这里我们并没有直接实现 `container_of` 宏的功能，因为在 C++ 中，我们可以通过类的继承和向下转型来获取到包含 `ListHead` 的那个对象。这是由于 C++ 的面向对象特性使得我们可以用更加自然的方式来实现同样的功能。

首先，关于 `ThrdpoolTaskEntry t1(new Task(1));` 的问题。这行代码实际上是创建了一个 `Task` 对象和一个 `ThrdpoolTaskEntry` 对象。

`new Task(1)` 创建了一个新的 `Task` 对象，这个对象的 `id` 成员被初始化为 1。然后，这个 `Task` 对象的指针被传递给 `ThrdpoolTaskEntry` 的构造函数，这个构造函数将这个指针保存在 `ThrdpoolTaskEntry` 对象的 `task` 成员中。因此，`ThrdpoolTaskEntry t1(new Task(1));` 就是创建了一个 `ThrdpoolTaskEntry` 对象，这个对象包含了一个指向 `id` 为 1 的 `Task` 对象的指针。

然后，关于在 C++ 中，我们可以通过类的继承和向下转型来获取到包含 `ListHead` 的那个对象的问题。

在这个例子中，`ThrdpoolTaskEntry` 类是从 `ListHead` 类派生出来的，因此，每个 `ThrdpoolTaskEntry` 对象都是一个 `ListHead` 对象。当我们有一个指向 `ListHead` 的指针时，我们可以将这个指针向下转型（downcast），将它转换为一个指向 `ThrdpoolTaskEntry` 的指针。这就是所谓的获取到包含 `ListHead` 的那个对象。

在 C++ 中，向下转型可以通过动态转型运算符 `dynamic_cast` 来进行。例如：

```cpp
ListHead<ThrdpoolTaskEntry>* list_head_ptr = &t1;  // t1 是一个 ThrdpoolTaskEntry 对象
ThrdpoolTaskEntry* entry_ptr = dynamic_cast<ThrdpoolTaskEntry*>(list_head_ptr);
```

这段代码将 `list_head_ptr`（它是一个指向 `ListHead` 的指针）向下转型为一个指向 `ThrdpoolTaskEntry` 的指针。如果转型成功，那么 `entry_ptr` 就会指向 `t1`。这就相当于我们从一个 `ListHead` 对象获取到了包含它的 `ThrdpoolTaskEntry` 对象。

请注意，在实际编程中，我们应该尽量避免使用向下转型，因为它会破坏类型系统的安全性。在这个例子中，如果我们可以确定每个 `ListHead` 对象都是由一个 `ThrdpoolTaskEntry` 对象包含的，那么使用向下转型应该是安全的。但在更一般的情况下，我们应该避免使用向下转型，而应该尽量使用虚函数来实现多态行为。



`memcmp` 是一个用于比较内存块的函数，用于比较两个内存区域的内容是否相等。它接受三个参数：

- 第一个参数 `&tid`：表示要比较的第一个内存块的起始地址，即指向 `tid` 变量的指针。
- 第二个参数 `&__zero_tid`：表示要比较的第二个内存块的起始地址，即指向 `__zero_tid` 变量的指针。
- 第三个参数 `sizeof(pthread_t)`：表示要比较的内存块的大小，即 `pthread_t` 类型的大小。

`memcmp` 函数会逐字节地比较两个内存块的内容。如果两个内存块的内容完全相同，则返回值为 0。如果有差异，则返回值不为 0。


## 命名风格
函数 `static int __thrdpool_create_threads(size_t nthreads, thrdpool_t *pool)` 的命名风格可以被描述为以下几个特点：

1. **前缀双下划线**：函数名以两个下划线开头 `__`，这是一个命名约定，用于指示该函数是一个内部函数或者是由特定库或框架提供的函数。前缀双下划线通常用于避免与用户定义的函数名冲突。

2. **小写字母**：函数名使用小写字母，这是一种常见的命名风格，遵循了 C 语言的命名约定。

3. **下划线分隔**：函数名中使用下划线 `_` 来分隔单词，这是一种常见的命名风格，称为下划线命名法（underscore_case）。使用下划线可以提高函数名的可读性和可理解性。

4. **函数作用描述**：函数名 `__thrdpool_create_threads` 反映了函数的主要目的或作用。它表明该函数用于在线程池中创建线程。

5. **静态函数**：函数使用 `static` 修饰符进行了静态声明，这表示该函数仅在当前源文件中可见，不能被其他源文件调用。这种静态声明的目的通常是将函数限制在特定的编译单元中使用，以避免与其他源文件中的同名函数冲突。

综上所述，`static int __thrdpool_create_threads(size_t nthreads, thrdpool_t *pool)` 的命名风格是使用小写字母、下划线分隔单词，并以双下划线作为前缀来表示一个具有特定作用且被限制在当前编译单元中使用的函数。这种命名风格有助于提高代码的可读性和可维护性，并与其他命名约定保持一致。

##
```cpp
struct __thrdpool_task_entry
{
	struct list_head list;
	struct thrdpool_task task;
};
```

`struct list_head` 是 Linux 内核中常用的数据结构，它用于实现双向链表。在许多内核数据结构中，我们经常会看到 `struct list_head` 成员，这是因为双向链表提供了一种灵活而高效的方式来组织和管理数据。

在你给出的例子中，`__thrdpool_task_entry` 结构体中的 `struct list_head list;` 成员就是用来将 `__thrdpool_task_entry` 实例链接到一个双向链表中的。具体来说，这个 `list` 成员包含了两个指向其他 `list_head` 的指针，一个是指向链表中前一个元素的 `prev` 指针，另一个是指向链表中后一个元素的 `next` 指针。通过这两个指针，我们可以在链表中向前或向后移动，从而遍历整个链表。

这种做法的主要优点是可以将数据的存储（在你的例子中，由 `struct thrdpool_task task;` 实现）与数据的组织方式（由 `struct list_head list;` 实现）解耦。这意味着我们可以在不改变数据存储方式的情况下，灵活地改变数据的组织方式。此外，通过使用双向链表，我们可以高效地进行数据的插入和删除操作。

需要注意的是，这种做法的一个前提是我们需要能够通过 `struct list_head` 成员找到包含它的 `__thrdpool_task_entry` 实例。在 Linux 内核中，这通常是通过 `container_of` 宏来实现的，这个宏可以通过一个结构体中的成员的地址计算出该结构体的地址。

## POSIX 线程库
POSIX 线程库中的线程属性（pthread attribute）用于设置线程的各种属性和行为。这些属性可以通过 `pthread_attr_t` 类型的线程属性对象来指定。

以下是一些常用的线程属性：

1. **线程栈大小（Stack Size）**：可以使用 `pthread_attr_setstacksize` 函数设置线程栈的大小，或使用 `pthread_attr_getstacksize` 函数获取当前线程栈的大小。

2. **线程栈地址（Stack Address）**：可以使用 `pthread_attr_setstack` 函数设置线程栈的起始地址和大小，或使用 `pthread_attr_getstack` 函数获取当前线程栈的地址和大小。

3. **线程调度策略（Scheduling Policy）**：可以使用 `pthread_attr_setschedpolicy` 函数设置线程的调度策略，如先进先出（FIFO）、循环轮转（Round-Robin）等。

4. **线程调度参数（Scheduling Parameters）**：可以使用 `pthread_attr_setschedparam` 函数设置线程的调度参数，如优先级等。

5. **线程分离状态（Detached State）**：可以使用 `pthread_attr_setdetachstate` 函数设置线程的分离状态，将线程设置为可分离状态，使其在结束时自动释放资源。

6. **线程继承属性（Inheritance Attribute）**：可以使用 `pthread_attr_setinheritsched` 函数设置线程的继承属性，指定线程是否继承创建它的线程的调度属性。

这只是线程属性的一小部分，POSIX 线程库还提供其他更详细的线程属性和函数来进行线程的配置和管理。要使用这些属性，通常需要先创建一个 `pthread_attr_t` 类型的线程属性对象，然后使用相应的属性设置函数进行配置。

需要注意的是，线程属性的具体支持和行为可能因操作系统和编译器而异。在使用线程属性时，建议参考相关的操作系统和编译器文档以了解更多详细信息。

pthread_key_t是POSIX线程库中的一种数据类型，用于表示线程特定数据（Thread Specific Data, TSD）的键。线程特定数据提供了一种将数据与特定线程关联的机制。每个线程都有一份这样的数据的副本。

这种机制非常有用，例如，当你使用一个全局变量，但每个线程都需要有自己的副本，或者当你使用一个库函数，这个函数需要存储一些状态信息，但这些信息对于不同的线程可能会有所不同。

下面是 `pthread_key_t` 的一般用法：

1. 使用 `pthread_key_create` 函数创建一个线程特定数据的键。该函数接受一个 `pthread_key_t` 类型的指针作为参数，用于存储创建的键。

2. 使用 `pthread_setspecific` 函数将特定于线程的数据与键关联。该函数接受一个 `pthread_key_t` 类型的键和一个指针作为参数，将指针的值与该线程的键关联起来。

3. 使用 `pthread_getspecific` 函数获取与键关联的线程特定数据。该函数接受一个 `pthread_key_t` 类型的键作为参数，并返回与该键关联的线程特定数据的指针。

4. 在需要销毁线程特定数据的时候，使用 `pthread_key_delete` 函数销毁键。该函数接受一个 `pthread_key_t` 类型的键作为参数，将该键及其关联的线程特定数据一同销毁。

使用 `pthread_key_t` 和相关的函数，可以在多线程程序中为每个线程创建和管理独立的线程特定数据，每个线程可以在其线程特定数据上存储和访问自己的数据，而不会干扰其他线程的数据。

需要注意的是，`pthread_key_t` 是一个不透明的类型，它实际上是一个整数或指针，用于在内部标识线程特定数据的键。直接操作 `pthread_key_t` 可能会导致未定义的行为，应该使用提供的函数来创建、设置、获取和删除线程特定数据。

`pthread_key_create` 函数允许提供一个析构函数，该函数将在线程结束时自动被调用，用来清理线程特定数据。下面是一个使用析构函数的示例：

```cpp
#include <iostream>
#include <pthread.h>

pthread_key_t key;

// 析构函数，用于清理线程特定数据
void destructor(void* value) {
    std::cout << "Destructor is called for value " << *(int*)value << std::endl;
    delete (int*)value;
}

void* threadFunc(void* arg) {
    // 创建一个新的整数对象，并设置线程特定数据
    int* value = new int(*(int*)arg);
    pthread_setspecific(key, value);

    int* p = (int*)pthread_getspecific(key);
    std::cout << "Thread " << *p << " has the key value " << *p << std::endl;

    return nullptr;
}

int main() {
    pthread_t thread1, thread2;

    // 创建线程特定数据的键，并提供一个析构函数
    pthread_key_create(&key, destructor);

    int value1 = 1;
    int value2 = 2;

    pthread_create(&thread1, nullptr, threadFunc, &value1);
    pthread_create(&thread2, nullptr, threadFunc, &value2);

    pthread_join(thread1, nullptr);
    pthread_join(thread2, nullptr);

    pthread_key_delete(key);

    return 0;
}
```

在这个例子中，我们首先创建了一个线程特定数据的键，并且提供了一个析构函数。这个析构函数会在线程结束时自动被调用，它删除了线程特定数据对应的整数对象，从而防止了内存泄漏。
然后我们创建了两个线程，每个线程都创建了一个新的整数对象，并设置了线程特定数据。注意这里我们使用了动态分配的内存，因为我们要在析构函数中删除这个对象。
你会发现，当线程结束时，析构函数自动被调用，清理了线程特定数据，防止了内存泄漏。


`pthread_setspecific` 是 POSIX 线程库中的一个函数，它被用于设置线程特定数据。线程特定数据（Thread-Specific Data，TSD）是一种允许每个线程都有其自己的独立数据副本的机制。

在这种情况下，即使多个线程可能访问和操作同一份代码，但是每个线程对其线程特定数据的读写都是隔离的。因此，不同的线程之间不会共享通过 `pthread_setspecific` 设置的数据。

例如，如果你在一个线程中使用 `pthread_setspecific(key, value)` 设置了一个特定的值，那么这个值只能在这个线程中通过 `pthread_getspecific(key)` 获取。在其他线程中使用相同的键调用 `pthread_getspecific(key)` 将会返回那个线程为这个键设置的值，如果没有设置过，将会返回 `NULL`。

因此，`pthread_setspecific` 可以用于在多线程环境中存储和操作线程局部（thread-local）数据，而不需要使用互斥锁等同步机制来防止数据竞争。但是要注意的是，线程特定数据在使用完毕后需要通过相应的机制进行清理，以避免内存泄漏。


`pthread_cond_signal` 和 `pthread_cond_broadcast` 是 POSIX 线程库中的两个函数，它们都用于发出条件变量的信号。条件变量用于同步线程，特别是当某些条件发生变化时，通知正在等待这些条件的线程。以下是这两个函数的基本差别：

1. **pthread_cond_signal**：这个函数唤醒正在等待指定条件变量的一个线程。如果有多个线程在等待，那么选择哪个线程被唤醒是不确定的。如果没有线程在等待，那么这个函数什么也不做。

2. **pthread_cond_broadcast**：这个函数唤醒所有正在等待指定条件变量的线程。所有正在等待的线程都会被唤醒并开始执行。如果没有线程在等待，那么这个函数什么也不做。

简单地说，`pthread_cond_signal` 选择一个线程来唤醒，而 `pthread_cond_broadcast` 唤醒所有等待的线程。使用哪个函数取决于你的需求。如果你想让所有等待的线程都响应某个条件的变化，那么可以使用 `pthread_cond_broadcast`。如果你只需要一个线程响应，那么可以使用 `pthread_cond_signal`。但是需要注意的是，哪个线程被 `pthread_cond_signal` 唤醒是不确定的，如果多个线程在等待，系统会选择其中一个唤醒。

这两个函数通常和 `pthread_cond_wait` 或 `pthread_cond_timedwait` 函数一起使用，这两个函数用于让线程进入等待状态，直到接收到对应的 `pthread_cond_signal` 或 `pthread_cond_broadcast` 信号。

`pthread_cond_signal`函数在POSIX线程（Pthread）库中用于唤醒一个等待在特定条件变量上的线程。具体哪个线程被唤醒是由线程调度器决定的。这种函数对线程的唤醒是单播的，即只唤醒一个线程，因此通常不会导致“惊群效应”。

“惊群效应”（Thundering Herd Problem）是指在并发环境中，当一群线程或进程都在等待同一事件的时候，这个事件发生后，这群线程或进程同时被唤醒并开始抢占CPU资源，导致系统性能下降的问题。在POSIX线程库中，如果使用`pthread_cond_broadcast`函数唤醒所有等待在特定条件变量上的线程，可能会引发这个问题。

但是，即使在使用`pthread_cond_signal`函数的情况下，如果系统的线程调度策略不合理或者并发控制策略不合理，也可能导致类似“惊群效应”的问题。因此，在设计多线程程序时，应该细心考虑线程的同步和调度策略，避免可能的性能问题。

## 先看接口

接口非常简洁

```cpp
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize);
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool);
int thrdpool_increase(thrdpool_t *pool);
int thrdpool_in_pool(thrdpool_t *pool);
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
					  thrdpool_t *pool);
```

就是线程池创建，销毁，扩容，判断是否在本线程的pool，还有调度

## 线程池的创建(创建消费者)

首先我们创建得知道，线程池的结构

而我们后面就会知道，我们这个结构pthread_setspecific设置成一个thread_local，每个线程都会有一个，所以有tid这些成员

```cpp
typedef struct __thrdpool thrdpool_t;

struct __thrdpool
{
	struct list_head task_queue;
	size_t nthreads;
	size_t stacksize;
	pthread_t tid;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_key_t key;
	pthread_cond_t *terminate;
};
```

我们看如何创建

```cpp
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize)
	// 1. 分配空间
    pool = (thrdpool_t *)malloc(sizeof (thrdpool_t));

    // 2. 初始化
	__thrdpool_init_locks(pool);
	pthread_key_create(&pool->key, NULL)
    INIT_LIST_HEAD(&pool->task_queue);
    pool->stacksize = stacksize;
    pool->nthreads = 0;
    memset(&pool->tid, 0, sizeof (pthread_t));
    pool->terminate = NULL;

    // 3. 创建线程
	__thrdpool_create_threads(nthreads, pool);

	return NULL;
}
```

```cpp
static int __thrdpool_create_threads(size_t nthreads, thrdpool_t *pool)
{
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, pool->stacksize);

		while (pool->nthreads < nthreads)
		{
			pthread_create(&tid, &attr, __thrdpool_routine, pool);
            ...
		}
}

```

然后可见我们线程跑的是__thrdpool_routine

```cpp
static void *__thrdpool_routine(void *arg)
{
	thrdpool_t *pool = (thrdpool_t *)arg;
	struct list_head **pos = &pool->task_queue.next;  
	pthread_setspecific(pool->key, pool);

	while (1)
	{
		pthread_mutex_lock(&pool->mutex);

		while (!pool->terminate && list_empty(&pool->task_queue))
			pthread_cond_wait(&pool->cond, &pool->mutex);

		entry = list_entry(*pos, struct __thrdpool_task_entry, list);
		list_del(*pos);

		pthread_mutex_unlock(&pool->mutex);

		task_routine = entry->task.routine;
		task_context = entry->task.context;
		free(entry);
		task_routine(task_context);
        
        ... 
	}   
    ...
}
```

线程池就是开启了许多消费者线程，如果有任务，就拿出来运行，否则就wait

## thrdpool_schedule，生产者PUT task

我们上次有个地方说到，我们这里就是把task封装好，交给线程池处理。

```cpp
/src/kernel/Executor.cc

int Executor::request(ExecSession *session, ExecQueue *queue)
{
    ... 
	session->queue = queue;
	entry = (struct ExecTaskEntry *)malloc(sizeof (struct ExecTaskEntry));

    entry->session = session;
    entry->thrdpool = this->thrdpool;

    pthread_mutex_lock(&queue->mutex);
    list_add_tail(&entry->list, &queue->task_list); 

    if (queue->task_list.next == &entry->list)
    {
        struct thrdpool_task task = {
            .routine	=	Executor::executor_thread_routine,
            .context	=	queue
        };
        thrdpool_schedule(&task, this->thrdpool);
    }

    pthread_mutex_unlock(&queue->mutex);
    ...
}

```

这里把task(executor_thread_routine)交给线程池处理

```cpp
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool)
{
	void *buf = malloc(sizeof (struct __thrdpool_task_entry));
    __thrdpool_schedule(task, buf, pool);

}
```

```cpp
inline void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
								thrdpool_t *pool)
{
	struct __thrdpool_task_entry *entry = (struct __thrdpool_task_entry *)buf;

	entry->task = *task;
	pthread_mutex_lock(&pool->mutex);
	
    list_add_tail(&entry->list, &pool->task_queue);
	pthread_cond_signal(&pool->cond);

	pthread_mutex_unlock(&pool->mutex);
}
```

这里就是添加至队列就完了，生产者消费者模型，这里就是生产者put，通知消费者来消费

## 未完待续

以上是线程池最核心的两个接口，因为代表着线程池生产者-消费者这个本质的模型。

我在创建的时候，就产生一堆消费者在这wait等待任务来，而我们的thrdpool_schedule就是put任务进来，是个生产者

线程池最为复杂的是各个情况的优雅退出，需要再多多仔细分析，写写demo。

还有就是扩容线程池。

还有仔细分析这里的pthread_key_t 的作用。
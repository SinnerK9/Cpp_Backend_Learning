由于本人此前只有使用cpp写算法题，而从未涉及网络编程与实际项目，了解这个项目需要从漫长的基础概念补全开始。

## 2026.3.23：网络编程初探，Socket与文件传输

首先，socket是怎么回事？
要从零构造一个webserver，其本质是考虑主机之间的通信，因此先要对IP地址，port以及TCP/IP都是些什么东西有一个基本的认识。
查询资料知，socket的构造基于TCP/IP协议栈，这些协议规定了数据传输的流程与规则。我们可以用IP地址唯一地指定一台网络设备，由于一个网络设备上有大量不同应用，端口用于确定通信传输的数据要传输到哪一个应用上。
在此基础上，socket(套接字)是一条不同主机和不同应用之间的虚拟数据通道，主要有两种：TCP/UDP，TCP连接最重要的性质是可靠性，通过超时重传保证另一方一定能够按顺序收到我方发出的数据包。特征：发起连接，不重复不丢失，按顺序传输。
UDP(用户报文协议)以报文为单位传输数据，由于不存在重传机制，这样的传输方式更快，延迟更低，占用资源更少，相应地，连接不安全。

我们默认通信的两个设备一个是客户端，一个是服务端，两边可以通过socket互相传输信息。对服务端而言，socket由bind()方法绑定到某个特定的IP与端口，然后调用listen()开始被动监听，等待客户端socket发来连接请求(三次握手)后accept()，要断开时，客户端发送close()，两者断开连接(四次挥手)。

## 2026.3.24: TCP_Server的基础构建

在此基础上，我们可以开始写一个基础TCP Server。首先需要注意到主机端和网络端的存储字节序问题，主机端往往使用小端序，反之网络端被指定为大端序，为了解决两者混用可能导致的数据误读问题，在网络通信过程中必须予以转换：调用转换函数htons(),htonl()来分别完成短整型和长整型从主机端到网络端的端序转换，反之则是ntohs(),ntohl()。

其次，IP地址底层是32位数字，但实际往往用字符串进行描述，因此用inet_pton(int af, const char *src, void *dst)来将主机字节序的字符串转化为网络字节序的整形，const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)反之。发现网络到主机多了一个size参数，用于标记dst指向内存的存储内容大小。而且其返回值是传出参数对应的字符串内存地址。

关键函数：socket()创建一个套接字，有三个传入参数，分别用于指定协议族协议，数据传输协议(STREAM/DGRAM)，协议。成功会返回一个正的文件描述符，凭借这个文件描述符可以操作某一块指定内存。

建立sockaddr_in类结构体，将指向的内存进行拆分，允许指定协议族，端口号，IP地址（后两者必须转化为计算机需要的形式）。
运用bind时，必须将更智能的地址表单sockaddr_in转换为其能认识的sockaddr！
调用listen方法，将socket转为被动监听状态，准备接收来自客户端的请求，注意其参数backlog，当接收到的请求超出这个参数时，请求不会纳入排队！
accept函数抓取一个请求，返回一个新的文件描述符，并且后面的一切读写都对新的文件描述符进行操作。注意：第三个传入参数必须时socklen_t*类型，用int会导致报错！
当通信通道建立后，调用 recv() 读取数据，调用 send() 发送数据。
用浏览器作为客户端去访问我们的服务器时，recv收到的是一大段按特定格式排列的纯文本字符串，即HTTP 请求报文。

## 2026.3.26:基础单线程TCP_Server的完善

经过了解，我发现address already in use问题本质上是TCP规定关闭连接的一方进入一段time_wait时期，既能够确保不会让旧报文影响新的连接，也保证对方收到报文。Server代码规定服务器主动关闭连接，因此8080端口进入TIME_WAIT，使用netstat命令排查可以看见8080端口的TIME_WAIT状态。为了使得端口可以复用，需要在bind前面加入setsockopt()方法，设置REUSEADDR，使其允许绑定time_wait中的端口。这能够大大提升效率。

到此，server已经能够完成一次性的监听，接收，关闭了，但是我们想要的不是一个只能使用一次的服务器，而应该不断监听来自客户端的需求。因此我们考虑在accept前加一个死循环，保证服务器一直在监听，确保能在任何时候接收到请求并进行处理。相应地，当socket接收失败时不应该弹出错误并return，而应该跳过接收下一个请求。到此为止，一个基本的TCP_Server就完成了。

## 2026.3.27:多线程的基础知识学习
先前我所实现的，是一个最为基础的单线程TCP服务器，它的关键特征是“每一次只能处理一个请求，在这个请求的处理过程中，其他请求都被阻塞，这严重影响了这个服务器的可用性，自然而然地，我想到用多线程来解决这个问题，在多线程并发过程中，每个线程可以独立运行不同的任务，但它们的资源并不独立拥有，而是依赖创建它们的进程而存在。这意味着同一进程内的不同线程可以很方便地进行数据沟通和通信，使得其相对于进程更适用于并发操作。然而其更高的灵活性也意味着程序员需要做更多的工作来确保资源的正确分配，它们需要以正确的操作顺序被利用，同时必须避免死锁产生。

### Thread对象的构造方式
为了进行多线程编程，c++用thread库来进行线程对象生命周期和资源分配的管理。
thread对象主要有三种构造方式：其一是先构造一个无参thread对象，再进行移动赋值，表现为将一个临时线程的所有权转换给当前对象。其次是带可变参数的构造方式，总共支持传入三类可调用对象：函数指针，仿函数和lambda表达式。最后是移动构造，也即将一个已经存在的线程的所有权转换到当前对象。
thread对象构造的一个重要特点是**禁止拷贝，支持移动**，线程资源无法拷贝，但是线程所有权可以进行转让。


### Thread库核心成员函数
最核心的成员函数是join()，它的作用是让当前线程等待一个线程完成，若该线程未执行完毕，那么当前线程也没有办法继续进行，必须阻塞等待该线程执行完毕。与之对应的是detach()，也就是将当前线程和创建的线程分离，两者分别执行。在分离出来的线程执行完毕后将其资源回收。用来描述一个线程能否执行join()方法的函数是joinable()，**join()和detach()都会将线程变为非joinable状态**。非joinable的线程还有以下两类:**无参构造的线程对象**和**状态已经被转移给其他线程的对象**，这总共三类线程都是非joinable的无效线程，**被销毁的线程必须是无效线程**！

### 当前线程操作函数
std::this_thread中提供操作当前线程的工具函数。
get_id用于获取当前线程的id；
sleep_for和sleep_until用于线程休眠，两者的区别是for用于休眠一段时间，until则是休眠到一个固定时间点，两者的差别从名字就可以看出来。
yield表示该线程主动让出当前时间片把资源交给其他线程

**值和引用问题**
线程函数的参数默认按值拷贝到线程内部，即使线程函数的参数是引用类型，在线程函数中修改后也影响不到实参本身。如果要传入引用使得内部操作影响实参，必须在构造thread对象过程中借助std::ref函数


## 2026.3.30 mutex的基本认识

大凡多线程管理，总要涉及到资源共享和线程安全问题，当多个线程同时读写共享资源时，可能会产生数据不一致或者冲突。锁的意义就是确保同一时刻只有一个线程可以访问共享资源，本质是保持数据一致性和准确性。互斥量mutex就是用来保证每个线程的访问过程不被其他线程打断的机制。

### 为什么需要Mutex
每个线程拥有自己独立的栈结构，但是全局变量等临界资源是直接被多个线程共享的。当不同的线程同时操作全局变量时，可能导致冲突，次数越多表现越明显，所以在多线程编程中，需要对共享资源进行适当的同步控制，加锁保护是实现这一点的关键手段。

### 四类互斥锁
std::mutex是最基本的互斥量，对象之间**既不能拷贝也不能移动。**
基本mutex主要有三类方法：lock,try_lock,unlock，作用十分显然。比如说两个线程同时对某个全局变量进行修改，加锁之后，任意时刻只有一个线程能修改，这就避免了竞争。
在加锁的时候，要注意加锁的位置。

std::recursive_mutex，意为递归互斥锁，主要用在递归加锁的场景中。
普通锁的特点是重复加锁导致阻塞，假如有这么一个函数：
void func(int n){
	if(n== 10){
	return;
	}
	mtx.lock();
	n++;
	func(n);
	mtx.unlock()
}
第一次上锁后，还没有解锁就递归调用函数再上一次锁，使用普通mutex会导致此处产生阻塞，这种抢到了锁但是无法申请再次上锁的情况就是死锁。
recursive_mutex的解决方法时，它使得自己持有锁资源的时候无需再做申请。

std::timed_mutex为时间互斥锁，具备定时解锁的功能。若到时间未解锁就自动解锁。其中有两种方法：try_lock_for和try_lock_until，一个表示相对时间一个表示绝对时间点。

最后一种锁是std::recursive_timed_mutex，对时间互斥锁有递归方面升级。

### RAII风格锁
RAII是现代C++编程的重要特点之一，翻译为资源获取即初始化，将资源的生命周期与对象的生命周期绑在一起，利用自动析构保证资源的释放。对于mutex来说RAII的思想尤为重要，因为手动加锁解锁会面临巨大的死锁风险，是对程序员的严峻考验。
void dangerousFunction(int id) {
    try {
        // 使用 RAII 风格的锁管理
        std::lock_guard<std::mutex> lock(mtx);

        std::cout << "Thread " << id << " is running." << std::endl;

        // 模拟异常情况，抛出异常
        if (id == 1) {
            throw std::runtime_error("Thread 1 encountered an error!");
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception caught in thread " << id << ": " << e.what() << std::endl;
    }

}

int main() {
    try {
        std::thread t1(dangerousFunction, 1);
        std::thread t2(dangerousFunction, 2);

        t1.join();
        t2.join();
    } catch (const std::exception& e) {
        std::cerr << "Exception caught: " << e.what() << std::endl;
    }

    return 0;
}
这样的一段代码是对RAII思想的体现，对象lock在创建时调用mtx.lock()，每次离开try作用域时都会调用析构函数进行unlock，保证锁被成功释放。

容易发现，此处并没有直接的unlock方法，而是采用了lock_guard类，用于实现资源的自动加锁和解锁，是RAII思想的体现，确保在作用域结束时自动释放锁资源。

lock_guard有以下特点：
**自动加锁**：创建对象时立即对互斥量加锁，确保进入临界区前已获得锁。
**自动解锁**：作用域结束时自动释放互斥量，避免资源泄漏和死锁。
**适用局部锁定**：通过栈上对象实现，只适用于局部范围的互斥量锁定。

另一个重要模板类是unique_lock，std::unique_lock 是 std::lock_guard 的增强版，提供了更灵活的加锁 / 解锁控制，支持延迟加锁、手动解锁、与条件变量配合使用等场景。
具备以下核心特点：
**灵活的加解锁时机**：可以手动调用 lock()/unlock()，不需要在创建时立即加锁。
**支持延迟加锁**：创建对象时可以选择不立即加锁，后续再手动获取锁。
**可与条件变量（std::condition_variable）配合**：实现复杂的线程同步与等待机制。
**RAII 保障**：生命周期结束时仍会自动解锁，避免资源泄漏。

在此基础上，unique_lock由于其灵活性，可以脱离lock_guard局部加锁的拘束，甚至可用get_lock方法把锁作为返回值传出去。


### 条件变量()
std::condition_variable 是 C++ 标准库中用于线程间条件同步的类，核心是等待-通知机制：
线程可以调用 wait 系列函数，主动进入等待状态，直到某个条件满足。
另一个线程在条件满足时，调用 notify_one/notify_all 唤醒等待的线程。


**重要方法：**
wait(unique_lock<mutex>& lock)：使当前线程进入无限等待，直到被notify_one()/notify_all()唤醒。调用时会自动释放锁，被唤醒后会重新获取锁。

wait_for(unique_lock<mutex>& lock, duration)：使线程最多等待指定时长，超时或被唤醒时返回。返回值：
std::cv_status::timeout：等待超时
std::cv_status::no_timeout：被唤醒

wait_until(unique_lock<mutex>& lock, time_point)：使线程等待到指定绝对时间点，超时或被唤醒时返回，返回值同 wait_for。

notify_one()：唤醒一个正在等待的线程（如果有多个，随机选一个）。

notify_all()：唤醒所有正在等待的线程。

关键特点与注意点：
必须配合 std::unique_lock 使用：
wait 会在等待时自动释放锁，这要求锁必须支持手动解锁，所以只能用 std::unique_lock，不能用 std::lock_guard。
虚假唤醒：
线程可能在没有调用 notify 的情况下被唤醒，所以必须在循环中检查条件，不能只判断一次：

while (!condition) {
    cv.wait(lock);
}
等待时释放锁
调用 wait 后，锁会被释放，让其他线程可以修改共享变量；被唤醒后，wait 会重新获取锁，保证临界区安全。

至此，我可以完成两个小练习，利用unique_lock和condition_variable模拟多线程工作，实现一个计数器和两个线程交替打印，熟悉“等待某条件成立”的多线程操作模式。并在此基础上，完成一个多生产者多消费者的模型，全面深化理解。

## 2026.4.1 Mutex,condition_variable，RAII思想构建PC模型

一、核心组件：模板编程练习
1. 核心成员
mutable std::mutex mtx：重点！const 成员函数（如 get_size）需加锁，mutable 允许 const 方法修改锁
两个条件变量：not_full（唤醒生产者）、not_empty（唤醒消费者）
stop_：停止标志，需加锁修改，避免信号丢失
max_size：队列容量，const 成员，构造函数初始化
2. 核心接口
构造函数：explicit 禁止隐式转换，带默认容量，符合多线程类规范
push：用unique_lock（配合条件变量），wait 避免虚假唤醒，std::move减少拷贝，生产后唤醒消费者
pop：同 push 逻辑，wait 等待队列非空，消费后唤醒生产者，严格判断 “stop_+ 队列空” 才退出
stop：用lock_guard，加锁修改 stop_+notify_all，避免死锁
get_size：const 方法，必须加锁，返回 size_t

二、生产者 / 消费者函数
生产者：随机休眠模拟生产耗时，生成唯一任务 ID，调用 push，检查返回值判断是否停止
消费者：循环消费，pop 阻塞等待任务，模拟处理耗时，收到停止信号则退出
三、主线程控制
用emplace_back创建线程（不用手动写 std::thread，原地构造，高效）
先 join 生产者：确保所有生产完成，再进入收尾（避免生产中停止队列）
休眠：本来硬停止两秒，后来考虑到实际情况改为每500ms轮询
调用 stop：唤醒所有阻塞线程，发送退出信号
join 消费者：确保所有任务处理完毕，安全退出

四、核心知识点
RAII 锁：lock_guard（简单场景用，轻量）、unique_lock（条件变量必须用，灵活），自动加解锁，避免死锁
条件变量：必须配合 unique_lock，wait 自动释放 / 重加锁，循环判断条件避免虚假唤醒
线程安全：所有操作共享资源（队列、stop_）必须加锁，杜绝数据竞争
避免死锁：stop 加锁、单一锁、notify_all 唤醒所有线程，无循环等待

## 2026.4.2 线程池基础
有了学习生产者消费者模型的基础，我下一步决定学习基础线程池的构建。
最根本的问题是，我们为什么需要一个线程池？
在我学习完多线程编程基本知识后，对原先的单线程循环服务器进行优化，改为每有一个访问请求就创造一个新的线程让它去跑，主线程无需考虑它的情况。然而，每有一个请求就要创造一个线程，线程执行完毕后还有销毁的过程，这种操作流程带来了巨大的额外开销，甚至还有崩溃的风险。为了解决这个问题，我们考虑进行线程的固定复用，即一开始就创造固定数量的工作线程，让它去任务队列里反复取任务，执行完之后不销毁，而是等待下次进行复用。
在实现过程中，我逐步理清了线程池的四大核心组件及其协作逻辑：
**任务队列 queue<function<void()>>**：用来存放待执行的任务。function<void()>是C++11的通用可调用对象包装器，能把普通函数、lambda等统一封装成任务，直接用task()即可执行。
线程数组vector<thread>：预先创建并管理固定数量的工作线程，实现线程复用。
互斥锁mutex：整个线程池只使用一把锁，专门保护共享的任务队列，避免多线程竞争导致数据混乱。特别需要注意的是，**锁的作用域至关重要，只在操作队列时加锁，任务执行时必须释放锁，否则会严重阻塞其他线程，这是高并发的关键**。
条件变量condition_variable：统一管理所有工作线程，没有任务时让线程休眠以节省 CPU，有任务时唤醒线程，退出时唤醒所有线程安全结束。

特别注意地：使用 atomic<bool> 作为停止标记，保证多线程下安全关闭线程池，析构时等待所有线程执行完毕再退出。
最终完成的线程池完整实现了生产者消费者模型：submit 作为生产者向队列添加任务，工作线程作为消费者循环取任务执行，实现了一个线程安全的线程池。


## 2026.4.3-4.4 线程池深化学习
实现基本的线程池并检验代码逻辑基本无误后，写一个测试线程池功能的测试cpp，尝试对线程池进行优化。我首先能注意到的是：这个线程池目前只能支持submit没有返回值的函数，这跟不上实际的生产需求，因此要学习如何让线程池支持返回值。
为了解决返回值的问题，我们要面临以下的困难：任务队列接收的是std::function<void()>，但是实际操作中任务函数往往有参数也有返回值，这两者间的矛盾怎么解决？线程操作中只有join()用于等待线程执行结束，而工作线程和主线程属于不同的线程，即使向线程传入了有返回值的函数，主线程也无法直接获取该返回值。
为了解决第一个问题（参数与队列类型不匹配），我们引入std::bind，它的作用是将函数和它的参数绑定在一起，生成一个新的无参可调用对象，使得其可以适配std::function<void()>的队列要求。举例：对于函数int add(int a, int b) { return a + b; }，通过auto func = std::bind(add, 1, 2);绑定后，调用func()就等价于调用add(1,2)，这样就解决了任务参数的适配问题。
要解决第二个问题（跨线程获取返回值），我们需要一个能够跨线程传递返回值的“通道”，这个通道要能实现：在工作线程中存入返回值，在主线程中取出该返回值。这个通道就是std::future & std::promise！其中，std::promise用于在工作线程中通过set_value()存入返回值，std::future用于在主线程中通过get()获取返回值。
在此基础上，我们可以使用更高一级的封装——std::packaged_task，它的核心作用是包装一个带返回值的可调用对象（函数、lambda等），当执行这个包装后的任务时，它会自动将返回值存入内部的共享状态，外界无需手动调用set_value()，只需通过get_future()就能获取对应的std::future，进而拿到返回值，这大大简化了程序员的操作。
在解决上述两个核心问题后，我们将目光重新投向执行层面，发现了一个新的问题：std::packaged_task是不可拷贝的，而线程池的任务队列std::queue<std::function<void()>>，以及lambda捕获，都要求存入/捕获的对象具备可拷贝性，std::packaged_task的不可拷贝性与这个要求冲突！
因此，我们采用的解决方案是：在std::packaged_task外套一层可拷贝的智能指针std::shared_ptr。std::shared_ptr本身支持任意拷贝，且多个拷贝会指向同一个std::packaged_task对象，同时它具备生命周期安全的特点，能够完美适配多线程编程场景。具体实现思路是：通过std::make_shared方法创建std::packaged_task的共享指针，再在lambda中捕获这个共享指针，这样既满足了可拷贝要求，又能正常执行任务。
实际生产中，工作线程所要执行的任务函数，其参数类型和返回值类型极其多样，为了对这些不同类型的任务进行统一封装，让submit方法支持任意函数和任意参数，我们引入了可变参数模板和std::forward。其中，模板<typename F, typename... Args>用于接收任意类型的函数F和任意数量、任意类型的参数Args...；std::forward用于实现“完美转发”，保留参数的原始值类别（左值/右值），避免不必要的拷贝，提升效率；再配合decltype(f(args...))自动推导函数的返回值类型，最终构成了通用的submit方法，实现了线程池对任意任务的支持。


## 2026.4.5 退出问题
解决线程池的返回值问题后，在与大模型讨论优化方法时，我注意到了另一个问题：现有的线程池析构函数最终只等待线程结束，而非等待任务全部完成，尽管在构造函数的判断逻辑中有对任务序列的判空逻辑，这段代码仍然有可优化之处。我想到的方法是，让我们可以显式地看到所有任务已经完成，并且在析构函数中增加一段判断任务完成的代码，其次再等待线程结束。
为了达到这个效果，考虑在线程池的底层构造里增加一个变量pending_task来记录已经提交但尚未完成的任务数，提交时该变量增加，而任务执行完毕后减少，pending_task变量使用atomic型保证多线程加减安全。

## 2026.4.6 构造服务器的新认识
把视角重新回到我的TCP_Server上面来，这个基础架构还有诸多不足之处，严重影响了它的实用性。梳理一下我利用学习的知识优化服务器架构的过程。
注意我初始的接受连接部分代码：
while(true){ 
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_socket = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            continue;
        }
        cout << "Client Connected! IP: " << inet_ntoa(client_addr.sin_addr) << endl;
        char buf[1000] = {0};
        int byte_read = recv(client_socket, buf, sizeof(buf) - 1, 0);
        cout << "Received: " << buf << endl;
        const char* response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Hello!</h1>";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
    }
首先，容易发现：当我们没有接收到连接时，整个线程在accept处阻塞，CPU空转闲置，利用率极低。其次，recv()阶段的效率也完全依赖于客户端发送数据的效率，一旦此处发送效率低，线程又被挂起。整个死循环服务器只有处理完一个客户端请求才能处理下一个，大部分时间用于等待I/O。而不是真正执行计算，这直接体现出了服务器基础架构层面的问题。
在学习多线程知识之后，我对这段核心代码进行第一次重写：
    vector<std::thread> Threads; 
    while(true){ 
        ...
        cout << "Client Connected! IP: " << inet_ntoa(client_addr.sin_addr) << endl;
        thread client_thread(Handle_Client, client_socket, client_addr);
        client_thread.detach();  // 分离线程，不等待它结束
    }
     for(auto& t : Threads) {
        if(t.joinable()) {
            t.join();
        }
    }
经过这次改写，核心处理逻辑变为：每当接收到一个请求之后，主线程把它丢给一个新建的子线程让它一边处理去，自己继续接收别的请求。这解决了每次只能处理一个客户端的问题，但是线程创建，销毁和上下文切换的过程带来了巨大的额外开销，涌入大量请求时，给每个请求分配一个线程的开销是无法承受的，并且accept()阶段阻塞的问题并没有在这次优化中被解决。
完成基本的线程池后，可以执行第二次改写：
threadpool pool(8); //优化：创造一个8线程线程池
    while(true){ 
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);//debug：此处用到的IP长度必须是socklen_t类型，其本质是一个无符号整型，用int会报错
        int client_socket = accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Accept Error!");
            continue;
        }
        cout << "Client Connected! IP: " << inet_ntoa(client_addr.sin_addr) << endl;
        pool.submit(Handle_Client, client_socket, client_addr); //将处理客户端请求和函数和所有参数传入submit令其打包成packagedtask
    }
此次优化实现了复用线程，减少创建开销，并且控制了并发线程数，不至于一次性产生大量线程。
但本质上，accept()依旧是阻塞的，一旦阻塞，会影响新连接的处理，同样地，线程池的大量闲置仍然影响了CPU资源的利用率。
于是我们可以提出一个解决方向，即不再让快速的CPU等待慢速的IO，线程只在有数据时被唤醒，这就有效地优化了资源浪费问题。

## 2026.4.7 IO模型认识
当前服务器：阻塞 I/O + 线程池
线程调用 accept / recv 时，若没有数据 / 连接，会在内核中被阻塞挂起。
挂起期间不占用 CPU，但会占用线程资源（栈、内核数据结构等）。
一个线程同一时间只能绑定并等待一个连接。
一旦某个连接因慢客户端等原因长时间阻塞，就会直接浪费整个线程。
线程池容量有限，线程被大量阻塞后，服务器处理能力会急剧下降。
改进第一步：非阻塞 I/O
将 socket 设置为非阻塞，accept / recv 无论有无数据都会立即返回。
线程不会被卡住，可以循环处理多个连接，不再被单个连接绑定。
但线程需要不断主动轮询所有文件描述符，全程处于运行状态。
导致 CPU 持续空转，大量浪费时间片，系统负载高，业务效率反而变差。

在查询资料过后，我了解到可以用IO多路复用模型来进行优化！所谓IO多路复用的本质，是把反复轮询是否有客户端请求的工作从线程递交给了操作系统内核，让内核来同时监控大量的连接。
初始的解决方案名为select，简而言之，它的原理是把大量fd放进一个集合，调用select之后内核会遍历所有fd，检查哪一个处于就绪状态。一旦其中一些fd变得可以读写，就提醒线程开始工作。这样优化的好处显而易见，它可以直接解决两个问题：无请求的时候线程可以直接挂起，不占CPU；不需要一个线程对应一个连接，大大省下开销。在此基础上可以对我的server进行第一次优化，采用select方法构建。

## 2026.4.8-4.9 Select学习
在使用 select 重构服务器代码之前，我们需要先了解 select 服务器的完整工作流程。
select 是一种单线程 I/O 多路复用模型，核心优势是：让一个线程同时监控多个文件描述符，解决传统模型中accept和recv两处阻塞无法同时处理的问题。
**工作流程：**
先创建服务端监听socket（listenfd），完成bind和listen，准备接收客户端连接。再创建一个总监听集合，将listenfd加入其中。这个集合里的所有 fd，都会由内核统一监控。
前置事项实现完毕后，进入select主循环：循环是select模型的工作主体，每次循环必须复制一份完整的总监听集合，传给select函数（select会修改传入的集合）。select开始阻塞等待，直到集合中任意一个 fd 就绪（新连接到达 / 客户端发送数据）。内核检测到就绪事件后，select返回，并自动清空未就绪的fd，只在集合中保留就绪fd。
遍历所有 fd，检查哪些处于就绪集合中：如果是监听fd就绪，则处理新连接accept，将新的客户端fd加入总监听集合。如果是客户端fd就绪，则调用recv读取数据，并进行业务处理、回复响应。
当客户端断开连接、读取出错或处理完成时：关闭对应的socket并将该fd从总监听集合中移除，避免无效监听。
**关键方法**
fd_set master_set;   // 总监听集合：保存所有需要监听的 fd（listenfd + 客户端fd）
fd_set read_fds;     // 临时就绪集合：每次传给 select，会被内核修改
FD_ZERO(&set);        // 清空集合，初始化必须使用
FD_SET(fd, &set);     // 将一个 fd 加入监听集合
FD_ISSET(fd, &set);   // 判断某个 fd 是否在就绪集合中（是否有事件）
FD_CLR(fd, &set);    // 将一个 fd 从集合中删除（断开连接时使用）
int select(
    int max_fd + 1,      // 检查范围：0 ~ max_fd
    fd_set* read_fds,    // 监听“读事件”的集合
    NULL,                // 写集合（不用）
    NULL,                // 异常集合（不用）
    NULL                 // 超时时间：NULL = 永久阻塞
);

## 2026.4.10-4.11 select->poll
目前，select服务器已经完全远离了生产环境，它本身带有严重影响投入实际使用的缺点，这是它的设计缺陷导致的。
select服务器的硬伤是什么？
1：select方法中创建的fd_set是被linux内核写死的固定大小位图，默认宏决定其最多监听1024个fd。想要拓展需要重新编译内核，非常麻烦。
2：以select服务器中的实际代码为例，必须写一句read_fds = master_set给select用。这意味着每次都要把所有fd拷贝一遍，随着连接数增加这个过程会占用更多资源。
3：遍历效率不理想，每次要遍历0-maxfd，进行大量FD_ISSET判断。尽管其中的很多fd根本未曾就绪，或者干脆就是空的。
4：作为内核系统调用，select每次需要将临时集合read_fds从用户空间拷贝到内核空间，修改完毕后又拷贝回去，连接增加则性能暴跌。

对于select面临的诸多困境，我们的第一个解决方案是poll。
poll解决了select的部分痛点：
1：最核心地，其解除了select模型1024硬限制问题，其最大长度只受系统内存限制，比1024个fd大得多。
2：创新性地运用了event和revents的事件模型，想要监听的事件和内核返回的事件分离，无需破坏原始数据。相反地，select所有的操作都发生在fd_set上，传入的set还会被内核直接暴力修改，最后返回的仍然是fd_set，逻辑相当混乱。
3：避免了max_fd的维护。

poll的特殊方法：
**pollfd结构体**：poll模型最标志性的结构，用于替代select的fd_set，存放要监听的fd和事件，其定义如下：
struct pollfd {
    int fd;        // 你要监听的文件描述符（listenfd / clientfd）
    short events;  // 【你告诉内核】你想监听什么事件
    short revents; // 【内核告诉你】实际发生了什么事件
};

**POLLIN**：poll专属的事件宏，表示“有数据可以读”。对于listenfd，这代表着有新连接，对于clientfd，这代表着有客户端发数据了。主要用fd来区分究竟发生了什么事件。代码中主要有两次用到，
fds[i].events = POLLIN;    //告诉内核要监听读事件
if (fds[i].revents & POLLIN)// 内核返回：真的有读事件（此处语法代表的意思：返回的事件中是否包含POLLIN？）

**poll函数**：用于把所有要监听的pollfd交给内核，内核阻塞等待直到事件发生。
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
传入三个参数，分别为结构体数组首地址，有效fd数量，以及timeout，表示等待多久后返回，-1表永久阻塞

## 2026.4.12 epoll初探
poll尽管在select的基础上做出了巨大优化，使得其真正具备了可用性，但依旧只是其优化版本，未能解决本质问题：每次调用poll需要把整个fd数组拷贝到内核态（其中绝大部分可能根本没有事件，完全是无效开销），内核需要遍历所有fd，以寻找有读事件的fd，用户态返回后还要重新遍历fd是否有revents。一旦出现大量连接，这个一次拷贝两次全量遍历的O(N)过程将会产生极大的性能负担，连接越多效率越低，一旦支持1w以上的连接，select和poll极可能直接卡死。
在这个问题上，epoll几乎直接优化了所有环节：epoll只在新增/删除fd时通过epoll_ctl传递一次，内核持久维护事件表，循环中无全量拷贝。基于硬件中断/回调机制，网卡/设备有数据时主动通知内核，内核直接将对应 fd 加入就绪队列，无需轮询扫描。epoll_wait 只返回已就绪的 fd 列表，用户态仅遍历就绪项，无无效开销。

有了这些基础知识，又可以将poll服务器改为基础的epoll服务器了！

### 一、整体流程
创建 listenfd，bind + listen
创建 epoll 实例（内核红黑树事件表）
将 listenfd 注册到 epoll，监听读事件 EPOLLIN
循环调用 epoll_wait 等待就绪事件
处理两类就绪事件：
listenfd 就绪：accept 新连接，并将 clientfd 加入 epoll
clientfd 就绪：recv 读取 HTTP 请求，回发响应
客户端断开时：从 epoll 中删除 fd 并关闭

### 二：关键代码
1：创建内核事件表
// 关键api: 创建一个epoll内核事件表（红黑树），返回句柄epoll_fd
int epoll_fd = epoll_create1(0);

2：存储结构体epoll_event
// 所有的fd在内核中都是以epoll_event的形式存储的
// 包含：所监听的fd+期望触发的事件
struct epoll_event ev;
ev.events = EPOLLIN;        // 基础LT水平触发，监听读事件
ev.data.fd = listenfd;      // 绑定要监听的fd

3：添加事件到事件表
//对事件表epoll_fd执行操作EPOLL_CTL_ADD添加listenfd的事件ev到epoll_fd指向的事件表
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listenfd, &ev)

4：定义接收epollwait返回的就绪事件
struct epoll_event events[1024];

5：阻塞等待就绪事件，只返回有事件的fd列表
int n = epoll_wait(epoll_fd, events, 1024, -1);

至此，我已经对epoll有了一定的理解，并完成了一个基础的epoll服务器！

### select和epoll的效率差距原因
为什么 epoll 比 select 快？
**等待队列与调度开销（新知识）**
select：n 个连接对应 n 个等待队列，每次调用需 n 次 thunk 挂起 / 唤醒，O (n) 调度开销。
epoll：全局 1 个等待队列，仅 1 次 thunk 开销，O (1) 调度，连接数不影响性能。
**数据拷贝**
select：每次循环全量拷贝 fd 集合，n 次拷贝开销。
epoll：仅增删 fd 时拷贝一次，内核维护红黑树，无重复拷贝。
**遍历开销**
select：内核 + 用户态两次全量遍历所有 socket，O (n) 无效开销。
epoll：内核不遍历，仅维护就绪队列，用户态只遍历就绪 socket，O (k) 高效（k<<n）。
**工作模式**
select：主动轮询，每次调用全遍历，大量无效操作。
epoll：事件驱动，socket 有数据主动上报，仅处理有效事件。

## 2026.4.13-4.15 Epoll服务器缺陷分析与后续优化
### 代码缺陷大观
目前的基础Epoll服务器，距离工业可用仍有相当距离。参考原项目的解析文档，我梳理出一下的缺陷。

#### 阻塞 I/O
代码现象：accept 和 recv 均操作默认的阻塞型文件描述符（fd）。
核心缺陷：虽然在 LT 模式下，epoll_wait 唤醒能保证“必定有连接”或“必定有数据”，不会发生传统阻塞模型中的“死等”。但是，如果遇到恶意的或网络极差的“慢客户端”，recv 仍会因等待读满预期数据而耗费大量时间。
最终后果：由于是单线程同步执行，主线程一旦被某一个慢连接的 recv 绊住，整个事件循环就会停滞。所有其他客户端的并发请求均被堵塞，造成“一个慢请求拖死整个服务器”的雪崩效应。
#### LT模式引发的大量系统调用和不必要上下文切换开销
代码现象：事件注册为 ev.events = EPOLLIN;（默认 LT 模式）。
核心缺陷：LT 模式的语义是“只要缓冲区还有数据，就会一直通知”。假设客户端发送了 2000 字节，而服务器 recv 的单次接收上限是 1024 字节。第一轮读取后剩余 976 字节，内核会立刻再次唤醒 epoll_wait，强制要求主线程继续读取。
最终后果：在高并发大流量场景下，未能一次性读完的数据会导致epoll_wait被频繁、重复地触发。这会产生海量的内核态与用户态之间的上下文切换开销，导致 CPU 负载飙升，吞吐量骤降。
#### 局部缓冲区导致的数据分片丢失 (TCP面向字节流特性)
代码现象：使用 char buf[1024] = {0}; 作为局部的、临时的接收容器。
核心缺陷：TCP 是面向字节流的协议，一个完整的 HTTP 请求报文极大概率会被底层网络拆分成多个 TCP 报文段分批到达。当第一个分片到达时，主线程将其读入 buf，但因为报文不完整无法解析，函数随之返回，局部变量 buf 被销毁。
最终后果：当请求的后半截数据在下一次 epoll_wait 触发时到达，前半截数据早已丢失。在非阻塞或长连接的场景下，服务器根本无法拼接出完整的 HTTP 报文，直接导致业务层解析失败。
#### I/O 操作与业务逻辑深度耦合
代码现象：主线程一条龙负责 epoll_wait 监听、recv 读取、HTTP 协议解析、组装 HTML 响应以及 send 发送。
核心缺陷：这种设计完全违背了 Reactor/Proactor 的架构思想。业务逻辑（尤其是可能涉及查库、读文件的耗时操作）与底层的网络 I/O 绑定在了同一个执行流中。
最终后果：业务处理的耗时会直接占用监听端口的时间。主线程在处理业务时，无法epoll_wait接收新连接，导致大量新连接堆积在内核 TCP 队列中。同时，这种架构完全无法利用现代多核 CPU 的并行计算能力，并发上限极低。
#### 异常连接状态监听缺失 (FD 泄漏隐患)
代码现象：仅监听了基础的读事件，未注册 EPOLLRDHUP、EPOLLERR 等异常标志位。
核心缺陷：网络环境极其复杂，客户端随时可能发生断电、断网或直接杀死进程等非优雅断开（RST 包）的情况。
最终后果：服务器由于未监听这些异常事件，无法第一时间感知客户端的断开，相关的 socket 描述符无法被及时close()释放。随着时间推移，服务器会积累大量处于 CLOSE_WAIT 状态的死连接（FD 泄漏），最终因耗尽系统的文件描述符配额而崩溃。
#### 客户端自行断连产生SIGPIPE信号，导致进程异常退出
代码现象：服务器直接对客户端socket调用send，未考虑连接的实时存活状态。
核心缺陷：在 TCP 协议栈中，如果客户端已经关闭了读取端（发送了FIN），而服务器仍尝试向其发送响应数据，第二次 write/send 操作会引发内核向进程发送 SIGPIPE信号。
最终后果：在 Linux 系统中，SIGPIPE 信号的默认动作是直接终止进程。这会导致服务器在没有任何报错日志的情况下突然崩溃退出，完全丧失鲁棒性。
#### 单次 Accept 导致的连接堆积延迟
代码现象：if(curr_fd == listenfd) 分支中，仅调用了一次 accept。
核心缺陷：当面临高并发场景时，同一毫秒内可能有成百上千个新连接涌入内核的全连接队列（Accept Queue）。单次 accept 只能从队列中摘取一个连接。
最终后果：虽然 LT 模式会在下一次循环继续提醒 listenfd 活跃，但每次只摘取一个连接的处理效率太低。大量积压的连接需经历多次 epoll_wait 的轮转才能被建立，导致新用户建立连接的往返延迟急剧增加，严重影响客户端体验。

### 优化思路
#### 引入非阻塞IO
面对可能存在的慢客户端卡死服务器问题，我们采取的思路是引入非阻塞IO，针对clientfd和listenfd进行改动，使得它们在面对慢客户端时，不会被阻塞在原地无所作为，可以转头先去解决别的问题，处理别的连接。
关键函数：setnonblocking用于将fd附上非阻塞属性！
//fcntl：Linux系统函数，全称file control，专门用来查看/修改fd的属性
int setnonblocking(int fd) {
//获取 fd 原本的所有状态属性，存储到old_option里（必须先用fctnl读出来保存，不可以直接set）
    int old_option = fcntl(fd, F_GETFL); //F_GETFL:flag get file status flags读出属性
    //在原有属性基础上，添加非阻塞标记
    int new_option = old_option | O_NONBLOCK; //用**位或运算**添加属性NONBLOCK
    //把新属性设置回fd，让非阻塞生效
    fcntl(fd, F_SETFL, new_option); //F_SETFL：显然，设置属性
    //返回旧属性
    return old_option;
}
对clientfd设置NONBLOCKING之后，它们的行为模式产生巨大变化：从没有数据就卡死等待，到如果数据没有准备好，就立刻返回-1并把errno（linux的全局错误码变量）设置为EAGAIN（专属于非阻塞IO的错误符，代表当前没有连接或数据，而非出现故障需要关闭socket）.

**为什么listenfd也需要设置非阻塞？**：既然LT模式保证了没有连接的情况下不会触发epoll_wait，为什么listenfd依旧需要设置非阻塞？除了支持循环读写之外，它还有另外一层作用：客户端完成三次握手后触发epoll_wait，**但是epoll_wait无法保证listenfd去accept的时候连接仍然正常可连**，因为在这个通知过程中，可能出现**客户端异常取消**的情况，这会使得阻塞IO无法接受连接，从而原地阻塞。

#### 循环读写
给accept和recv都加上while(true)的死循环，直到errno == EAGAIN判断不再有数据和连接后再跳出等待下一次epoll_wait，解决了原来高并发情况下单次accept效率过低导致大量连接堆积超时，反复激发epoll_wait浪费系统资源的问题；以及一次epoll_wait仅有单次recv导致分片接收的数据无法整合成完整数据的问题。

**非阻塞IO的改动是循环读写可用的前置必要条件**：设想阻塞IO配合循环读写，一次循环中读完所有数据和连接之后的最后一次请求会因为读不到数据和连接而阻塞在原地。

#### LT->ET
在设置了非阻塞IO和循环读写之后，我们可以将LT模式改为ET模式，出现状态变化再调用epoll_wait，利用循环读写一次性读完所有数据/accept完所有连接，而不必用LT模式反复触发epoll_wait带来额外的不必要上下文转换开销。
ev.events = EPOLLIN | EPOLLET; 
client_ev.events = EPOLLIN | EPOLLET; 
利用位或运算修改内核规则，规定只有状态发生变化时才触发一次。

#### 增加对SIGPIPE信号的忽略逻辑 + 提前监听客户端提前断开事件EPOLLRDHUP
引入signal.h，在主函数里增加 signal(SIGPIPE, SIG_IGN); //设置忽略SIGPIPE
可以有效解决send时发现客户端断连，无效write产生SIGPIPE导致的关闭。
client_ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
TCP对端主动关闭连接发送FIN会产生事件标志EPOLLRDHUP，监听之可以提前直到对方已关连

#### 增加对EPOLLHUP | EPOLLERR等其他复杂情况的判断
及时感知其他复杂网络环境导致的异常情况，必须在处理fd之前先确定是否存在EPOLLRDHUP（单侧断连，和前述优化的情况触发时机不同）| EPOLLHUP（双侧断连）| EPOLLERR（连接错误：断网崩溃超时等）等异常情况，一旦出现，后续不会再触发EPOLLIN/EPOLLOUT（可读/可写）。应当先做判断，确定无异常再再对fd进行针对性处理（accept/recv）。因此在读取fd后，先进行如下判断：
//用位运算确定是否有异常标志
if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
    cout << "fd: " << curr_fd << " 异常断开，已清理" << endl;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, curr_fd, NULL);  
    close(curr_fd);                                    
    continue;                                        
}

#### 新增一个结构体：全局大数组内有string形式缓冲区记录断续传来的数据并组合在一起，防止局部数组导致的丢数据
struct ClientState {
    int fd;
    std::string buffer;
};
ClientState users[65536];

找到一个新的客户端连接，先把它丢进全局大数组，此时没有recv，接收到的数据初始化为空
users[client_fd].fd = client_fd; 
users[client_fd].buffer = "";

每次recv，把连接读取到string中，让它自动扩容把新内容加上，确定读完前不send
char buf[1024] = {0};
int byte_read = recv(curr_fd,buf,sizeof(buf)-1,0);
if(byte_read > 0)
users[curr_fd].buffer += buf;

确定读完后，得到的buffer就是所有数据，可以回应了
else if(byte_read < 0)
if(errno == EAGAIN||errno == EWOULDBLOCK){
if(!users[curr_fd].buffer.empty()){
std::cout << "fd " << curr_fd << " 的完整请求内容: \n" << users[curr_fd].buffer << endl;
const char* response = "HTTP/1.1 200 OK\r\n\r\n<h1>Hello</h1>";
send(curr_fd, response, strlen(response), 0);
users[curr_fd].buffer.clear();
    }
break;
}
记得给每次退出增加清空buffer环节！

#### 线程池 + EPOLLONESHOT，IO与业务解耦
1. 注册客户端事件时增加 EPOLLONESHOT
在 accept 新连接后，给 client_ev 加上：
client_ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
作用：EPOLLONESHOT使得一个fd对应的事件在执行过一次后不再可执行，防止多个线程同时接管。

2. 主线程不再处理业务，只负责处理与内核有关的网络IO：
读数据 → 拼到全局 buffer
数据读完 → 把任务丢给线程池
立刻返回事件循环，处理下一个连接
else if(byte_read < 0){
    if(errno == EAGAIN||errno == EWOULDBLOCK){
        if(!users[curr_fd].buffer.empty()){
            pool.submit(handle_client, curr_fd, epoll_fd);
        }
        break;
    }
}

3. 业务处理函数 handle_client 在线程池中执行
void handle_client(int fd,int epoll_fd){
    // 处理HTTP请求（解析、查找、拼接响应…）
    std::cout << "fd " << fd << " 的完整请求内容: \n" << users[fd].buffer << endl;

    // 发送响应
    const char* response = "HTTP/1.1 200 OK\r\n\r\n<h1>Hello</h1>";
    send(fd, response, strlen(response), 0);

    // 清空缓冲区
    users[fd].buffer.clear();

    //重点：重新激活 EPOLLONESHOT，让fd能接收下一次请求
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.fd= fd;
    epoll_ctl(epoll_fd,EPOLL_CTL_MOD,fd,&ev); //用MOD修改状态参数，重新激活ONESHOT
}

4. 主线程创建线程池
int main() {
    // 忽略SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    threadpool pool(8);
...
}

经过这段优化，我终于成功构建了Webserver的基本雏形，具备了一定的工业可用性。

## 2026.4.16 HTTP报文基础认识
### 一、HTTP报文整体基础知识
HTTP 协议定义了两种报文：请求报文和响应报文）。两者格式都由四部分构成，必须严格按照规范构造，否则将无法正常解析。
### 二、HTTP 请求报文
由四部分组成：
请求行 → 请求头部 → 空行 → 请求数据
1. 请求行
位于报文第一行，作用是说明本次请求的基本信息。
**形如：请求方法 资源路径 HTTP版本**
示例：GET /562f25980001b1b106000338.jpg HTTP/1.1
**常见请求方法**：
GET：请求获取资源，一般不携带数据
POST：向服务器提交数据（登录、注册、上传等）
2. 请求头部
由若干行键:值结构组成，用于传递附加信息。
**常见字段**：
Host：服务器域名
User-Agent：客户端浏览器 / 系统信息
Accept：客户端可接收的数据类型
Accept-Encoding：支持的压缩方式（gzip 等）
Content-Type：POST 请求的数据格式
Content-Length：POST 请求数据长度
Connection：连接方式（Keep-Alive /close）
3. 空行
必须存在，作用是分隔请求头和请求数据。即使没有请求数据，空行也不能省略。
4. 请求数据
GET一般没有，POST才有，存放表单、JSON 等提交内容
示例：name=Professional%20Ajax&publisher=Wiley
### 三、HTTP响应报文
由四部分组成：状态行 → 响应头部 → 空行 → 响应正文
1. 状态行
形如：HTTP版本 状态码 状态描述
示例：HTTP/1.1 200 OK
**常见状态码**：
200 OK：请求成功
404 Not Found：资源不存在
500 Internal Server Error：服务器内部错误
2. 响应头部
服务器给客户端的附加信息，同样是键值对格式。
常见字段：
Date：响应时间
Content-Type：响应内容类型（text/html、image/jpeg等）
Content-Length：响应体长度
Server：服务器信息
3. 空行
必须存在，分隔响应头与响应体。
4. 响应正文
真正返回给客户端的内容：HTML 页面 / 图片 / 视频 / JSON 数据
浏览器根据 Content-Type 进行渲染/解析！
### 四、构造 HTTP 报文的关键点
格式必须规范，每行用换行分隔，头部键值用:分隔；
请求行不能为空，是解析起点；
空行不能缺失，否则会导致报文结构解析错误；
正文长度必须与Content-Length一致，否则会截断或超时。

## 2026.4.17-4.18 业务逻辑优化初试
实现了基本的server架构之后，为了真正实现一个可用的服务器，我们需要把目光转向应用层协议处理。在现有代码中，为了注重服务器架构本身，我们把业务逻辑简化为输出收到的报文并返回hello，这和实际应用相距甚远！在实际操作中，客户端发送到服务端的是上面所示的请求报文，里面含有请求的类型，服务和支持返回的内容等大量信息，业务逻辑首先要做的就是解析客户端发来的报文，读出其中蕴含的信息。
有了昨天的前置知识，我们知道请求报文都是按行书写的，首先要做的就是想办法把报文按行解析，不难发现，每一行结束的标志是\r\n，我们可以通过这个标志的出现来判断读完一行！

size_t first_line_end = request.find("\r\n"); 
利用find函数，我们可以找到第一行结束的位置，再结合substr，这就分割出了第一行。
    stringstream ss(first_line);
    string method,url,version;
    ss >> method >> url >> version;

用stringstream进行简单分割，读出第一行里的三个部分：方法，路径，版本。使用字符串操作组合出本地的文件存储路径。
stat(filepath.c_str(),&file_stat)使用Linux的stat方法查看路径里是否有所需文件，文件不存在则输出404 Not Found，手动编辑响应报文体。反之只需要发送报文头提示200 OK，用while循环把文件一点一点send过去。

std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(file_stat.st_size) + "\r\n\r\n"; 
send(fd,header.c_str(),header.size(),0); 
int src_fd = open(filepath.c_str(),O_RDONLY); 
char file_buf[1024]; 
int bytes_read;

while((bytes_read = read(src_fd,file_buf,sizeof(file_buf))) > 0){
    send(fd,file_buf,bytes_read,0);
}
close(src_fd);

## 2026.4.19 文件服务优化
我们当前实现的是最基础的文件服务逻辑。很容易注意到这样的业务逻辑的缺陷：一次只能发送1KB大小的文件内容，对于较大的文件来说传输效率非常堪忧，并且考虑这个过程的其他开销：使用read时进入内核态，需要将文件内容读入内核空间，再将这些内容读到用户态内存file_buf，这个过程中的大量切换和数据拷贝开销不小。读到缓冲区后，还需要反复动用send系统调用，将用户态缓冲区里的东西再次拷贝回内核，且触发切换开销（文件大的时候这个次数可能非常大），这个过程含有大量的性能浪费。
如何减少这些无意义的浪费？考虑两个主要部分：内核态和用户态的切换开销，以及数据在用户态空间和内核态空间反复拷贝的开销。
问题是，用户态空间和内核态空间隔离不可共享，需要另寻方法让内核将文件读到文件内容放到内核内存区后，避开拷贝过程也能让用户态内存拿到。
对于数据拷贝问题：解决方案是mmap(内存页表映射)：在数据被读到内核内存区后，直接将用户态的虚拟地址指向这块内核内存区，直接获取数据，绕开了内核到用户和用户到内核的两次拷贝！
对于send次数过多的问题，解决方案是writev(聚合写)：mmap使得我们摆脱了1KB大小的缓冲区，将几千次send变成两次send(一次header一次body，地址不连续)。writev使得可以一次性传输内存地址不连续的多片数据，这代表着它可以将响应头和响应体的整个大文件用一次系统调用直接发出去，直接避免了多次send！
std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(file_stat.st_size) + "\r\n\r\n"; //stat返回=0，找到了
        int src_fd = open(filepath.c_str(),O_RDONLY); //READ_ONLY打开
        //进行内存映射。传入参数：映射虚拟内存地址（NULL自动分配），映射内存长度，映射权限（只读），映射类型（私有映射），映射的文件描述符，文件偏移量（0表示开头开始映射）
        //返回值是映射后的内存首地址，直接靠这个地址即可访问文件
        char* file_address = (char*) mmap(NULL,file_stat.st_size,PROT_READ,MAP_PRIVATE,src_fd,0);
        close(src_fd); 
        //writev专用的数据块描述结构体，描述不连续内存，包含内存首地址和数据长度
        struct iovec iv[2];
        iv[0].iov_base = (void*)header.c_str();
        iv[0].iov_len = header.size();
        iv[1].iov_base = file_address;
        iv[1].iov_len = file_stat.st_size;
        writev(fd,iv,2); //聚合写：传入客户端socket描述符，iovec数组和要传送的数据块数量
        munmap(file_address, file_stat.st_size); //解除映射，释放资源

## 2026.4.20-4.21 主从状态机实现
为了符合真正的使用场景，我们还需要继续深化解析http报文的逻辑，容易发现，目前的解析逻辑比较简陋，只完成了解析首行的逻辑，暂时没有完成后面的header；并且用stringstream解析每一行有巨大的隐患：一旦发来的报文并不完全（粘包/半包/恶意链接），没有办法解析出三个部分，这可能导致崩溃和越界。并且没有对常见的长连接的支持。
面对复杂的网络环境，要解决粘包半包等情况，必须保持坚决的按段按行按步解析，要达成这个目的，我们采取的方法是引入**主从状态机**，从状态机只有一个任务：将报文按行切分。真正的解析任务由主状态机完成，其任务是记录当前解析到每一步（Request？Header？Body?），并且按当前状态执行相应的解析逻辑，解析得到的结果决定接下来的操作。
从状态机的逻辑只需检测\r\n切行即可，为了完成主状态机的解析任务，需要先定义几种不同的解析状态和解析结果。
enum CHECK_STATE { 
    CHECK_STATE_REQUESTLINE = 0, //请求行
    CHECK_STATE_HEADER, //头部
    CHECK_STATE_CONTENT //请求体
};

//列举各种解析结果
enum HTTP_CODE {
    NO_REQUEST,// 请求不完整，继续读数据
    GET_REQUEST,// 成功拿到完整 HTTP 请求
    BAD_REQUEST, // 请求语法错误（400）
    INTERNAL_ERROR // 服务器内部错误（500）
};
我们的客户端连接信息都存在ClientState结构体中，因此其中不仅要有fd和接收缓冲区buffer，还应存储当前的解析状态和解析到的下标位置，方便调用逻辑并进行接下来的解析；同理，解析得到的词条也存储在此，这个类中需要新的**初始化逻辑**：
 void init(int client_fd){
        fd = client_fd;
        buffer.clear();
        check_state = CHECK_STATE_REQUESTLINE;
        checked_idx = 0;
        keep_alive = false;
        content_length = 0;
        method.clear();
        url.clear();
        version.clear();
    }

**主从状态机逻辑实现**
//从状态机逻辑实现：字符串切分
std::string get_line(ClientState& client){
    size_t pos = client.buffer.find("\r\n",client.checked_idx);
    //没找着行，撤了
    if(pos == std::string::npos){
        return "";
    }
    std::string line = client.buffer.substr(client.checked_idx,pos - client.checked_idx); //用substr方法将当前行分割出来
    client.checked_idx = pos + 2; //这行已经切分出来，更新checked_idx，+2跳过\r\n
    return line;
}

//核心：主状态机逻辑实现
HTTP_CODE parse_http_request(ClientState& client){
    std::string line;
    //循环逻辑：能够读到行 / 读请求体（请求体不一定有换行符）
    while((line = get_line(client)) != "" || client.check_state == CHECK_STATE_CONTENT){

        switch(client.check_state){
            case CHECK_STATE_REQUESTLINE:{
                //正在解析请求行，由于能保证读到了行，可以直接用stringstream
                std::stringstream ss(line);
                ss >> client.method >> client.url >> client.version;
                //格式有错误，请求有问题！
                if(client.method.empty() || client.url.empty()){
                    return BAD_REQUEST;
                }
                client.check_state = CHECK_STATE_HEADER; //请求行只有一行，读完就更新到请求头了
                break;
            }
            case CHECK_STATE_HEADER:{
                //注意：请求头和请求体中间必定有一个空行，这可以作为切换状态的判断标准
                //读不到又分两种情况：这请求报文的请求体就是空的 / 有请求体，但是在空行同样读不到
                if(line.empty()){
                    if(client.content_length == 0){
                        return GET_REQUEST; //没请求体了，已经读完了
                    }
                    client.check_state = CHECK_STATE_CONTENT;
                    break;
                }
                //开始解析具体的词条!
                //是否connection词条？是否长连接？
                if(line.find("Connection:") != std::string::npos){
                    if(line.find("keep_alive") != std::string::npos){
                        client.keep_alive = true;
                    }
                }
                //解析请求体长度
                if(line.find("Content-Length:")!= std::string::npos){
                    client.content_length = std::stoi(line.substr(15)); //请求体长度：Content-Length:后面的子字符串转为数字
                }
                break;
            }
            case CHECK_STATE_CONTENT:{ 
                //请求体阶段不再按行读取，而是按字节判断
                //缓冲区里还没处理的字节数大于等于请求体长度，说明确实解析完毕了
                if(client.buffer.size() - client.checked_idx >= client.content_length){
                 return GET_REQUEST;
                }
                return NO_REQUEST; 
            }
            default:
                return INTERNAL_ERROR; //啥都不是？那就是未知内部错误！
        }
    }
    return NO_REQUEST; //连一个完整行都没读到，请求不完整
}

### 将主从状态机逻辑集成到处理函数中
在处理函数(Handle_Client())中对于主状态机返回的各种处理状态，跳转到对应的处理逻辑：
NO_REQUEST:请求不完整，更新事件属性（重置EPOLLONESHOT）让它回去重新recv。
BAD_REQUEST:请求语法有问题，不可能解析，直接断掉链接！
GET_REQUEST:收到完整请求，可以进行解析和响应。执行上一步已经完成的文件服务逻辑，并且增加对是否长连接(keep-alive)的判断，若为长连接则在服务后先不断连接，只需进行初始化重置；否则直接关闭连接删除事件。

**其他优化**

1：问题产生：完成前面的代码后，我在用浏览器访问的过程中发现一直重复卡在正在加载，无法进入预设的页面。经过反复排查，最后将问题锁定在前面实现的get_line逻辑：
std::string get_line(ClientState& client){
    size_t pos = client.buffer.find("\r\n",client.checked_idx);
    if(pos == std::string::npos){
        return "";
    }
    std::string line = client.buffer.substr(client.checked_idx,pos - client.checked_idx); 
    client.checked_idx = pos + 2
    return line;
}
考虑http报文中空行的判断：其可能是在请求头和请求体之间的空行，也可能是请求真的不完整，但是全都返回了""。产生混淆导致跳进了NO_REQUEST，永远不可能发送响应！因此我将读行和判断分开，重写从状态机的逻辑：
bool get_line(ClientState& client, std::string& line) {
    size_t pos = client.buffer.find("\r\n", client.checked_idx);
    // 没找着换行符：返回 false，表示请求还没收全
    if (pos == std::string::npos) {
        return false;
    }
    //
    line = client.buffer.substr(client.checked_idx, pos - client.checked_idx); 
    client.checked_idx = pos + 2; //跳过 \r\n
    return true;
}
用两种方法共同确定状态：是否成功读到？（有无\r\n）读到的是什么？
false就跳到NO_REQUEST，true则进入判断：非空正常解读，空行则切换解析状态至读请求体。

2：在接收到新连接后，先对新连接进行初始化。
3：增加新的情况：在网页中加入测试图片test.jpg，增加content-type判断逻辑
if(client.url.find(".jpg") != std::string::npos || client.url.find(".png") != std::string::npos) {
                header += "Content-Type: image/jpeg\r\n"; // 图片类型
            } else {
                header += "Content-Type: text/html; charset=utf-8\r\n"; // 网页类型
            }
优化之后，网页可以显示指定的照片test.jpg了！
4：在观察终端过程中，发现有额外请求favicon.ico，查询资料后发现这是网页图标的请求，因此增加了一个图标文件，并且学习了如何禁用网页缓存。

## 2026.4.22-4.23 集成MySQL实现账号注册登录

### MySQL准备和连接池构建
在此基础上，可以集成MySQL库实现账号注册登录的功能了。在linux上安装好MySQL服务并登录后，创建webserver库并授权本地C++连接，创建测试账号密码，完成C++调用MySQL API的前置准备。
首先我们明确，账号密码存储在MySQL库webserver数据库的user表内，每次查询需要创建MySQL连接，并且通过连接查询库中存不存在响应的账号密码。与线程池类似地，如果每一次查询数据库内容都需要新建MySQL连接，会严重影响性能，因此我们需要创建一个MySQL连接池，连接池内包含基础的构造函数，创建MySQL连接和归还连接函数。
值得注意的是：**MySQL连接池不能随意创建，全局只能存在一个连接池**，由于这个原因，连接池的构造函数是私有的，外界只能获取单例来访问唯一的连接池，向其请求连接。
'''
static MySQLPool* get_instance() {
        if (!instance) instance = new MySQLPool();
        return instance;
    }
'''
instance是连接池池内预先构造的静态连接池指针，指向唯一一个连接池。如果在调用该函数的时候还没有创建，就会自动创建一个实例。通过理解这种方式，我学会了单例模式的构造方法。

### 增加POST解析
在前面的服务器代码中，我们并没有面向请求体的解析。现在的登录功能要求我们接收POST模式发来的报文中含有的账号密码信息，因此我们需要在ClientState结构体中增加一个post_data的string来存储客户端发来的内容，并且新增POST的解析函数。由于发来的POST内容形式固定形如user=admin&password=123456，可以轻松得到解析方法如下：
'''
bool parse_post_data(ClientState& client,std::string& username,std::string& password){
    size_t user_pos = client.post_data.find("user=");
    size_t pwd_pos = client.post_data.find("&password:");
    if(user_pos == std::string::npos || pwd_pos == std::string::npos){
        return false;
    }
    username = client.post_data.substr(user_pos + 5,pwd_pos - (user_pos + 5));
    password = client.post_data.substr(pwd_pos + 10);
    return true;
}
'''

### 数据库连接校验与动态响应
解析出POST报文中含有的账号密码后，我们需要将其传入数据库进行比对，决定能否登录，并且对不同的结果做出动态响应，而不是无论什么情况都发送一个静态的文件。
'''
bool check_login(const std::string& user, const std::string& password){
    MySQLPool* pool = MySQLPool::get_instance();
    MYSQL* conn = pool->get_conn();
    if(conn == NULL){
        return false;
    }
    char sql[1024];
    sprintf(sql,"SELECT * FROM user WHERE username='%s' AND password='%s'",user.c_str(),password.c_str());
    if(mysql_query(conn,sql)){
        pool->return_conn(conn);
        return false; 
    }
    MYSQL_RES* res = mysql_store_result(conn);
    bool success = mysql_num_rows(res) > 0; 
    mysql_free_result(res);
    pool->return_conn(conn);
    return success;
}
'''
在代码中，我们用到大量MySQL的关键API：在将输出的账号密码转化为数据库的查询语句后，调用mysql_query()，通过连接执行传入的SQL语句，传入参数为连接和语句，返回值为0表示成功，非0失败。
再调用储存查询结果集的mysql_store_result，传入连接，传出一个指向结果集结构体的指针res，包含查询到的所有行数据，对其进行mysql_num_rows(res)判断，得到查询结果的行数，查询出的结果行数大于0则说明数据库中有相应的账号密码。

视查询结果不同，我们有两种不同的响应如下：
if(success){
        html = "<html><body><h1>✅ 登录成功！欢迎回来</h1></body></html>";
    }else{
        html = "<html><body><h1>❌ 登录失败！账号不存在或密码错误</h1></body></html>";
    }
将其和响应头一同发送即可！

### 完善主状态机和POST登录逻辑
现在我们终于可以对POST类报文的请求体进行解析了！
在主状态机的case CHECK_STATE_CONTENT中，判断buffer中的剩余空间大于请求体的大小后，将请求体部分存入post_data。
返回我们真正用于处理连接的handle_client函数，确定请求完整的情况下，我们需要增加对POST请求的处理逻辑。在解析请求行的时候，区分开GET类和POST类,确定为POST请求且请求路径为专门的/login页面后，进入处理逻辑：先进行报文解析得到用户名和密码，再在数据库中查询对应词条，根据查询结果发送对应响应。
'''
if(client.method == "POST" && client.url == "/login"){
            std::string user;
            std::string password;
            bool if_parse = parse_post_data(client,user,password);
            bool if_login = check_login(user,password);
            send_login_response(fd,if_login);
        }
'''

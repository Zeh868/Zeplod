> 语言: **中文** | [English](../../en/40-app-development/41-zephyr-app-development.md)

# Zephyr 应用开发与服务指南

本文面向**在 Zephyr 上编写应用代码**的开发者，归纳常用内核能力、设备与配置工作流，以及编写**系统级服务**（后台线程、定时、同步、资源管理）时的注意点。细节以当前 Zephyr 版本官方文档为准；本仓库特有封装见 **[系统服务使用说明.md](../30-核心模块/36-系统服务使用说明.md)**、**[事件系统详细使用说明.md](../30-核心模块/31-事件系统详细使用说明.md)**。

**前置**：[环境搭建与配置指南.md](../10-环境与构建/11-环境搭建与配置指南.md) · [Freestanding应用与构建基础.md](../10-环境与构建/12-Freestanding应用与构建基础.md) · [设备树与内存配置手册.md](44-设备树与内存配置手册.md)（按需）

**内核对象详解与代码示例**：见下 **[§6 Zephyr 内核数据结构（`struct`）与示例](#6-zephyr-内核数据结构struct与示例)**。

---

## 1. 分层与职责

| 层次 | 典型内容 | 应用开发者关注点 |
|------|----------|------------------|
| **应用 / 业务** | `main()`、`app_main`、模块、状态机 | 少阻塞、少在中断里做重活 |
| **本模板服务** | `sys_*`、事件总线、模块管理器 | 与 `CONFIG_*` 对齐，见 **[项目配置项说明.md](42-项目配置项说明.md)** |
| **Zephyr 内核** | 线程、调度、同步、定时器、堆 | 栈大小、优先级、超时 |
| **驱动 / 子系统** | GPIO、UART、I2C、Flash… | `device` 句柄、**`DT`** 节点、错误码 |
| **硬件** | SoC、外设 | 时钟、引脚、DMA、MPU（与 overlay 相关） |

---

## 2. 线程与调度

- **线程**：`k_thread_create()` / `K_THREAD_DEFINE`；每个线程需**独立栈**，大小在 `prj.conf` 中常通过 **`CONFIG_MAIN_STACK_SIZE`**、**`CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE`** 或自定义线程属性配置。
- **优先级**：数值**越小优先级越高**（与部分 RTOS 习惯一致，以 Zephyr 文档为准）；避免大量同优先级线程饥饿，谨慎使用**忙等**。
- **协作式 vs 抢占式**：取决于 `CONFIG_NUM_COOP_PRIORITIES` / `CONFIG_NUM_PREEMPT_PRIORITIES` 等；不了解时不要混用假设。
- **系统工作队列**：`k_work` 提交到 **`system workqueue`**，适合**非实时**、可延迟执行的短任务；别在 work 里长时间持锁或阻塞。

**实践**：长耗时任务单独线程；与硬件紧耦合的用高优先级线程或中断+队列。

---

## 3. 同步与通信（常用）

| 机制 | 典型用途 |
|------|----------|
| **`k_mutex`** | 保护共享可变状态（同优先级线程间）；持锁时间要短。 |
| **`k_sem`** | 计数、信号、线程间唤醒；注意初始 count 与上限。 |
| **`k_msgq` / `k_fifo` / `k_pipe`** | 结构化数据流、生产者–消费者。 |
| **`k_poll`** | 多事件等待（socket、fd、内核对象等场景）。 |

**注意**：**ISR** 中只能使用 **ISR 安全** API（如 `k_sem_give` 部分场景）；复杂逻辑放到线程里处理。

---

## 4. 时间与定时器

- **`k_sleep()` / `k_msleep()`**：线程让出 CPU；精度受 tick 配置影响（**`CONFIG_SYS_CLOCK_TICKS_PER_SEC`**、tickless 等）。
- **`k_timer`**：基于内核定时器的周期性/一次性回调；回调上下文规则见文档。
- **超时**：大量阻塞 API 接受 **`k_timeout_t`**（如 `K_MSEC(n)`、`K_FOREVER`），避免死锁要设计超时路径。

本模板 **`sys_timer`** 在 Zephyr 定时能力之上做封装，见 **[系统服务使用说明.md](../30-核心模块/36-系统服务使用说明.md)**。

---

## 5. 内存

- **`k_malloc()` / `k_free()`**：使用内核堆，大小由 **`CONFIG_HEAP_MEM_POOL_SIZE`** 等决定；与 **SRAM** 总预算相关，见 **[设备树与内存配置手册.md](44-设备树与内存配置手册.md)**。
- **`k_heap`**：可在静态缓冲或指定内存区上建独立堆（多段 RAM 场景）。
- **栈溢出**：开启 **`CONFIG_THREAD_STACK_INFO`**、`CONFIG_THREAD_NAME` 等便于调试；合理设置各线程栈。

本模板 **`sys_memory`** 提供多池与统计，见 **[系统服务使用说明.md](../30-核心模块/36-系统服务使用说明.md)**。

---

## 6. Zephyr 内核数据结构（`struct`）与示例

以下列出应用层最常用的**内核对象类型**（均为 **`struct`**，通过静态分配或宏定义获得实例）。API 名称以 Zephyr 3.x/4.x 为主线；若你使用的版本签名不同，请以 **`include/zephyr/kernel.h`** 与官方文档为准。

### 6.1 对象速查表

| 内核类型 | 典型用途 |
|----------|----------|
| **`struct k_thread`** | 线程控制块；与栈一起由 `k_thread_create` / `K_THREAD_DEFINE` 使用 |
| **`struct k_mutex`** | 互斥锁，保护共享数据 |
| **`struct k_sem`** | 计数信号量、同步、ISR→线程通知 |
| **`struct k_msgq`** | 定长消息队列（拷贝整型或小型结构体） |
| **`struct k_fifo` / `struct k_lifo`** | 指针队列（节点需含链表域，常用于大块缓冲传递） |
| **`struct k_pipe`** | 字节流管道 |
| **`struct k_timer`** | 基于系统时钟的一次/周期定时，到期回调 |
| **`struct k_work` / `struct k_work_delayable`** | 提交到工作队列执行，避免在 ISR 里做重活 |
| **`struct k_poll_event`** | `k_poll` 多路等待 |
| **`struct k_heap`** | 在指定内存区上的堆（可与 `k_malloc` 所在内核堆区分） |
| **`struct k_mem_slab`** | 定长对象池，分配/释放可预测、碎片少 |
| **`struct ring_buf`** | 环形字节缓冲（常用于 UART/流式数据，见 `zephyr/sys/ring_buffer.h`） |
| **`struct k_spinlock`** | 多核/低延迟临界区（SMP 或 ISR 与线程间极短互斥，用法见官方 Spinlocks） |

---

### 6.2 线程 `struct k_thread` 与栈

线程必须绑定**已分配栈**。常用两种写法：

**方式 A：宏一次性定义线程 + 栈 + 入口**

```c
#include <zephyr/kernel.h>

#define MY_STACK_SIZE 1024
#define MY_PRIORITY   5

static void my_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		/* 线程体 */
		k_msleep(100);
	}
}

K_THREAD_DEFINE(my_tid, MY_STACK_SIZE, my_thread_entry,
		NULL, NULL, NULL, MY_PRIORITY, 0, 0);
```

**方式 B：显式 `struct k_thread` + `K_THREAD_STACK_DEFINE`**

```c
static struct k_thread my_thread_data;
K_THREAD_STACK_DEFINE(my_stack, MY_STACK_SIZE);

static void start_my_thread(void)
{
	k_thread_create(&my_thread_data, my_stack,
			K_THREAD_STACK_SIZEOF(my_stack),
			my_thread_entry, NULL, NULL, NULL,
			MY_PRIORITY, 0, K_NO_WAIT);
}
```

---

### 6.3 互斥锁 `struct k_mutex`

```c
static struct k_mutex g_mtx;

static void mutex_demo(void)
{
	k_mutex_init(&g_mtx);

	k_mutex_lock(&g_mtx, K_FOREVER);
	/* 临界区：访问共享变量 */
	k_mutex_unlock(&g_mtx);

	/* 带超时，避免死锁 */
	if (k_mutex_lock(&g_mtx, K_MSEC(10)) == 0) {
		/* ... */
		k_mutex_unlock(&g_mtx);
	}
}
```

---

### 6.4 信号量 `struct k_sem`

```c
static K_SEM_DEFINE(my_sem, 0, 1); /* 初值 0，最大 1 —— 二值信号量 */

static void sem_thread(void)
{
	k_sem_take(&my_sem, K_FOREVER); /* 等待 ISR 或其它线程 give */
}

/* ISR 中通知线程（示例） */
static void my_isr(const void *arg)
{
	ARG_UNUSED(arg);
	k_sem_give(&my_sem);
}
```

---

### 6.5 消息队列 `struct k_msgq`

适合传递**固定长度**小消息（如 `uint32_t`、小型 `struct`）：

```c
#define MSG_WORDS 4
#define MSG_COUNT 8

K_MSGQ_DEFINE(my_msgq, sizeof(uint32_t), MSG_COUNT, 4);

static void msgq_producer(void)
{
	uint32_t v = 0xAA;

	if (k_msgq_put(&my_msgq, &v, K_MSEC(10)) != 0) {
		/* 队列满 */
	}
}

static void msgq_consumer(void)
{
	uint32_t out;

	if (k_msgq_get(&my_msgq, &out, K_FOREVER) == 0) {
		/* 使用 out */
	}
}
```

---

### 6.6 FIFO `struct k_fifo`（传递指针）

队列元素必须是**含链表节点字段**的结构体，且通常由 **slab / 堆** 分配节点：

```c
struct msg_node {
	void *fifo_reserved;   /* 内核使用，须为首成员 */
	int payload;
};

static struct k_fifo g_fifo;

static void fifo_init(void)
{
	k_fifo_init(&g_fifo);
}

static void fifo_send(int data)
{
	struct msg_node *n = k_malloc(sizeof(*n));

	if (n == NULL) {
		return;
	}
	n->payload = data;
	k_fifo_put(&g_fifo, n);
}

static void fifo_recv_thread(void)
{
	struct msg_node *n;

	for (;;) {
		n = k_fifo_get(&g_fifo, K_FOREVER);
		/* 处理 n->payload */
		k_free(n);
	}
}
```

---

### 6.7 定时器 `struct k_timer`

```c
static void on_timer_expiry(struct k_timer *t);

K_TIMER_DEFINE(my_timer, on_timer_expiry, NULL);

static void on_timer_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	/* 注意：回调上下文规则见官方文档；复杂逻辑可转 k_work */
}

static void timer_start_demo(void)
{
	/* 100ms 后首次触发，之后每 500ms 周期触发 */
	k_timer_start(&my_timer, K_MSEC(100), K_MSEC(500));
}
```

---

### 6.8 工作队列 `struct k_work` / `struct k_work_delayable`

把耗时或需线程上下文的逻辑从 **ISR / 定时器回调** 挪到 **system workqueue**：

```c
static void my_work_handler(struct k_work *w);

K_WORK_DEFINE(my_work, my_work_handler);

static void my_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	/* 在线程上下文执行 */
}

static void trigger_from_isr_or_thread(void)
{
	k_work_submit(&my_work);
}
```

**延迟执行**（防抖、合并触发）可用 **`k_work_delayable`**：先 **`k_work_init_delayable`**，再 **`k_work_schedule`** / **`k_work_reschedule`**（具体 API 以当前版本 `kernel.h` 为准）。

---

### 6.9 独立堆 `struct k_heap`

在静态数组或某段 SRAM 上建堆，与全局 **`k_malloc`** 隔离：

```c
static struct k_heap app_heap;
static uint8_t heap_mem[4096] __aligned(8);

static void app_heap_init(void)
{
	k_heap_init(&app_heap, heap_mem, sizeof(heap_mem));
}

static void *app_alloc(size_t n)
{
	return k_heap_alloc(&app_heap, n, K_NO_WAIT);
}

static void app_free(void *p)
{
	k_heap_free(&app_heap, p);
}
```

---

### 6.10 定长内存池 `struct k_mem_slab`

比通用堆更利于**固定块大小、低碎片**场景（如网络包描述符、传感器帧）：

```c
#define BLK_SZ   64
#define BLK_NUM  8

K_MEM_SLAB_DEFINE(my_slab, BLK_SZ, BLK_NUM, 4);

static void slab_demo(void)
{
	void *blk;

	if (k_mem_slab_alloc(&my_slab, &blk, K_FOREVER) == 0) {
		/* 使用 (uint8_t *)blk 指向的 BLK_SZ 字节 */
		k_mem_slab_free(&my_slab, blk);
	}
}
```

---

### 6.11 `k_poll`（多事件等待，简例）

用于同时等待多个内核对象就绪（如信号量 + FIFO），适合网络/多路 IO；**完整用法**（多事件、`K_POLL_STATE_*` 判断）见 **[Kernel — Polling](https://docs.zephyrproject.org/latest/kernel/services/polling.html)**。下例仅演示**单信号量**等待，且假设已存在 **`static K_SEM_DEFINE(my_sem, ...)`**。

```c
struct k_poll_event events[1];

k_poll_event_init(&events[0], K_POLL_TYPE_SEM_AVAILABLE,
		  K_POLL_MODE_NOTIFY_ONLY, &my_sem);

/* 等待就绪，超时 100ms；返回 0 表示未超时，再检查 events[i].state */
k_poll(events, ARRAY_SIZE(events), K_MSEC(100));
```

（若编译器无 **`ARRAY_SIZE`**，可写 **`1`** 或包含 **`<zephyr/sys/util.h>`**。）

---

## 7. 日志（Zephyr `LOG`）

- 在模块内 **`LOG_MODULE_REGISTER(name)`**，使用 **`LOG_INF` / `LOG_ERR`** 等；级别由 **`CONFIG_LOG`** 及模块 **`LOG_LEVEL`** 控制。
- 默认输出依赖 **UART/console** 与 **`prj.conf`** 中的 **`CONFIG_LOG`**、**`CONFIG_CONSOLE`** 等。
- 高吞吐场景注意**栈**与**缓冲区**；量产可关闭调试级别以减 Flash/RAM。

与模板 **`sys_log`** 的关系：可在业务层统一封装，便于内存环缓与模块名字段，二者可并存，注意**不要重复刷屏**。

---

## 8. Shell（可选）

启用 **`CONFIG_SHELL`** 及相关子命令后，可在串口使用交互命令做调试。会占用 **Flash/RAM** 与**一个会话上下文**；发布镜像常关闭或裁剪命令集。

---

## 9. 设备与驱动（应用侧用法）

1. **Devicetree** 中节点带 **`status = "okay"`** 且驱动已启用时，在 C 代码中用 **`DEVICE_DT_GET()`** / **`DEVICE_DT_GET_ONE()`** 等获取 **`const struct device *`**。
2. 使用前 **`device_is_ready(dev)`**。
3. 调用 **`gpio_pin_configure`**、**`i2c_transfer`** 等 API 时传入 **`dev`** 与 **DT 宏**指定的引脚/通道（**`GPIO_DT_SPEC_GET`** 等）。
4. 错误码多为 **负 errno**（`-EIO`、`-ETIMEDOUT` 等），需向上传递或记录日志。

### 9.1 示例：GPIO（`gpio_dt_spec`）

下列为常见写法：假定板级 DTS 已为 LED 提供 **`led0`** 别名（许多 Nucleo 如此；若无则需改用你板上的节点标签或 **`DT_NODELABEL`**）。

```c
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

static int board_led_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	return ret;
}
```

**`gpio_pin_configure_dt`** 同时完成了 **设备与引脚** 的绑定；若用手动 **`DEVICE_DT_GET(DT_PARENT(...))`** 等方式，则需自行传入 **`dev`** 与 **`pin`**。应用级节点与 **`app.overlay`** 见 **[设备树与内存配置手册.md](44-设备树与内存配置手册.md)**。

**设备树**：应用级修改优先 **`app.overlay`**，见 **[设备树与内存配置手册.md](44-设备树与内存配置手册.md)**。

---

## 10. Kconfig 与 `prj.conf` 工作流

- **`prj.conf`**：写 **`CONFIG_*=y`** 或赋值；合并进最终 **`.config**。
- **`Kconfig`**（应用根目录等）：定义本应用可见的菜单项；通过 **`rsource`** 拆分到子目录。
- 修改后若行为异常：**`west build -p always`** 或 **`pristine`**；复杂项用 **`menuconfig` / `guiconfig`**。

本应用扩展项释义见 **[项目配置项说明.md](42-项目配置项说明.md)**。

---

## 11. 中断与 ISR

- ISR 要**极短**：清标志、拷贝数据到缓冲区、**释放信号量/提交 work** 等。
- 若 SoC 支持 **优先级嵌套**，注意与临界区、**`irq_lock`** 的配合。
- **Never** 在 ISR 里调用可能阻塞或分配堆的 API（除非文档明确 ISR-safe）。

**ISR 安全的 API 示例：**
| API | ISR 安全条件 | 原因 |
|-----|-------------|------|
| `k_msgq_put(K_NO_WAIT)` | ✅ 安全 | 内部使用 spinlock |
| `k_sem_give()` | ✅ 安全 | 不阻塞 |
| `k_mutex_lock()` | ❌ 禁止 | 可能阻塞/调度 |
| `k_malloc()` | ⚠️ 避免 | 可能不可重入 |

> 技术细节：Spinlock 只禁用中断不触发调度，Mutex 可能阻塞触发调度。详见 **[框架核心技术实现细节.md](../20-架构设计/23-框架核心技术实现细节.md#24-isr-安全发布)**。

---

## 12. 编写「服务」时的通用模式

「服务」在此指长期运行、对外提供 API 的后台能力（类似本模板的 **`sys_*`**、IPC 工作线程）：

1. **明确初始化阶段**：优先 **`SYS_INIT`**（本模板中 **`app_main.c`** 与各 **`example_module_*.c`** 已采用，优先级见 **`app_config.h`** 的 **`APP_INIT_PRIO_*`**）；返回错误要可恢复或记录。
2. **资源归属**：堆、静态缓冲、句柄的生命周期清晰；停止路径要**释放/注销**（若适用）。
3. **线程模型**：单线程串行处理 vs 多线程；若多线程共享状态，**统一用 mutex 或消息队列**，避免裸全局无锁。
4. **与事件系统协作**：慢业务走 **发布–订阅**，不要在回调里阻塞 IPC/网络。
5. **可配置性**：通过 **`Kconfig`** 暴露栈大小、队列深度、超时，便于不同板型裁剪。

---

## 13. 与本仓库文档的衔接

| 主题 | 文档 |
|------|------|
| 本模板 `sys_*` | [系统服务使用说明.md](../30-核心模块/36-系统服务使用说明.md) |
| 事件与模块 | [事件系统详细使用说明.md](../30-核心模块/31-事件系统详细使用说明.md) · [模块系统详细使用说明.md](../30-核心模块/32-模块系统详细使用说明.md) |
| Thread IPC | [线程IPC服务使用说明.md](../30-核心模块/33-线程IPC服务使用说明.md) |
| 内存布局 / overlay | [设备树与内存配置手册.md](44-设备树与内存配置手册.md) |
| OTA / 存储 / PM | [OTA与存储扩展指南.md](../70-发布与产品化/74-OTA与存储扩展指南.md) |

---

## 14. 推荐阅读（官方）

- [Kernel Services](https://docs.zephyrproject.org/latest/kernel/services/index.html)
- [Device Driver Model](https://docs.zephyrproject.org/latest/kernel/drivers/index.html)
- [Build and Configuration Systems](https://docs.zephyrproject.org/latest/build/index.html)
- [Application Development](https://docs.zephyrproject.org/latest/application/index.html)

---

*本文随 Zephyr 大版本演进；若 API 与官方不一致，以官方为准。*

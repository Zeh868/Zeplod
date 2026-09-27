/**
 * @file test_event_system.c
 * @brief 事件系统单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            修复 ZTEST_SUITE 参数错位；rt 系列改为直接断言；统计用例补哨兵断言；裸调用返回值检查
 * 2026-09-27       1.2            zeh            unregister_type 用例对齐注销后换名重注册契约；新增正向用例
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include "event_memory.h"
#include "event_queue.h"
#include "event_system_internal.h"
#include "test_event_stubs.h"
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_event_system);

#define STOP_FROM_CALLBACK_EVENT_TYPE 240U

static atomic_t       g_stop_from_callback_seen;
static event_status_t g_stop_from_callback_status;

static void stop_from_callback_handler(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);

    g_stop_from_callback_status = event_system_stop();
    atomic_set(&g_stop_from_callback_seen, 1);
}

/* =============================================================================
 * 测试用例
 * ============================================================================= */

/**
 * @brief 测试事件系统初始化
 */
ZTEST(test_event_system, test_event_system_init) {
    event_status_t status;

    /* 测试正常初始化 */
    status = event_system_init();
    zassert_equal(status, EVENT_OK, "事件系统初始化失败");

    /* 测试重复初始化（应返回 OK）*/
    status = event_system_init();
    zassert_equal(status, EVENT_OK, "重复初始化应返回 OK");
}

/**
 * @brief 测试事件类型注册
 */
ZTEST(test_event_system, test_event_register_type) {
    event_status_t status;

    /* 先初始化 */
    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 测试注册有效类型 */
    status = event_register_type(10, "test_event");
    zassert_equal(status, EVENT_OK, "事件类型注册失败");

    /* 测试重复注册（应返回 OK）*/
    status = event_register_type(10, "test_event");
    zassert_equal(status, EVENT_OK, "重复注册应返回 OK");

    /* 同一 ID 不允许换名注册，避免不同载荷契约静默冲突 */
    status = event_register_type(10, "different_event");
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "同一 ID 使用不同名称应被拒绝");

    /* 测试类型边界：255 在 CONFIG_EVENT_MAX_TYPES=256 内属有效 ID（历史遗留的"无效类型"注释有误） */
    status = event_register_type(255, "invalid");
    zassert_equal(status, EVENT_OK, "255 应是有效类型");
}

/**
 * @brief 测试事件订阅
 */
ZTEST(test_event_system, test_event_subscribe) {
    event_status_t status;
    uint32_t       subscriber_id;

    /* 先初始化和注册 */
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(20, "subscribe_test"), EVENT_OK, NULL);

    /* 测试正常订阅 */
    status = event_subscribe(20, NULL, NULL, &subscriber_id);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "空回调应返回错误");

    /* 测试有效订阅 */
    status = event_subscribe(20, test_event_noop_callback, NULL, &subscriber_id);
    zassert_equal(status, EVENT_OK, "订阅失败");
    zassert_true(subscriber_id > 0, "订阅 ID 应大于 0");

    /* 测试取消订阅 */
    status = event_unsubscribe(20, subscriber_id);
    zassert_equal(status, EVENT_OK, "取消订阅失败");
}

/**
 * @brief 测试事件创建和释放
 */
ZTEST(test_event_system, test_event_create_free) {
    event_t* event;

    /* 先初始化 */
    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 测试创建事件 */
    event = event_create(30, EVENT_PRIORITY_NORMAL);
    zassert_not_null(event, "事件创建失败");
    zassert_equal(event->type, 30, "事件类型不匹配");
    zassert_equal(event->priority, EVENT_PRIORITY_NORMAL, "事件优先级不匹配");

    /* 测试释放事件 */
    event_free(event);
    /* 不应崩溃 */
}

/**
 * @brief 测试事件统计
 */
ZTEST(test_event_system, test_event_statistics) {
    uint32_t total_events, queue_depth, dropped_events;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 先清零统计：dropped 计数是跨套件存续的全局量，须显式复位后断言才有确定性 */
    event_system_reset_statistics();

    /* 哨兵初始化：验证接口确实写出了全部输出参数而非残留调用方栈值 */
    total_events = 0xDEADBEEFU;
    queue_depth = 0xDEADBEEFU;
    dropped_events = 0xDEADBEEFU;

    event_get_statistics(&total_events, &queue_depth, &dropped_events);

    /* 刚 init 的系统：已分发计数为 0、队列为空、丢弃计数已被复位 */
    zassert_equal(total_events, 0U, "初始 total_events 应为 0");
    zassert_equal(queue_depth, 0U, "初始 queue_depth 应为 0");
    zassert_equal(dropped_events, 0U, "复位后 dropped_events 应为 0");
}

/**
 * @brief 测试事件发布（无订阅者）
 */
ZTEST(test_event_system, test_event_publish_no_subscriber) {
    event_status_t status;
    event_t        event = {0};

    event.type = 40;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.data_len = 0;

    /* 先初始化和启动 */
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    status = event_publish(&event);
    zassert_equal(status, EVENT_ERR_NOT_FOUND, "未注册类型应拒绝发布");

    event_system_stop();
}

/**
 * @brief 测试发布时 flags/data_len 一致性校验
 */
ZTEST(test_event_system, test_event_publish_invalid_payload) {
    event_t        event = {0};
    event_status_t status;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(41, "publish_validate"), EVENT_OK, NULL);

    event.type = 41;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.data_len = CONFIG_EVENT_INLINE_DATA_SIZE + 1U;
    event.flags = EVENT_FLAG_DATA_INLINE;

    status = event_publish(&event);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "INLINE 超长应拒绝");

    event.data_len = 8U;
    event.flags = EVENT_FLAG_DATA_DYNAMIC;
    event.data.ptr = NULL;

    status = event_publish(&event);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "DYNAMIC 空指针应拒绝");

    uint8_t payload = 0x5AU;
    event.data.ptr = &payload;
    event.flags = EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_SLAB_256;

    status = event_publish(&event);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "缺少 DATA_FROM_SLAB 的 slab 标志应拒绝");

    event.flags = EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_DATA_FROM_SLAB | EVENT_FLAG_SLAB_256;

    status = event_publish(&event);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "不属于标记 slab 的指针应拒绝");

    event_system_stop();
}

/**
 * @brief 测试 event_create_with_data
 */
ZTEST(test_event_system, test_event_create_with_data) {
    event_t* event;
    uint32_t test_data = 0x12345678;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 测试正常创建带数据的事件 */
    event = event_create_with_data(50, EVENT_PRIORITY_HIGH, &test_data, sizeof(test_data));
    zassert_not_null(event, "事件创建失败");
    zassert_equal(event->type, 50, "事件类型不匹配");
    zassert_equal(event->priority, EVENT_PRIORITY_HIGH, "事件优先级不匹配");
    zassert_equal(event->data_len, sizeof(test_data), "数据长度不匹配");

    /* 验证数据副本正确 - 小数据使用内联存储 */
    zassert_true((event->flags & EVENT_FLAG_DATA_INLINE) != 0 || (event->flags & EVENT_FLAG_DATA_DYNAMIC) != 0,
                 "数据应标记为内联或动态");

    if (event->flags & EVENT_FLAG_DATA_INLINE) {
        zassert_mem_equal(event->data.inline_data, &test_data, sizeof(test_data), "内联数据不正确");
    } else if (event->flags & EVENT_FLAG_DATA_DYNAMIC) {
        zassert_not_null(event->data.ptr, "动态数据指针不应为 NULL");
        zassert_equal(*(uint32_t*) event->data.ptr, 0x12345678, "动态数据副本不正确");
    }

    event_free(event);

    /* 测试 NULL 数据（应退化为 event_create）*/
    event = event_create_with_data(51, EVENT_PRIORITY_NORMAL, NULL, 0);
    zassert_not_null(event, "事件创建失败");
    zassert_equal(event->data_len, 0, "数据长度应为 0");
    zassert_false((event->flags & EVENT_FLAG_DATA_DYNAMIC) != 0, "NULL 数据时不应标记为动态");

    event_free(event);
}

/**
 * @brief 测试 event_notify_subscribers
 */
ZTEST(test_event_system, test_event_notify_subscribers) {
    event_status_t status;
    uint32_t       subscriber_id;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(60, "notify_test"), EVENT_OK, NULL);

    /* 订阅事件 */
    status = event_subscribe(60, test_event_noop_callback, NULL, &subscriber_id);
    zassert_equal(status, EVENT_OK, "订阅失败");

    /* 创建并发布事件 */
    uint32_t test_data = 999;
    status = event_publish_copy(60, EVENT_PRIORITY_NORMAL, &test_data, sizeof(test_data));
    zassert_equal(status, EVENT_OK, "发布失败");

    /* 验证事件已发布（本套件无分发器线程，事件留在队列）*/
    zassert_true(subscriber_id > 0, "订阅者 ID 应大于 0");

    event_unsubscribe(60, subscriber_id);
    event_system_stop();
}

/**
 * @brief 测试 event_unsubscribe_all
 */
ZTEST(test_event_system, test_event_unsubscribe_all) {
    event_status_t status;
    uint32_t       sub_id1, sub_id2, sub_id3;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(70, "unsubscribe_all_test1"), EVENT_OK, NULL);
    zassert_equal(event_register_type(71, "unsubscribe_all_test2"), EVENT_OK, NULL);
    zassert_equal(event_register_type(72, "unsubscribe_all_test3"), EVENT_OK, NULL);

    /* 同一回调订阅多个事件类型，每次订阅会得到不同的 subscriber_id */
    status = event_subscribe(70, test_event_noop_callback, NULL, &sub_id1);
    zassert_equal(status, EVENT_OK, "订阅 70 失败");

    status = event_subscribe(71, test_event_noop_callback, NULL, &sub_id2);
    zassert_equal(status, EVENT_OK, "订阅 71 失败");

    status = event_subscribe(72, test_event_noop_callback, NULL, &sub_id3);
    zassert_equal(status, EVENT_OK, "订阅 72 失败");

    /* 验证每次订阅都得到不同的 subscriber_id */
    zassert_true(sub_id1 > 0, "订阅者 ID 应大于 0");
    zassert_true(sub_id2 > 0, "订阅者 ID 应大于 0");
    zassert_true(sub_id3 > 0, "订阅者 ID 应大于 0");

    /* event_unsubscribe_all 只能取消一个 subscriber_id 的订阅
     * （因为每次订阅分配不同的 ID，所以需要分别取消）*/
    event_unsubscribe_all(sub_id1);
    event_unsubscribe_all(sub_id2);
    event_unsubscribe_all(sub_id3);

    /* 验证所有订阅都已取消 */
    zassert_equal(event_get_subscriber_count(70), 0, "事件 70 的订阅者应为 0");
    zassert_equal(event_get_subscriber_count(71), 0, "事件 71 的订阅者应为 0");
    zassert_equal(event_get_subscriber_count(72), 0, "事件 72 的订阅者应为 0");
}

/**
 * @brief 测试 event_unregister_type
 */
ZTEST(test_event_system, test_event_unregister_type) {
    event_status_t status;
    uint32_t       subscriber_id;
    const char*    registered_name;
    char           name_snapshot[CONFIG_EVENT_TYPE_NAME_MAX];

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 注册事件类型 */
    status = event_register_type(80, "unregister_test");
    zassert_equal(status, EVENT_OK, "注册失败");
    registered_name = event_get_type_name(80);

    /* 返回的是内部名称缓冲指针，按头文件指引立即拷贝后再断言 */
    zassert_not_null(registered_name, NULL);
    (void) strncpy(name_snapshot, registered_name, sizeof(name_snapshot) - 1U);
    name_snapshot[sizeof(name_snapshot) - 1U] = '\0';
    zassert_str_equal(name_snapshot, "unregister_test", "注册后类型名应正确");

    /* 订阅事件 */
    status = event_subscribe(80, test_event_noop_callback, NULL, &subscriber_id);
    zassert_equal(status, EVENT_OK, "订阅失败");

    /* 尝试注销有订阅者的类型（应失败）*/
    status = event_unregister_type(80);
    zassert_true(status != EVENT_OK, "有订阅者时应注销失败");

    /* 取消订阅后再注销 */
    status = event_unsubscribe(80, subscriber_id);
    zassert_equal(status, EVENT_OK, "取消订阅失败");

    status = event_unregister_type(80);
    zassert_equal(status, EVENT_OK, "注销失败");

    /* 验证类型已注销 */
    zassert_equal(event_get_subscriber_count(80), 0, "注销后订阅者数应为 0");
    zassert_str_equal(event_get_type_name(80), "UNREGISTERED", "注销后查询应返回 UNREGISTERED");

    /* 注销后槽位完全释放：换名重注册应成功 */
    status = event_register_type(80, "renamed_type");
    zassert_equal(status, EVENT_OK, "注销后换名重注册应成功");

    /* 重注册占位后，同 ID 再以不同名注册应被拒绝 */
    status = event_register_type(80, "unregister_test");
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "重注册占位后同 ID 换名应被拒绝");
}

/**
 * @brief 注销 → 换名重注册成功（固化注销完全释放槽位契约，防复发）
 */
ZTEST(test_event_system, test_event_unregister_then_renamed_register) {
    event_status_t status;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(81, "orig_name"), EVENT_OK, NULL);

    /* 注销后槽位完全释放，任意名可重注册 */
    zassert_equal(event_unregister_type(81), EVENT_OK, NULL);
    status = event_register_type(81, "new_name");
    zassert_equal(status, EVENT_OK, "注销后换名重注册应成功");
    zassert_str_equal(event_get_type_name(81), "new_name", "重注册后类型名应更新");

    /* 重注册占位后，原名（视为不同名）应被拒绝 */
    status = event_register_type(81, "orig_name");
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "重注册占位后换名应被拒绝");
}

/**
 * @brief 测试 event_system_get_queue
 */
ZTEST(test_event_system, test_event_system_get_queue) {
    struct k_msgq* queue;

    /* 初始化后应返回有效指针 */
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    queue = event_system_get_queue();
    zassert_not_null(queue, "初始化后队列应为非 NULL");
}

/**
 * @brief 测试 event_system_shutdown
 */
ZTEST(test_event_system, test_event_system_shutdown) {
    event_status_t status;
    uint32_t       subscriber_id;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(90, "shutdown_test"), EVENT_OK, NULL);
    zassert_equal(event_subscribe(90, test_event_noop_callback, NULL, &subscriber_id), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    /* 关闭系统 */
    status = event_system_shutdown();
    zassert_equal(status, EVENT_OK, "关闭失败");

    /* 关闭后应为未初始化状态 */
    zassert_false(event_system_is_running(), "关闭后不应运行");

    /* 重新初始化应成功 */
    status = event_system_init();
    zassert_equal(status, EVENT_OK, "重新初始化失败");
}

/**
 * @brief 测试 event_get_type_name
 */
ZTEST(test_event_system, test_event_get_type_name) {
    const char* name;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 注册类型 */
    zassert_equal(event_register_type(100, "test_type_name"), EVENT_OK, NULL);

    /* 获取已注册类型名称 */
    name = event_get_type_name(100);
    zassert_str_equal(name, "test_type_name", "类型名称不匹配");

    /* 获取未注册类型名称 */
    name = event_get_type_name(101);
    zassert_str_equal(name, "UNREGISTERED", "未注册类型应返回 UNREGISTERED");

    /* 获取无效类型 ID */
    name = event_get_type_name(255);
    zassert_str_equal(name, "UNREGISTERED", "无效类型应返回 UNREGISTERED");
}

/**
 * @brief 测试订阅者上限
 */
ZTEST(test_event_system, test_event_subscribe_max_subscribers) {
    event_status_t status;
    uint32_t       subscriber_ids[CONFIG_EVENT_MAX_SUBSCRIBERS + 1];

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(110, "max_sub_test"), EVENT_OK, NULL);

    /* 订阅直到达到上限 */
    for (int i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
        status = event_subscribe(110, test_event_noop_callback, NULL, &subscriber_ids[i]);
        zassert_equal(status, EVENT_OK, "订阅 %d 失败", i);
    }

    /* 超过上限应失败 */
    status = event_subscribe(110, test_event_noop_callback, NULL, &subscriber_ids[CONFIG_EVENT_MAX_SUBSCRIBERS]);
    zassert_equal(status, EVENT_ERR_QUEUE_FULL, "超过订阅上限应返回 QUEUE_FULL");

    /* 清理 */
    for (int i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
        event_unsubscribe(110, subscriber_ids[i]);
    }
}

/**
 * @brief 测试 event_create_rt 实时安全创建
 *
 * 测试配置（tests/prj.conf）仅启用 NORMAL 池（4 块），HIGH/CRITICAL 无独立池时
 * 实现回退到 NORMAL 池，四种优先级均应成功分配（逐个创建即释放，不会占满池）。
 */
ZTEST(test_event_system, test_event_create_rt) {
    event_t* event;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 测试创建 CRITICAL 优先级事件 */
    event = event_create_rt(120, EVENT_PRIORITY_CRITICAL);
    zassert_not_null(event, "CRITICAL（回退 NORMAL 池）应创建成功");
    zassert_equal(event->type, 120, "事件类型不匹配");
    zassert_equal(event->priority, EVENT_PRIORITY_CRITICAL, "优先级不匹配");
    zassert_true((event->flags & EVENT_FLAG_FROM_SLAB) != 0, "应标记为来自 Slab");
    event_free(event);

    /* 测试创建 HIGH 优先级事件 */
    event = event_create_rt(121, EVENT_PRIORITY_HIGH);
    zassert_not_null(event, "HIGH（回退 NORMAL 池）应创建成功");
    zassert_equal(event->type, 121, "事件类型不匹配");
    event_free(event);

    /* 测试创建 NORMAL 优先级事件 */
    event = event_create_rt(122, EVENT_PRIORITY_NORMAL);
    zassert_not_null(event, "NORMAL 应创建成功");
    zassert_equal(event->type, 122, "事件类型不匹配");
    event_free(event);

    /* 测试创建 LOW 优先级事件 */
    event = event_create_rt(123, EVENT_PRIORITY_LOW);
    zassert_not_null(event, "LOW（共用 NORMAL 池）应创建成功");
    zassert_equal(event->type, 123, "事件类型不匹配");
    event_free(event);
}

#if EVENT_SLAB_ENABLED
/**
 * @brief 释放事件时应按实际地址归还 Slab，不受可变优先级影响
 */
ZTEST(test_event_system, test_event_free_uses_actual_slab_owner) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);

    struct k_mem_slab* slab = event_memory_select_event_slab(EVENT_PRIORITY_NORMAL);
    uint32_t           free_before = k_mem_slab_num_free_get(slab);
    event_t*           event = event_create_rt(124, EVENT_PRIORITY_NORMAL);

    zassert_not_null(event, NULL);
    zassert_equal(k_mem_slab_num_free_get(slab), free_before - 1U, NULL);

    event->priority = EVENT_PRIORITY_CRITICAL;
    event_free(event);

    zassert_equal(k_mem_slab_num_free_get(slab), free_before, "事件应归还到实际分配池");
}
#endif

/**
 * @brief 测试 event_create_with_data_rt 实时安全带数据创建
 *
 * 测试配置（tests/prj.conf）：EVENT_STRUCT_SIZE_64 → 内联负载 48 字节；
 * 大负载 slab 池（LARGE）未启用 → 超过内联上限的 RT 创建按契约返回 NULL。
 */
ZTEST(test_event_system, test_event_create_with_data_rt) {
    event_t* event;
    uint8_t  small_data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t  large_data[256];

    for (int i = 0; i < 256; i++) {
        large_data[i] = (uint8_t) i;
    }

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 测试 NULL 数据（应退化为 event_create_rt）*/
    event = event_create_with_data_rt(130, EVENT_PRIORITY_NORMAL, NULL, 0);
    zassert_not_null(event, "NULL 数据应退化为 event_create_rt 成功");
    zassert_equal(event->data_len, 0, "数据长度应为 0");
    event_free(event);

    /* 测试小数据（16 字节 < 内联上限 48，走内联存储）*/
    event = event_create_with_data_rt(131, EVENT_PRIORITY_NORMAL, small_data, sizeof(small_data));
    zassert_not_null(event, "内联大小的 RT 创建应成功");
    zassert_equal(event->data_len, sizeof(small_data), "数据长度不匹配");
    zassert_true((event->flags & EVENT_FLAG_DATA_INLINE) != 0, "小数据应内联存储");
    zassert_mem_equal(event->data.inline_data, small_data, sizeof(small_data), "数据内容不匹配");
    event_free(event);

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
    /* 测试大数据（需要 LARGE Slab）*/
    event = event_create_with_data_rt(132, EVENT_PRIORITY_NORMAL, large_data, sizeof(large_data));
    zassert_not_null(event, "已配置 LARGE Slab 时大数据 RT 创建应成功");
    zassert_equal(event->data_len, sizeof(large_data), "大数据长度不匹配");
    event_free(event);
#else
    /* 大负载 slab 未配置：RT 路径不允许堆回退，按契约应释放事件壳并返回 NULL */
    event = event_create_with_data_rt(132, EVENT_PRIORITY_NORMAL, large_data, sizeof(large_data));
    zassert_is_null(event, "未配置 LARGE Slab 时大数据 RT 创建应返回 NULL");
#endif
}

/**
 * @brief 测试 event_publish_copy_rt 实时安全发布
 *
 * 测试配置下 NORMAL 池（4 块）空闲、队列为空，发布 4 字节内联负载应确定性成功。
 */
ZTEST(test_event_system, test_event_publish_copy_rt) {
    event_status_t status;
    uint32_t       test_data = 0xDEADBEEF;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(140, "publish_rt_test"), EVENT_OK, NULL);

    status = event_publish_copy_rt(140, EVENT_PRIORITY_NORMAL, &test_data, sizeof(test_data));
    zassert_equal(status, EVENT_OK, "池空闲且队列未满时 RT 发布应成功");

    event_system_stop();
}

/**
 * @brief 测试发布到已停止系统
 */
ZTEST(test_event_system, test_event_publish_when_stopped) {
    event_status_t status;
    event_t        event = {.type = 150, .priority = EVENT_PRIORITY_NORMAL, .data_len = 0};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    /* 不启动系统 */

    /* 发布应被拒绝（未 start 时返回 NOT_RUNNING） */
    status = event_publish(&event);
    zassert_equal(status, EVENT_ERR_NOT_RUNNING, "停止状态下发布应被拒绝");

    /* event_publish_copy 也应被拒绝 */
    status = event_publish_copy(150, EVENT_PRIORITY_NORMAL, "test", 4);
    zassert_equal(status, EVENT_ERR_NOT_RUNNING, "停止状态下 publish_copy 应被拒绝");
}

/**
 * @brief 发布失败路径后生命周期仍可正常 stop/shutdown
 */
ZTEST(test_event_system, test_event_publish_failure_lifecycle_ok) {
    event_status_t status;
    event_t        bad = {.type = 151,
                          .priority = EVENT_PRIORITY_NORMAL,
                          .data_len = 1U,
                          .flags = EVENT_FLAG_DATA_INLINE | EVENT_FLAG_DATA_DYNAMIC,
                          .data = {.inline_data = {0xAA}}};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(151, "publish_fail"), EVENT_OK, NULL);

    status = event_publish(&bad);
    zassert_equal(status, EVENT_ERR_NOT_RUNNING, NULL);

    zassert_equal(event_system_start(), EVENT_OK, NULL);
    status = event_publish(&bad);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, NULL);

    {
        event_t unreg = {.type = 152, .priority = EVENT_PRIORITY_NORMAL, .data_len = 0U};

        status = event_publish(&unreg);
        zassert_equal(status, EVENT_ERR_NOT_FOUND, NULL);
    }

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

/**
 * @brief 测试事件系统多次 shutdown
 */
ZTEST(test_event_system, test_event_system_shutdown_multiple) {
    event_status_t status;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 第一次关闭 */
    status = event_system_shutdown();
    zassert_equal(status, EVENT_OK, "第一次关闭失败");

    /* 第二次关闭应幂等 */
    status = event_system_shutdown();
    zassert_equal(status, EVENT_OK, "重复关闭应返回 OK");
}

/**
 * @brief 测试订阅未注册类型
 */
ZTEST(test_event_system, test_event_subscribe_unregistered_type) {
    event_status_t status;
    uint32_t       subscriber_id;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    /* 不注册类型 160 */

    status = event_subscribe(160, test_event_noop_callback, NULL, &subscriber_id);
    zassert_equal(status, EVENT_ERR_NOT_FOUND, "订阅未注册类型应返回 NOT_FOUND");
}

/**
 * @brief 测试 event_system_start/stop 多次调用
 */
ZTEST(test_event_system, test_event_system_lifecycle_start_stop_cycle) {
    event_status_t status;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_false(event_system_is_running(), "init 后不应运行");

    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_true(event_system_is_running(), "start 后应运行");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_false(event_system_is_running(), "stop 后不应运行");

    /* STOPPED 后应允许再次 start（状态机 STOPPED -> STARTING -> RUNNING） */
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_true(event_system_is_running(), "二次 start 后应运行");

    status = event_system_stop();
    zassert_equal(status, EVENT_OK, NULL);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
    zassert_false(event_system_is_running(), "shutdown 后不应运行");
}

ZTEST(test_event_system, test_event_system_start_stop_multiple) {
    event_status_t status;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 多次启动应幂等 */
    status = event_system_start();
    zassert_equal(status, EVENT_OK, "第一次启动失败");

    status = event_system_start();
    zassert_equal(status, EVENT_OK, "重复启动应返回 OK");

    /* 多次停止应幂等 */
    status = event_system_stop();
    zassert_equal(status, EVENT_OK, "第一次停止失败");

    status = event_system_stop();
    zassert_equal(status, EVENT_OK, "重复停止应返回 OK");
}

ZTEST(test_event_system, test_subscriber_id_wrap_preserves_uniqueness) {
    uint32_t first_id;
    uint32_t second_id;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(161, "subscriber_wrap"), EVENT_OK, NULL);

    atomic_set(&g_event_system.next_subscriber_id, (atomic_val_t) UINT32_MAX);
    g_event_system.subscriber_id_wrapped = false;

    zassert_equal(event_subscribe(161, test_event_noop_callback, NULL, &first_id), EVENT_OK, NULL);
    zassert_equal(first_id, UINT32_MAX, NULL);
    zassert_equal(event_subscribe(161, test_event_noop_callback, NULL, &second_id), EVENT_OK, NULL);
    zassert_not_equal(second_id, EVENT_SUBSCRIBER_ID_INVALID, NULL);
    zassert_not_equal(second_id, first_id, NULL);
    zassert_true(g_event_system.subscriber_id_wrapped, NULL);
}

/**
 * @brief 测试 event_system_stop 会停止分发器，start 时自动恢复
 */
ZTEST(test_event_system, test_event_system_stop_restarts_dispatcher) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_STOPPED, NULL);

    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief event_system_stop 不允许从分发器回调内部调用
 */
ZTEST(test_event_system, test_event_system_stop_rejected_from_callback) {
    uint32_t subscriber_id;

    atomic_set(&g_stop_from_callback_seen, 0);
    g_stop_from_callback_status = EVENT_OK;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(STOP_FROM_CALLBACK_EVENT_TYPE, "stop_from_cb"), EVENT_OK, NULL);
    zassert_equal(event_subscribe(STOP_FROM_CALLBACK_EVENT_TYPE, stop_from_callback_handler, NULL, &subscriber_id),
                  EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_publish_copy(STOP_FROM_CALLBACK_EVENT_TYPE, EVENT_PRIORITY_NORMAL, NULL, 0), EVENT_OK, NULL);
    zassert_true(ztest_wait_atomic_nonzero(&g_stop_from_callback_seen, 2000U), "回调应已执行");
    zassert_equal(g_stop_from_callback_status, EVENT_ERR_INVALID_ARG, "回调内 stop 应被拒绝");
    zassert_true(event_system_is_running(), "被拒绝后事件系统仍应运行");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

#if IS_ENABLED(CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK)

#define BLOCK_TEST_EVENT_TYPE 200U

static K_THREAD_STACK_DEFINE(block_pub_stack, 2048);
static struct k_thread block_pub_thread;
static atomic_t        block_pub_started;
static atomic_t        block_pub_done;
static event_status_t  block_pub_result;

static void block_publish_thread_entry(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    event_t ev = {.type = BLOCK_TEST_EVENT_TYPE, .priority = EVENT_PRIORITY_NORMAL};

    atomic_set(&block_pub_started, 1);
    block_pub_result = event_publish(&ev);
    atomic_set(&block_pub_done, 1);
}

/**
 * @brief BLOCK 策略：队列满时 stop 应使阻塞中的 publish 返回 NOT_RUNNING（不死锁）
 */
ZTEST(test_event_system, test_block_publish_unblocks_on_stop) {
    struct k_msgq* q;
    uint32_t       cap;
    event_t        filler = {.type = BLOCK_TEST_EVENT_TYPE, .priority = EVENT_PRIORITY_LOW};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(BLOCK_TEST_EVENT_TYPE, "block_test"), EVENT_OK, NULL);

    q = event_system_get_queue();
    zassert_not_null(q, NULL);
    cap = event_queue_capacity(q);

    for (uint32_t i = 0; i < cap; i++) {
        zassert_equal(event_publish(&filler), EVENT_OK, NULL);
    }

    atomic_set(&block_pub_started, 0);
    atomic_set(&block_pub_done, 0);
    /* K_NO_WAIT 创建即就绪运行，无需再调 k_thread_start */
    k_tid_t tid = k_thread_create(&block_pub_thread, block_pub_stack, K_THREAD_STACK_SIZEOF(block_pub_stack),
                                  block_publish_thread_entry, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
    zassert_not_null(tid, NULL);

    zassert_true(ztest_wait_atomic_nonzero(&block_pub_started, 2000U), "发布线程应已启动");
    zassert_equal(atomic_get(&block_pub_done), 0, "publish 应在满队列上阻塞");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);

    zassert_true(ztest_wait_atomic_eq(&block_pub_done, 1, 2000U), "stop 后阻塞 publish 应返回");
    zassert_equal(atomic_get(&block_pub_done), 1, NULL);
    zassert_equal(block_pub_result, EVENT_ERR_NOT_RUNNING, NULL);

    zassert_equal(k_thread_join(tid, K_MSEC(1000)), 0, NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}
#endif /* CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK */

/**
 * @brief 测试 event_is_running
 */
ZTEST(test_event_system, test_event_is_running) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 初始化后但未启动 */
    zassert_false(event_system_is_running(), "初始化后应为未运行");

    /* 启动后 */
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_true(event_system_is_running(), "启动后应为运行中");

    /* 停止后 */
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_false(event_system_is_running(), "停止后应为未运行");
}

/* =============================================================================
 * 测试夹具 (Test Fixtures)
 * ============================================================================= */

static void* event_system_suite_setup(void) {
    /* 全局初始化一次；重复 init 幂等返回 EVENT_OK，后续用例各自重 init */
    event_system_init();
    event_system_start();
    return NULL;
}

static void event_system_suite_teardown(void* fixture) {
    (void) fixture;
    event_system_stop();
}

/**
 * @brief 每个测试用例后的清理函数
 *
 * 确保测试间的状态隔离。
 */
static void event_system_test_teardown(void* fixture) {
    (void) fixture;
    /* 停止并关闭事件系统，清理所有注册的类型和订阅 */
    event_system_stop();
    event_system_shutdown();
    /* 不重新初始化，让下一个测试用例自己调用 init/start */
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

/* Zephyr: setup, before_each, after_each, suite_teardown — 每测清理须在 arg5（对照 test_module_manager.c） */
ZTEST_SUITE(test_event_system, NULL, event_system_suite_setup, NULL, event_system_test_teardown,
            event_system_suite_teardown);

/**
 * @file sys_time.h
 * @brief 墙钟时间服务头文件
 *
 * 可选服务（CONFIG_SYS_TIME_ENABLE）。Phase 3 提供基于 `k_uptime` 的软件墙钟；
 * 量产可叠加 SNTP 后端同步。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-06-13       1.1            zeh            64 位 uptime、互斥锁（与实现同步）
 * 2026-09-18       1.2            zeh            注明错误码体系双轨差异（登记延期）
 *
 * @note 错误码体系说明：本模块返回 APP_ERR_INIT / APP_ERR_INVALID_PARAM /
 *       APP_ERR_TIME 等应用层错误码，而 services 其余模块返回 -EINVAL / -EBUSY
 *       等 errno 风格负值——两套体系并存。统一为单一体系属跨模块接口变更，
 *       本轮登记延期（详见 2026-09-18 审查整改记录），暂以各自头文件声明为准。
 */

#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 核心 API
 * ============================================================================= */

/**
 * @brief 初始化时间服务（幂等，SYS_INIT 自动调用）
 * @return 0 成功
 */
int sys_time_init(void);

/**
 * @brief 设置当前 Unix 时间（毫秒）
 *
 * @param unix_ms UTC 毫秒时间戳
 * @return 0 成功；APP_ERR_INIT / APP_ERR_INVALID_PARAM
 */
int sys_time_set_unix_ms(int64_t unix_ms);

/**
 * @brief 获取当前 Unix 时间（毫秒）
 *
 * @param out_unix_ms 输出时间戳
 * @return 0 成功；APP_ERR_TIME 未设置；APP_ERR_INVALID_PARAM / APP_ERR_INIT
 */
int sys_time_get_unix_ms(int64_t* out_unix_ms);

/**
 * @brief 墙钟是否已有效（曾成功 set 或 SNTP 同步）
 */
bool sys_time_is_valid(void);

/**
 * @brief 清除有效标志（测试用）
 */
void sys_time_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_TIME_H */

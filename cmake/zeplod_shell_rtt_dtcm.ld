/*
 * shell RTT 通道 1 缓冲 → DTCM（.shell_rtt_dtcm 段）
 *
 * 背景：conf/targets/rtt_console.conf 把 shell 后端移到 RTT 通道 1，其
 * 上行/下行缓冲原本随 .bss 落在 SDRAM；Cortex-M7 D-Cache 开启时调试器
 * 会读到旧数据。DTCM 不经 cache，把缓冲放进 DTCM 后 DCACHE 可保持默认
 * 开启。片段经根 CMakeLists.txt 的 zephyr_linker_sources(SECTIONS ...)
 * 注入 SECTIONS 区，先于通用 .bss 规则生效（-fdata-sections 的输入段名
 * .bss.shell_rtt_up_buf / .bss.shell_rtt_down_buf 由本段首先匹配）。
 * 本片段仅适用于存在 DTCM 的目标，由 SEGGER_RTT_SECTION_DTCM 条件保护。
 */

SECTION_PROLOGUE(.shell_rtt_dtcm, (NOLOAD), SUBALIGN(4))
{
	__shell_rtt_dtcm_start = .;
	KEEP(*(".bss.shell_rtt_up_buf"))
	KEEP(*(".bss.shell_rtt_down_buf"))
	__shell_rtt_dtcm_end = .;
} GROUP_LINK_IN(DTCM)

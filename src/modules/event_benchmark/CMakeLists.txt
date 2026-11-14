#
# Copyright (c) 2026
# SPDX-License-Identifier: GPL-3.0
#

if(CONFIG_EVENT_BENCHMARK)
    target_sources(app PRIVATE event_benchmark.c)
    if(CONFIG_EVENT_BENCHMARK_SHELL)
        target_sources(app PRIVATE event_benchmark_shell.c)
    endif()
endif()

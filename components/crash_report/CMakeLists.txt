# Serial-free crash reporter (Task #-: post-hoc backtrace over WiFi).
#
# esp_system  -> panic_info_t / panic_exception_t (esp_private/panic_internal.h)
# event_log   -> event_log_emit() for the boot-time replay
# xtensa      -> XtExcFrame, esp_cpu_process_stack_pc(), esp_backtrace_get_next_frame()
#                (xtensa-only headers; a RISC-V target build still compiles, capturing PC + cause only)
set(reqs event_log esp_system)
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    list(APPEND reqs xtensa)
endif()

idf_component_register(
    SRCS "crash_report.c"
    INCLUDE_DIRS "include"
    REQUIRES ${reqs}
)

# Intercept the fatal-error handler: __wrap_esp_panic_handler snapshots the crash backtrace to
# RTC_NOINIT before __real_esp_panic_handler dumps + reboots. The flag is a link-time transformation,
# so it must reach the final app link -- it propagates there as an INTERFACE usage requirement because
# main REQUIRES crash_report. The panic_handler.c call site is a direct C call, which --wrap can rewrite.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

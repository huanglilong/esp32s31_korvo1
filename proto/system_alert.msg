# System resource alert
uint64  timestamp
uint32  current_value        # Current value (0-10000 = 0-100.00%)
uint32  threshold            # Alert threshold (0-10000 = 0-100.00%)
uint32  task_cpu_pct         # For CPU_HIGH: that task's CPU%, else 0
uint32  free_internal        # Free internal SRAM at alert time (bytes)
uint32  free_psram           # Free PSRAM at alert time (bytes)
char[16] task_name           # For CPU_HIGH: task name causing alert, else empty
uint8   alert_type           # 0=CPU_HIGH, 1=MEM_INTERNAL_HIGH, 2=MEM_PSRAM_HIGH
uint8   severity             # 0=INFO, 1=WARNING, 2=CRITICAL

uint8 ORB_QUEUE_LENGTH = 5

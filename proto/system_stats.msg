# System performance statistics
uint64  timestamp
uint32  free_internal        # Free internal SRAM (bytes)
uint32  free_psram           # Free PSRAM (bytes)
uint32  min_free_internal    # Historical minimum free internal SRAM (bytes)
uint32  min_free_psram       # Historical minimum free PSRAM (bytes)
uint32  task_count           # Current number of FreeRTOS tasks
uint32  total_cpu_pct        # Total busy CPU utilization (excludes IDLE), 0-10000 = 0-100.00%
uint32  core0_cpu_pct        # Core 0 busy CPU%, 0-10000 = 0-100.00%
uint32  core1_cpu_pct        # Core 1 busy CPU%, 0-10000 = 0-100.00%

uint8 ORB_QUEUE_LENGTH = 3

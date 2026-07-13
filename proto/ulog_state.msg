# ULog writer state
uint64  timestamp
char[128] filepath        # Current log file path (empty if not logging)
bool    logging           # True if currently logging to .ulog file

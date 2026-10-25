#pragma once
#include <stdbool.h>
extern volatile bool clock_time_valid;
extern volatile bool clock_wifi_connected;
extern char clock_ap_ssid[32];
char *clock_scan_json(void);
void clock_http_scan_test(void);
void clock_network_start(void);
bool clock_set_epoch(long long seconds);

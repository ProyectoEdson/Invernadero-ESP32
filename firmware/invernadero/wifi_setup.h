#pragma once

#include <stdint.h>

bool wifiSetupStart(const char *deviceId, const char *bootId);
bool wifiSetupConnected();
uint32_t wifiSetupSwitchCount();
void wifiSetupReportCloudReachable();

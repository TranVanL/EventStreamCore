#pragma once    
#include <thread>

void pinThreadToCore(std::thread& t, int core_id);
// thread_util.cpp
#include "thread_util.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

int bind_rt_thread()
{
    // TL_BIND_CORES=0 → 跳过（回退开关）
    const char *env = getenv("TL_BIND_CORES");
    if (env && strcmp(env, "0") == 0)
        return -1;

    // 默认绑 A76 大核 4/5/6（0x70）；CPU7 留给 NPU 中断 ISR（S99setfreq 绑定）
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(4, &cpuset);
    CPU_SET(5, &cpuset);
    CPU_SET(6, &cpuset);
    int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (ret != 0)
        fprintf(stderr, "[Bind] bind_rt_thread 失败 ret=%d\n", ret);
    return ret;
}

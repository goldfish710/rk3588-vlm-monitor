// thread_util.h
// CPU 亲和性绑定工具（性能优化 B 阶段）
//
// bind_rt_thread(): 把实时性敏感的线程（视频编码/AI 推理）绑到 RK3588 大核 A76。
//   - 默认掩码 0x70 = CPU4/5/6（大核簇，留 CPU7 给 NPU 中断 ISR）
//   - 环境变量 TL_BIND_CORES=0 跳过绑定（全局回退开关）
//   - 后台线程（ASR/chat/audio 等 nice 10）不绑定，由内核调度
//
// 注意：chat_llm 线程内会调用 rkllm_run（RKLLM 可能按需创建内部线程并继承
// affinity），故 chat_llm 永不绑定；main 线程必须在所有 init 之后才绑定。
#ifndef THREAD_UTIL_H
#define THREAD_UTIL_H

#include <pthread.h>

// 返回 0=成功，-1=跳过（env 关闭或失败不致命）
int bind_rt_thread();

#endif // THREAD_UTIL_H

# 13-八股:CMake 与构建系统

> 每题答案都锚定本项目真实构建(TLServer 交叉编译 RK3588)。用法:先背结论,
> 再结合项目代码吃透细节。配套实操:server/TLServer/build-linux.sh + CMakeLists.txt。

## Q1: CMake 是什么?和 Makefile 有什么区别?

**答**:CMake 是**构建系统生成器**,不是构建工具本身。它读 CMakeLists.txt,生成
Makefile/Ninja 工程,再由 make/ninja 真正编译。区别:
- Makefile:直接描述编译规则,平台/工具链相关,跨平台要手写多套
- CMake:跨平台抽象层,"一次描述,到处生成"——同一份 CMakeLists 可在 Linux
  (Makefile/Ninja)和 Windows(Visual Studio 工程)下构建
- CMake 的三阶段:configure(读配置、检查环境)→ generate(生成 Makefile)→
  build(执行编译)。`cmake ..` 做前两步,`make` 做第三步

**项目实例**:本项目 CMakeLists.txt 跨两个 SOC(rk3588/rk3568 时代)共用一份,
切换平台只改 `TARGET_SOC` 变量。

## Q2: 交叉编译是什么?需要哪三件套?

**答**:在 host 架构(x86 PC)上生成 target 架构(aarch64 板端)可执行文件。三件套:
1. **交叉编译器**:前缀如 `aarch64-buildroot-linux-gnu-gcc/g++`
2. **sysroot**:目标平台的根文件系统镜像(头文件 + 库)——编译时 include/lib
   搜索路径必须指向它,否则会链接到 host 的 x86 库(ABI 不匹配)
3. **链接约定/ABI**:目标平台的动态链接器、库命名规则

**项目实例**:build-linux.sh 用环境变量方式:
```sh
export GCC_COMPILER=/home/yzy/atk-dlrk3588-toolchain
export CC=${GCC_COMPILER}/bin/aarch64-buildroot-linux-gnu-gcc
export CXX=${GCC_COMPILER}/bin/aarch64-buildroot-linux-gnu-g++
```
CMakeLists 里 `include_directories(${SYSROOT})` 显式指向目标平台的 /usr/include。
更规范的做法是 toolchain.cmake 文件,小项目用环境变量够用(面试要能说出两种方式的差异)。

## Q3: 静态库和动态库的区别?板端部署选哪种?

**答**:
| | 静态库(.a) | 动态库(.so) |
|---|---|---|
| 链接时机 | 编译期,代码拷进可执行文件 | 运行期,由 ld.so 动态加载 |
| 产物大小 | 大(每个程序一份) | 小(多程序共享一份) |
| 升级 | 要重新编译链接 | 换 .so 即可(ABI 兼容前提下) |
| 部署 | 只推一个二进制 | 必须带 .so + 配 `LD_LIBRARY_PATH` |

**项目实例(三分类策略)**:
1. 小工具库源码直接编入(cJSON、minimp4)——无版本管理成本
2. 预编译静态链接(live555、rga、mpp、rknpu2)——部署只需推一个二进制
3. 运行时动态库集中放 install/lib(rkllmrt.so/rknnrt.so)——板端
   `LD_LIBRARY_PATH=./lib ./TLServer` 运行
**踩坑**:librkllmrt.so 依赖 libgomp.so.1,板端系统库没有,必须随包携带
(4B 实测 demo 阶段就栽在这:报 "libgomp.so.1: cannot open shared object file")。

## Q4: undefined reference 是什么?静态库链接顺序为什么敏感?

**答**:undefined reference = 编译通过(声明找到了),但**链接**时找不到符号定义。
原因排查顺序:①符号在哪个库里、库路径对不对 ②静态库**链接顺序**(被依赖的库
必须放在依赖它的库**后面**——链接器从左到右只扫描一次,每个库只取"当前还缺的
符号")③C/C++ 名字修饰破坏(缺 extern "C")④头文件与 .so 版本不一致。

**项目实例**:live555 三个库有依赖链
(groupsock → BasicUsageEnvironment → UsageEnvironment),顺序写反就 undefined
reference,把被依赖的放后面解决。

## Q5: extern "C" 是干嘛的?

**答**:C++ 有**名字修饰**(name mangling)——函数符号会被编码成包含参数类型的长名
(如 `_Z8funcNamei`),以支持重载。C 没有。extern "C" 告诉编译器这段代码按 C 的
符号规则生成,使 C 和 C++ 能互相链接。

**项目实例**:paho-mqtt 是 C 库,头文件自带 extern "C" 保护;但自己封装的 C 接口
给 C 文件用时必须手动包一层——第一次写封装漏了,链接期报一堆 mangled 名字找不到。

## Q6: 编译的四个阶段?各产什么?

**答**:预处理(`-E`,宏展开/#include 展开)→ 编译(`-S`,C/C++ → 汇编)→ 汇编
(`-c`,汇编 → 目标文件 .o)→ 链接(多个 .o + 库 → 可执行文件)。
调试编译问题的定位就是判断问题出在哪一阶段:语法错误=编译期;undefined
reference=链接期;头文件找不到=预处理期。

## Q7: -O2 和 -O3 区别?为什么项目用 -O2?

**答**:都是优化级别。-O2 常规优化(不显著增加代码体积与编译时间);-O3 更激进
(循环展开、函数内联更狠),代价是体积膨胀、编译变慢、浮点重排可能改变数值行为。
**项目选择 -O2 的理由**(面试可直接讲):性能热点根本不在 CPU 代码——视频编码在
MPP 硬件、AI 在 NPU、缩放旋转在 RGA,C++ 侧只有调度和轻量后处理,优化 CPU 代码
收益天花板很低;-O3 的浮点重排风险(后处理置信度计算)不可接受。

## Q8: include_directories 和 find_package 的区别?为什么项目多用前者?

**答**:include_directories/link_directories 是"手动挡"——直接告诉编译器头文件
和库在哪;find_package 是"自动挡"——按包的 xxxConfig.cmake 文件自动注入头文件
路径、库路径、编译选项。**项目多用手动挡的原因**:板端厂商库(rknpu2/rga/mpp)
不提供标准 CMake package 配置;手动指定路径简单直接、依赖关系可见。代价是移植性
差(路径写死)——面试可以说"厂商库生态不完善时的务实选择"。
**踩坑**:CMakeLists 曾 `find_package(OpenMP REQUIRED)`,本机交叉工具链没有
libgomp 导致 configure 失败,删掉该依赖(本项目不需要 OpenMP 并行)解决。

## Q9: SOURCES 用显式清单还是 file(GLOB)?

**答**:项目用**显式清单**——每个 .cpp 手写进 add_executable。
理由:GLOB 在**新增文件时不会自动触发 CMake 重新 configure**,新文件可能不被编译
(CMake 明确不推荐 GLOB 收集源文件);显式清单新增文件必须改 CMakeLists,换来
增量编译正确、依赖关系可审计。加 FrameProvider 时就是靠显式清单避免漏编。

## Q10: RPATH 是什么?和 LD_LIBRARY_PATH 什么关系?

**答**:RPATH 是**烧进可执行文件**里的动态库搜索路径(链接期 -Wl,-rpath 指定),
优先级高于系统默认路径;LD_LIBRARY_PATH 是**运行期环境变量**,优先级最高。
板端部署通常用 LD_LIBRARY_PATH=./lib(灵活、不用重编);RPATH 适合希望"免环境
变量"直接运行的场景。面试加分:知道两者的优先级关系
(LD_LIBRARY_PATH > RPATH/RUNPATH > /etc/ld.so.conf > 默认路径)。

## Q11: 本项目 CMakeLists 的结构怎么解读?

**答**(对着 CMakeLists.txt 讲):
```
cmake_minimum_required(VERSION ...)   # CMake 最低版本
project(TLServer)                     # 工程名
set(...)                              # 变量:SOURCE 目录/工具链路径
option(PIPER_TTS OFF)                 # 开关:./build-linux.sh -p 打开
include_directories(...)              # 头文件搜索路径(分层:SERVER/CPP/第三方)
link_directories(...)                 # 库搜索路径
add_executable(TLServer ${SOURCES})   # 可执行目标(显式源文件清单)
target_link_libraries(TLServer ...)   # 链接库
install(TARGETS ... DESTINATION ...)  # 安装规则:产物复制到 install 目录
```
**部署链路**:编译产物 → install/rk3588_linux_aarch64/atk_rtsp_ai_server/
(binary + lib + model)→ adb push 到板端 /AI/ → LD_LIBRARY_PATH=./lib 运行。

## Q12: 版本不匹配类错误怎么排查?

**答**:症状:编译通过,运行时 `version GLIBCXX_3.4.x not found` / 符号找不到。
根因:交叉编译环境的库版本与板端运行环境不一致(头文件是新的,.so 是旧的,或相反)。
排查:①`strings libstdc++.so.6 | grep GLIBCXX` 对照版本 ②ldd 看运行时解析到哪个库
③板端与交叉环境逐版本核对。**项目实例**:板端 libpaho 版本与 PC 交叉环境不一致,
运行时找不到符号——教训写进交接说明书:交叉编译环境的库必须和板端逐版本核对。

## 附:本项目构建相关真实踩坑速记(面试故事素材)

1. OpenMP REQUIRED → 工具链无 libgomp → configure 失败(删依赖)
2. 静态库链接顺序反了 → undefined reference(被依赖库放后面)
3. 封装 C 接口漏 extern "C" → mangled 名字找不到
4. 板端缺 libgomp.so.1 → 随包携带(TLServer /AI/lib 与 4B demo 各踩一次)
5. file(GLOB) 陷阱 → 显式 SOURCES 清单

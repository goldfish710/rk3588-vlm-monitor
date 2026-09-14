# 05. 项目精讲:config 配置系统

> 目标:读懂 config.h / config.cpp 的全部设计,并掌握"配置系统"这个
> 嵌入式项目通用组件的写法。这是项目最简单、最完整的模块——完美的起步精讲。

---

## 5.1 这个模块解决什么问题

板端程序有成百个参数(分辨率/码率/模型路径/关键词/API key……),每个都写死在代码里,每次调整都要重新编译。config 系统的目标:**参数进 ini 文件,改文件重启即生效**。

```ini
[main_stream]
width=1920
bitrate=4000000

[asr]
asr_enable=1
asr_keywords=我好痛,救命,你好

[chat]
llm_model=deepseek-v4-flash
```

---

## 5.2 数据结构:带默认值的 struct 群

```cpp
// server/TLServer/config.h
struct MainStreamConfig {
    int width = 1920;                  // C++11 成员默认值
    int height = 1080;
    int bitrate = 4000000;
    std::string rtsp_path = "h265";
};

struct AsrConfig {
    bool enable = false;
    std::string model_dir = "./model";
    std::string keywords = "救命,来人";
    int cooldown_sec = 30;
    // 运行时字段(不写 ini,由 TLmain 从别的配置复制):
    int capture_rate = 44100;
};

struct AppConfig {
    MainStreamConfig main_stream;
    SubStreamConfig sub_stream;
    AiConfig ai;
    RecordConfig record;
    AsrConfig asr;
    ChatConfig chat;
    bool load(const std::string &path);       // 成员函数:读取解析
    bool saveDefault(const std::string &path); // 生成默认 ini
};
```

**设计要点**(C 程序员的视角逐个看):

1. **默认值在声明处**:`AppConfig g_config;` 一创建就全部可用——程序即使读不到 ini,也能用默认配置跑。这是 C 里"全 memset + 手工设默认"的进化
2. **struct 套 struct**:按 ini 的 `[section]` 分组,字段名对齐 ini 键名,读代码时天然映射
3. **全局单例**:

```cpp
extern AppConfig g_config;   // config.h 声明
AppConfig g_config;          // config.cpp 定义(唯一一份)
```

所有模块 include config.h 就能读配置,不用层层传参。嵌入式项目常用这招;缺点是全局可变——项目约定:启动时 load 一次,之后**只读**。

---

## 5.3 解析器:手写 if-else 键值匹配

### load() 的主循环

```cpp
// server/TLServer/config.cpp
bool AppConfig::load(const std::string &path) {
    std::ifstream f(path.c_str());     // C++ 文件流(对比 C 的 fopen)
    std::string section;               // 当前 [section]
    std::string line;

    while (std::getline(f, line)) {    // 逐行读,自动处理换行
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;  // 注释

        if (line[0] == '[' && line[line.size()-1] == ']') {
            section = line.substr(1, line.size() - 2);   // 切换 section
            continue;
        }

        std::string key, value;
        if (!parseKeyValue(line, key, value)) continue;

        // ----- 按 section 分派 -----
        if (section == "main_stream") {
            if (key == "width")  main_stream.width = toInt(value, main_stream.width);
            else if (key == "rtsp_path") main_stream.rtsp_path = value;
        } else if (section == "chat") {
            if (key == "wake_word") chat.wake_word = value;
        }
        // ...
    }
    return true;
}
```

### 关键观察:C++ 和 C 的写法几乎一样,差异在工具函数

```cpp
// 你看这些像不像 C?逐字符处理,完全一样。C++ 只是多了这些帮手:
line.substr(1, n);              // 截子串(对比 C 的 strncpy+手动补\0)
line.find('=');                 // 找字符(对比 strchr)
line.empty();                   // 判空(对比 strlen==0)
```

### 类型转换的"带默认值"模式

```cpp
main_stream.width = toInt(value, main_stream.width);
//                        ↑解析失败时用这个默认值
```

所以 ini 里写 `width=abc` 也不会崩,而是保持默认值——**健壮性来自每个转换都有兜底**。

---

## 5.4 saveDefault:生成默认配置文件

```cpp
bool AppConfig::saveDefault(const std::string &path) {
    std::ofstream f(path.c_str());          // 输出文件流
    f << "[main_stream]\n";
    f << "width=" << main_stream.width << "\n";
    // ...
    return true;
}
```

`<<` 流输出(对比 C 的 fprintf),std::string/int 直接拼接,不用写 %d/%s 格式串。

启动逻辑(TLmain)就三行:

```cpp
const std::string config_path = "./config.ini";
if (access(config_path.c_str(), F_OK) != 0)   // 文件不存在
    g_config.saveDefault(config_path);        // 生成默认 ini
g_config.load(config_path);                   // 读取覆盖默认值
```

**首次运行自举**:没配置 → 生成默认 → 读默认;有配置 → 直接读。嵌入式产品的标配流程。

---

## 5.5 我们踩过的坑(这个模块的活教材)

**坑 1:追加配置键时落错 section**

曾把 `asr_segment_timeout_ms` 追加到 ini 的 `[chat]` 节末尾——解析器在 chat 分支找不到这个键,静默忽略,用户改值无效。教训:
- 解析器对**未知键静默忽略**是方便也是危险:拼错键名/放错 section 都不报错
- 排查思路:启动日志打印实际生效的值(项目后来加了 `[Config] 主码流:...` 打印),或给 load 加"未识别键"警告

**坑 2:默认值双刃剑**

`osd_fps_enable` 曾经默认 true,但板端旧 ini 没有这个键——所以"没配"不等于"关闭"。改默认值时要考虑老配置文件的兼容。

---

## 5.6 思考题

1. 给 config 系统加一个新键(比如 `[chat] reply_tone=温和`),需要改哪几个地方?动手列清单(结构体字段/解析分支/saveDefault/使用处)。
2. `toInt(value, default)` 的 value 是 `"123abc"` 时,atoi 返回什么?如果 ini 里有中文(UTF-8 多字节),trim 和行读取会出问题吗?
3. 为什么 `g_config` 用全局变量而不是单例类(像 Logger::getInstance())?两种方案各自利弊是什么?

---

## 一句话总结

配置系统 = 带默认值的 struct 群 + 手写 ini 解析 + 首次运行自举。读懂它,你就理解了项目里所有"启动参数"从哪来、怎么改、为什么改错时静默。

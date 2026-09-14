# 11. 项目精讲:AudioPlayback 播放 + TTS 链路(含 base64/cJSON 实践)

> 目标:读懂 AudioPlayback.h/.cpp 和 chat_engine.cpp 里的 doTts/playReply。
> 本章较短——因为知识点大部分前面学过,这里是"综合应用":
> 文件解析、数据变换(重采样/声道复制)、C 库的 C++ 包装。

---

## 11.1 模块职责:把一段 wav 变成喇叭声

```
TTS 音频(wav,16kHz mono)
  → 解析 wav 头(采样率/声道)
  → 重采样到 44.1kHz(RK809 共享时钟,必须与采集一致)
  → mono 复制为双声道(硬件最少 2ch)
  → snd_pcm_writei 循环写入(xrun 恢复)
  → drain(排空) → close
```

这一串"数据整形"都源于**硬件的三条约束**(都是我们实测出来的):
1. RK809 采集/播放共享时钟 → 播放必须 44.1kHz
2. 硬件最少 2 声道 → mono 必须复制
3. 直连 hw:0,0(不能用 default/dmix,与采集并发会阻塞)

**硬件约束驱动软件设计**——嵌入式项目读代码时永远要问"这段代码是在迁就什么硬件特性"。

---

## 11.2 WAV 头解析:二进制格式的 C++ 读法

```cpp
// AudioPlayback.cpp  parseWavHeader()
char hdr[12];
if (fread(hdr, 1, 12, f) != 12 ||
    memcmp(hdr, "RIFF", 4) != 0 ||
    memcmp(hdr + 8, "WAVE", 4) != 0)
    return false;

while (!have_data) {
    char ch[8];
    if (fread(ch, 1, 8, f) != 8) break;
    uint32_t sz = (uint32_t)(uint8_t)ch[4] | ((uint32_t)(uint8_t)ch[5] << 8) |
                  ((uint32_t)(uint8_t)ch[6] << 16) | ((uint32_t)(uint8_t)ch[7] << 24);
    if (memcmp(ch, "fmt ", 4) == 0) {
        std::vector<uint8_t> b(sz);
        fread(b.data(), 1, sz, f);
        uint16_t fmt_tag = (uint16_t)(b[0] | (b[1] << 8));
        channels = (unsigned int)(b[2] | (b[3] << 8));
        rate = (unsigned int)(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
        uint16_t bits = (uint16_t)(b[14] | (b[15] << 8));
        if (fmt_tag != 1 || bits != 16) break;    // 只支持 PCM16
    } else if (memcmp(ch, "data", 4) == 0) {
        pcm_bytes = sz; pcm_off = ftell(f);
        have_data = true;
    } else {
        fseek(f, sz, SEEK_CUR);                    // 未知块:跳过
    }
}
```

### 逐点看

1. **小端手动拼整数**:`ch[4] | ch[5]<<8 | ...` —— 和 C 完全一样。C++ 不改变二进制协议的处理方式,只是 b 缓冲用了 vector(自动释放)
2. **未知块跳过**:WAV 格式允许 RIFF 块任意顺序/附加块(如 LIST 元数据),解析器必须"认得的处理、不认得的跳过"——**协议解析的通用礼貌**
3. 防御链:magic 检查 → fmt_tag/bits 检查 → 找不到 data 返回 false。每层失败都有明确路径,而不是半解析状态继续跑

---

## 11.3 数据整形:重采样 + 声道复制(纯 C 思维,C++ 容器)

```cpp
// 16kHz → 44.1kHz 线性插值
std::vector<int16_t> resampled;
resampled.resize((size_t)((double)mono.size() * PLAY_RATE / rate) + 1);
double step = (double)rate / PLAY_RATE;         // 每输出样本对应输入位置步进
for (size_t i = 0; i < resampled.size(); i++) {
    double pos = step * i;
    size_t i0 = (size_t)pos;
    if (i0 + 1 >= src_n) { resampled[i] = src[src_n - 1]; continue; }  // 尾部防越界
    double frac = pos - i0;
    resampled[i] = (int16_t)((double)src[i0] * (1.0 - frac) + (double)src[i0 + 1] * frac);
}

// mono → 双声道(左右相同)
std::vector<int16_t> stereo(src_n * 2);
for (size_t i = 0; i < src_n; i++)
    stereo[i * 2] = stereo[i * 2 + 1] = src[i];
```

**线性插值公式**(C 里也会写):`out[i] = in[i0]*(1-frac) + in[i0+1]*frac`。注意:
- **上采样(16k→44.1k)不会混叠**:源信号最高 8kHz,目标 Nyquist 22kHz 装得下,线性插值够用。下采样才需要抗混叠滤波——知道这个,你就知道什么时候能偷懒
- 尾部 `if (i0+1 >= src_n)` 分支:插值需要两个样本,最后一个样本没有"下一个",取尾值兜底。**插值/卷积类代码必踩的边界,这里显式处理了**

---

## 11.4 播放循环:xrun 恢复(和采集对称)

```cpp
const int16_t *p = stereo.data();
snd_pcm_uframes_t frames = (snd_pcm_uframes_t)src_n;
while (frames > 0) {
    snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, frames);
    if (n < 0) {
        if (snd_pcm_recover(pcm, (int)n, 1) < 0) {   // xrun:prepare 后重试
            fprintf(stderr, "[Playback] 写失败: %s\n", snd_strerror((int)n));
            snd_pcm_close(pcm);
            return -1;
        }
        continue;                                     // 恢复后重写同位置
    }
    p += (size_t)n * 2;                                // 写成功 n 帧,指针前移
    frames -= (snd_pcm_uframes_t)n;
}
snd_pcm_drain(pcm);    // 等硬件播完剩余缓冲
snd_pcm_close(pcm);
```

与 AudioCapture 的 readi 循环**镜像对称**:一个读、一个写,错误恢复套路完全相同。学会一个,另一个白送。

`drain` 与 `drop` 的区别(面试考点):drain = 播完剩余数据再停;drop = 立刻丢弃。正常播完用 drain,紧急停止用 drop(采集关闭时就是 drop——停得快)。

---

## 11.5 doTts:base64 解码 + cJSON 取值(chat_engine.cpp)

```cpp
// 自写 base64 解码(30 行,cJSON 不管这个)
std::vector<uint8_t> base64Decode(const std::string &in) {
    static const int8_t rev[256] = { /* 查表:ASCII → 6bit 值,-1=非法 */ };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int v = (c < 256) ? rev[c] : -1;
        if (v < 0) continue;                 // 跳过换行等非 base64 字符
        val = (val << 6) | v;                // 累积位
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));   // 每满 8 位出一个字节
            bits -= 8;
        }
    }
    return out;
}
```

位运算密集——C 程序员的舒适区。`static const` 查表放函数内:只初始化一次,作用域收窄。

取 JSON 的防御式取值:

```cpp
cJSON *code = cJSON_GetObjectItem(r, "code");
cJSON *msg  = cJSON_GetObjectItem(r, "message");
if (!cJSON_IsNumber(code) || code->valueint != 3000) { ... }
cJSON *data = cJSON_GetObjectItem(r, "data");
if (!cJSON_IsString(data)) { ... }
```

**先查类型再取值**——JSON 是弱类型,服务器返回什么全凭心情,每个字段都要防御(对比直接 `data->valuestring` 的裸奔写法)。

---

## 11.6 playReply:回声门控的时序

```cpp
void ChatEngine::playReply(const std::string &text, std::string &err) {
    std::string wav;
    if (!doTts(text, wav, err)) return;
    if (play_state_cb_) play_state_cb_(true);       // ① 开闸:抑制 ASR
    int rc = playWavFile(wav, cfg_.playback_device);
    if (rc != 0) { err = "播放失败"; play_state_cb_(false); return; }
    for (int i = 0; i < 10 && running_.load(); i++)  // ② 尾 2s 继续抑制
        usleep(200000);
    if (play_state_cb_) play_state_cb_(false);       // ③ 关闸
    remove(wav.c_str());                             // ④ 清理临时文件
}
```

**尾 2s 为什么要抑制**(项目实测结论):喇叭声音被 mic 录进去 → ASR 要 ~1.5s 才能"消化"这些回声 token。播放停止立刻开闸,残留的回声就会变成"用户说的话"。**抑制窗口 = 播放时长 + 系统延迟余量**——这是回声消除(简化版)的时序核心。

`remove()` 清理临时文件 + 失败路径也要关闸——每条路径都成对开闭门控,RAII 思想的手写版。

---

## 11.7 思考题

1. 重采样代码的 `step = rate / PLAY_RATE`——如果反过来写(PLAY_RATE/rate),插值会怎样?推导一个样本验证。
2. `resampled.resize(...+1)` 的 +1 是为什么?不加会怎样?
3. playWavFile 中途 return -1 的路径(写失败)有没有漏关的 pcm 句柄?对照代码逐条 return 检查——这就是代码审查的"资源审计法"。
4. 回声门控的尾 2s 如果改成 5s,用户体验差在哪?改成 0.5s 会出什么 bug?

---

## 一句话总结

播放链路 = 格式解析(防御式)+ 数据整形(为硬件约束服务)+ 循环写入(对称于采集)+ 门控时序(回声抑制)。数据从哪里来、被什么约束改造、往哪里去——三个问题贯穿一切数据处理代码。

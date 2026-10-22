#ifndef _UART_AUDIO_CODEC_H
#define _UART_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/uart.h>
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>

#include <vector>
#include <cstdint>
#include <functional>

extern "C" {
#include "layer3.h"   // shine MP3 编码器（components/shine）
}

/*
 * UartAudioCodec —— 通过 UART 对接 CI1302 离线语音芯片的 AudioCodec 实现。
 *
 * 设计哲学（对齐 VB6824 模式）：
 *   - 芯片做所有事：唤醒词检测、AEC、降噪、VAD、Opus 编码、MP3 播放
 *   - ESP32 只管收发：上行收到 Opus 帧直通转发，下行收到 PCM 转 MP3 推给芯片
 *   - 无会话控制：不干预"什么时候开始录/什么时候停录"，芯片自己 VAD 控制
 *   - 零耦合：codec 层不依赖 Application，所有事件通过回调上报给板级
 *
 * 数据流：
 *  上行（1302 -> ESP32 -> 云端）：
 *    1302 VAD 检测到人声 → 自动开始推 Opus 帧(0x0105 PCM_MIDDLE)
 *    → ESP32 入直通队列 → AudioService 转发到云端（不二次编码）
 *    → 1302 VAD_END → 发 0x0106 PCM_FINISH → ESP32 清队列
 *    → 服务器自己 VAD 检测到无音频 → 提交 STT
 *
 *  下行（云端 -> ESP32 -> 1302）：
 *    云端 Opus 解码 PCM → Write() → shine 编码 MP3 → 缓冲
 *    → 攒够 8KB 发 0x0201 NET_PLAY_START → 1302 拉流播放
 *    → 1302 发 0x020A PLAY_DATA_GET → ESP32 回 0x020B PLAY_DATA_RECV
 *    → tts.stop → NotifyStreamEnd → flush + 尾部静音 → 0x020C PLAY_DATA_END
 *    → 1302 播完 → 0x020D PLAY_TTS_END
 *
 *  唤醒流程：
 *    1302 检测到唤醒词 → 发 0x0102 WAKE_UP
 *    → codec 层触发 OnWakeUp 回调 → 板级决定怎么处理（调 WakeWordInvoke 等）
 *
 *  UART 帧格式（16 字节小端帧头 + 负载）：
 *    magic u32 = 0x5A5AA5A5（字节序 a5 a5 5a 5a）| checksum u16（负载逐字节累加）|
 *    type u16 | len u16 | version u16（普通命令 0x0100）| fill_data u32 = 0x12345678
 */
class UartAudioCodec : public AudioCodec {
public:
    UartAudioCodec(uart_port_t uart_port, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate);
    virtual ~UartAudioCodec();

    virtual void Start() override;
    virtual void SetOutputVolume(int volume) override;

    /* CI1302 模式无本地硬件开关，保持使能标志恒真 */
    virtual void EnableInput(bool enable) override {}
    virtual void EnableOutput(bool enable) override {}

    /* 上行直通：CI1302 已编码的 Opus 帧 */
    virtual bool HasUplinkOpusPacket() override;
    virtual bool PopUplinkOpusPacket(std::unique_ptr<AudioStreamPacket>& packet) override;

    /* 下行播放（拉流式）：流结束通知 + 打断 */
    virtual void NotifyStreamEnd() override;
    virtual void AbortPlayback() override;

    /* 唤醒回调（板级注册）——对齐 VB6824 的 OnWakeUp 模式 */
    typedef std::function<void()> WakeUpCallback;
    void OnWakeUp(WakeUpCallback callback) { wake_up_callback_ = std::move(callback); }

protected:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

private:
    // ---- CI1302 协议命令字 ----
    enum Ci1302Cmd : uint16_t {
        CI_CMD_LOCAL_ASR_NOTIFY   = 0x0101,  // 本地语音识别结果通知
        CI_CMD_WAKE_UP            = 0x0102,  // 唤醒
        CI_CMD_VAD_END            = 0x0103,  // VAD 结束
        CI_CMD_PCM_MIDDLE         = 0x0105,  // PCM 中间包（CI1302 侧 Opus 编码帧）
        CI_CMD_PCM_FINISH         = 0x0106,  // PCM 数据结束包
        CI_CMD_VAD_START          = 0x0108,  // VAD 开始
        CI_CMD_EXIT_WAKE_UP       = 0x0109,  // 退出唤醒
        CI_CMD_SET_AUDIO_VOLUME   = 0x0117,  // 设置语音芯片音量（1 字节，1-7）
        CI_CMD_SET_VAD_SENSITIVITY_ACTIVATE_LENTH = 0x0110,  // 静音多久判 VAD 结束
        CI_CMD_NET_PLAY_START     = 0x0201,  // 开始播放（启动 CI1302 拉流）
        CI_CMD_NET_PLAY_STOP      = 0x0204,  // 停止播放（立即停）
        CI_CMD_PLAY_DATA_GET      = 0x020A,  // CI1302 请求播放数据
        CI_CMD_PLAY_DATA_RECV    = 0x020B,  // ESP32 下发播放数据
        CI_CMD_PLAY_DATA_END     = 0x020C,  // ESP32 通知播放数据发完
        CI_CMD_PLAY_TTS_END      = 0x020D,  // CI1302 播放完成上报
        CI_CMD_AUDIO_SYS_READY   = 0x0601,  // CI1302 语音系统就绪
        CI_CMD_CMD_EXEC_STATE     = 0x0801,  // CI1302 指令执行应答
    };

    static constexpr uint32_t CIAS_MAGIC       = 0x5A5AA5A5;
    static constexpr uint32_t CIAS_FILL_DEF    = 0x12345678;
    static constexpr uint16_t CIAS_VERSION_DEF = 0x0100;
    static constexpr size_t   CIAS_HEADER_SIZE = 16;
    static constexpr size_t   CIAS_MAX_PAYLOAD = 2048;

    // ---- 上行（1302 Opus 直通）----
    static constexpr size_t   UPLINK_OPUS_FRAME_MAX  = 256;
    static constexpr size_t   UPLINK_OPUS_QUEUE_LEN  = 64;

    // ---- 下行（MP3 播放）----
    static constexpr int MP3_SAMPLE_RATE          = 24000;
    static constexpr int MP3_BITRATE              = 48;
    static constexpr size_t DOWNLINK_MP3_BUF_SIZE = 64 * 1024;
    static constexpr uint32_t PLAY_DATA_MAX_CHUNK = 1024;
    static constexpr size_t   PLAY_PREBUFFER_BYTES     = 8192;
    static constexpr int      PLAY_PREBUFFER_TIMEOUT_MS = 1500;
    static constexpr int      PLAY_DATA_WAIT_MS         = 100;
    static constexpr int      PLAY_NO_GET_TIMEOUT_MS    = 10000;
    static constexpr int      PLAY_RESTART_TIMEOUT_MS   = 3000;
    static constexpr int      PLAY_RESTART_MAX          = 2;
    static constexpr int      PLAY_AUTO_END_GRACE_MS    = 2000;
    static constexpr size_t   PLAY_STALL_RESET_BYTES    = 32 * 1024;
    static constexpr int      PLAY_STALL_RESET_MS       = 1500;
    static constexpr int      PLAY_TAIL_SILENCE_MS      = 1000;
    static constexpr uint8_t  VAD_END_SILENCE_INDEX     = 20;

    uart_port_t uart_port_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    int baud_rate_;

    // ---- UART 收发 ----
    TaskHandle_t rx_task_handle_ = nullptr;
    SemaphoreHandle_t tx_mutex_ = nullptr;
    SemaphoreHandle_t state_mutex_ = nullptr;

    // ---- 上行直通 ----
    QueueHandle_t uplink_opus_queue_ = nullptr;

    // ---- 下行播放 ----
    StreamBufferHandle_t downlink_mp3_sb_ = nullptr;
    QueueHandle_t play_req_queue_ = nullptr;
    TaskHandle_t play_task_handle_ = nullptr;

    enum class PlayState : uint8_t { kIdle, kPrebuffer, kPlaying };
    PlayState play_state_ = PlayState::kIdle;
    volatile TickType_t last_write_tick_ = 0;
    volatile TickType_t last_get_tick_ = 0;
    volatile TickType_t session_start_tick_ = 0;
    volatile bool stream_end_requested_ = false;
    volatile bool stream_end_reported_ = false;

    // ---- shine MP3 编码器 ----
    shine_t shine_enc_ = nullptr;
    int shine_samples_per_pass_ = 0;
    std::vector<int16_t> mp3_accum_;
    SemaphoreHandle_t mp3_mutex_ = nullptr;

    // ---- 播放看门狗 ----
    int play_restart_count_ = 0;

    // ---- RX 解析 ----
    std::vector<uint8_t> rx_parse_buf_;

    // ---- 唤醒回调 ----
    WakeUpCallback wake_up_callback_;

    // ---- 方法 ----
    void InitUart();
    static void RxTaskEntry(void* arg);
    void RxTaskLoop();
    void ParseAndDispatch();
    void DispatchFrame(uint16_t cmd, const uint8_t* payload, uint16_t len);
    void SendFrame(uint16_t cmd, const uint8_t* payload, uint16_t len);
    void SendCmd(uint16_t cmd);

    // 上行
    void OnWakeUpEvent();
    void QueueUplinkOpus(const uint8_t* payload, uint16_t len);
    void ClearUplinkOpusQueue();

    // 下行
    static void PlayTaskEntry(void* arg);
    void PlayTaskLoop();
    void HandlePlayDataGet(uint32_t request);
    void MaybeAutoEndOrAbort();
    void StartPlaySessionIfReady();
    void FinishPlaySession();
    void AbortPlaySessionInternal(const char* reason);
    bool Mp3EncoderInit();
    void Mp3Encode(const int16_t* data, int samples);
    void Mp3Flush(std::vector<uint8_t>& tail);
};

#endif // _UART_AUDIO_CODEC_H

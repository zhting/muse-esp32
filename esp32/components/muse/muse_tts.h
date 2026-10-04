#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_ENABLED

/**
 * @brief 异步启动火山引擎流式 TTS 请求 (豆包语音合成 2.0 unidirectional 接口)
 * @param text 待合成的文本
 * @return 启动成功返回 true，若未配置 API Key 或初始化失败返回 false
 */
bool muse_tts_start(const char *text);

/**
 * @brief 从流式 TTS 接收缓冲区读取已解码就绪的 MP3 二进制数据块
 * @param out_mp3 目标缓冲区
 * @param max_len 最大读取字节数
 * @return 实际读取到的 MP3 字节数（若当前无数据返回 0）
 */
size_t muse_tts_read_chunk(uint8_t *out_mp3, size_t max_len);

/**
 * @brief 查询流式 TTS 是否已经完整传输并消费完毕
 * @return true 表示整个音频流已全部接收完成且缓冲区已排空
 */
bool muse_tts_is_finished(void);

/**
 * @brief 中断/取消当前正在进行的 TTS 传输（释放网络连接和缓冲区）
 */
void muse_tts_cancel(void);

#else

static inline bool muse_tts_start(const char *text) { (void)text; return false; }
static inline size_t muse_tts_read_chunk(uint8_t *out_mp3, size_t max_len) { (void)out_mp3; (void)max_len; return 0; }
static inline bool muse_tts_is_finished(void) { return true; }
static inline void muse_tts_cancel(void) {}

#endif

#ifdef __cplusplus
}
#endif

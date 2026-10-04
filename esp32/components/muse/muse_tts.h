#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 使用火山引擎 seed-audio-1.0 将文本合成为 MP3 音频（温柔女声音色）
 * @param text 待合成的文本
 * @param out_mp3 存储输出 MP3 音频的缓冲区
 * @param max_mp3_len 缓冲区最大长度
 * @return 成功返回 MP3 二进制字节数，失败返回 <= 0
 */
int muse_tts_fetch(const char *text, uint8_t *out_mp3, size_t max_mp3_len);

#ifdef __cplusplus
}
#endif

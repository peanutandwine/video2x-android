#pragma once

// Anime4K 直连管线（a4k/）与 JNI 层之间的最小接口。
// 只在 processAnime4K 执行期间有意义。
int a4k_processed_frames();
int a4k_total_frames();
void a4k_request_abort();

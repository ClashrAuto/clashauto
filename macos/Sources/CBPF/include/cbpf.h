// BPF 的几个 ioctl 常量在 <net/bpf.h> 里是 _IOW(...) 宏，Swift 导入不进来 ——
// 用几个 static const 把它们的**求值结果**暴露给 Swift。纯常量，无逻辑。
#ifndef COAST_CBPF_H
#define COAST_CBPF_H

#include <sys/ioctl.h>
#include <net/bpf.h>

static const unsigned long COAST_BIOCSETIF     = BIOCSETIF;
static const unsigned long COAST_BIOCSHDRCMPLT = BIOCSHDRCMPLT;
// 收包（被动抢答设备 NS）用：立即模式、读缓冲长度、别回显自己发出的帧。
static const unsigned long COAST_BIOCIMMEDIATE = BIOCIMMEDIATE;
static const unsigned long COAST_BIOCGBLEN     = BIOCGBLEN;
static const unsigned long COAST_BIOCSSEESENT  = BIOCSSEESENT;

// BPF 读缓冲里每个包前有 struct bpf_hdr，包间按 BPF_WORDALIGN 对齐。这几个宏/结构在
// Swift 里导不进来（bpf_hdr 布局带 padding、BPF_WORDALIGN 是宏），用内联函数把结果暴露出去，
// 免得 Swift 侧手算结构偏移出错。
static inline unsigned int coast_bpf_caplen(const void *p) { return ((const struct bpf_hdr *)p)->bh_caplen; }
static inline unsigned int coast_bpf_hdrlen(const void *p) { return ((const struct bpf_hdr *)p)->bh_hdrlen; }
static inline int coast_bpf_wordalign(int x) { return BPF_WORDALIGN(x); }

#endif

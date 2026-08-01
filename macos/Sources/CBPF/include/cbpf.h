// BPF 的几个 ioctl 常量在 <net/bpf.h> 里是 _IOW(...) 宏，Swift 导入不进来 ——
// 用几个 static const 把它们的**求值结果**暴露给 Swift。纯常量，无逻辑。
#ifndef COAST_CBPF_H
#define COAST_CBPF_H

#include <sys/ioctl.h>
#include <net/bpf.h>

static const unsigned long COAST_BIOCSETIF     = BIOCSETIF;
static const unsigned long COAST_BIOCSHDRCMPLT = BIOCSHDRCMPLT;

#endif

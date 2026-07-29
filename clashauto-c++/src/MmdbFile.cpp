// MmdbFile 的实现。设计与取舍见 MmdbFile.h。
#include "MmdbFile.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtGlobal>

namespace {

// 体积下限。真实的 country.mmdb 在 5–8 MB 量级；这里取一个足够松、但能挡掉「GitHub 502 页面 /
// 代理错误页 / 半个 TLS 记录」的值。不取更严的下限：以后上游换更紧凑的库不该被这里误杀。
constexpr qint64 kMinBytes = 512 * 1024;

// metadata 段的定位标记。规范：从文件末尾往前找**最后一次**出现，且搜索范围以 128 KiB 为界
//（否则一份恰好含有这串字节的数据段会把定位带偏）。
const char kMarker[] = "\xab\xcd\xef" "MaxMind.com";
constexpr int kMarkerLen = 14; // 3 字节魔数 + "MaxMind.com"
constexpr qint64 kMetadataSearchWindow = 128 * 1024;

// —— metadata 段的最小解码器 ——
// 只够读出 map 里的几个整数键，不解数据段。类型编码见 MaxMind DB 规范：
//   1=pointer 2=utf8 3=double 4=bytes 5=uint16 6=uint32 7=map
//   扩展型（首字节高 3 位为 0，真实类型 = 7 + 下一字节）：8=int32 9=uint64 10=uint128
//   11=array 12=container 13=end_marker 14=boolean 15=float
struct Cursor {
    const uchar *p = nullptr;
    qint64 size = 0;   // metadata 段长度（= 从标记之后到文件末尾）
    qint64 off = 0;
    bool bad = false;  // 越界/畸形，一旦置位就一路短路到底

    bool need(qint64 n)
    {
        if (bad || off + n > size) {
            bad = true;
            return false;
        }
        return true;
    }
    uchar take()
    {
        if (!need(1))
            return 0;
        return p[off++];
    }
    quint64 takeUint(int n)
    {
        if (n < 0 || n > 8 || !need(n))
            return 0;
        quint64 v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 8) | p[off++];
        return v;
    }
};

struct Value {
    int type = 0;        // 解出来的真实类型号
    quint64 num = 0;     // type ∈ {5,6,9,10,8,14} 时有效
    QByteArray str;      // type == 2 时有效
};

// 解一个值。skipOnly=true 时不保留字符串内容（省一次拷贝）。
// 递归深度：metadata 是扁平的 map/array，实测深度 ≤ 3；这里仍设一个上限防畸形文件把栈打穿。
Value decode(Cursor &c, int depth = 0);

void skipValue(Cursor &c, int depth)
{
    (void)decode(c, depth);
}

Value decode(Cursor &c, int depth)
{
    Value v;
    if (c.bad || depth > 16) {
        c.bad = true;
        return v;
    }
    const uchar ctrl = c.take();
    if (c.bad)
        return v;
    int type = ctrl >> 5;
    if (type == 0) {
        type = 7 + int(c.take());
        if (c.bad)
            return v;
    }
    v.type = type;

    quint64 size = ctrl & 0x1f;
    if (type == 1) {
        // 指针：size 的高 2 位是长度档，低 3 位参与数值。指向 metadata 段内的偏移。
        const int ss = int((size >> 3) & 3);
        const quint64 low = size & 7;
        quint64 ptr = 0;
        switch (ss) {
        case 0: ptr = (low << 8) | c.takeUint(1); break;
        case 1: ptr = 2048 + ((low << 16) | c.takeUint(2)); break;
        case 2: ptr = 526336 + ((low << 24) | c.takeUint(3)); break;
        default: ptr = c.takeUint(4); break;
        }
        if (c.bad || qint64(ptr) >= c.size) {
            c.bad = true;
            return v;
        }
        // 跟指针：另起一个游标，绝不动调用方的 off（指针之后还有兄弟字段要接着读）。
        Cursor sub{c.p, c.size, qint64(ptr), false};
        Value target = decode(sub, depth + 1);
        if (sub.bad)
            c.bad = true;
        return target;
    }
    if (size == 29) {
        size = 29 + c.takeUint(1);
    } else if (size == 30) {
        size = 285 + c.takeUint(2);
    } else if (size == 31) {
        size = 65821 + c.takeUint(3);
    }
    if (c.bad)
        return v;

    switch (type) {
    case 2: // utf8
        if (!c.need(qint64(size)))
            return v;
        v.str = QByteArray(reinterpret_cast<const char *>(c.p + c.off), int(size));
        c.off += qint64(size);
        return v;
    case 5: case 6: case 9: case 10: case 8: // uint16/32/64/128/int32
        v.num = c.takeUint(int(qMin<quint64>(size, 8)));
        return v;
    case 7: // map
        for (quint64 i = 0; i < size && !c.bad; ++i) {
            skipValue(c, depth + 1); // key
            skipValue(c, depth + 1); // value
        }
        return v;
    case 11: // array
        for (quint64 i = 0; i < size && !c.bad; ++i)
            skipValue(c, depth + 1);
        return v;
    case 14: // boolean：值编码在 size 里，不占额外字节
        v.num = size;
        return v;
    default: // bytes / double / float / 其它：按 size 跳过
        if (!c.need(qint64(size)))
            return v;
        c.off += qint64(size);
        return v;
    }
}

struct Meta {
    quint64 nodeCount = 0;
    quint64 recordSize = 0;
    quint64 ipVersion = 0;
    quint64 majorVersion = 0;
    bool ok = false;
};

// 解顶层 map，挑出我们要的几个键。键必须是字面字符串（MaxMind 的 metadata 一向如此）；
// 万一某个键被指针编码，decode 会把它跟出来，一样能比对。
Meta parseMeta(const uchar *base, qint64 size)
{
    Meta m;
    Cursor c{base, size, 0, false};
    const uchar ctrl = c.take();
    if (c.bad || (ctrl >> 5) != 7)
        return m; // 顶层必须是 map
    quint64 n = ctrl & 0x1f;
    if (n == 29) n = 29 + c.takeUint(1);
    else if (n == 30) n = 285 + c.takeUint(2);
    else if (n == 31) n = 65821 + c.takeUint(3);
    if (c.bad)
        return m;

    for (quint64 i = 0; i < n && !c.bad; ++i) {
        const Value key = decode(c);
        const Value val = decode(c);
        if (c.bad)
            return m;
        if (key.str == "node_count") m.nodeCount = val.num;
        else if (key.str == "record_size") m.recordSize = val.num;
        else if (key.str == "ip_version") m.ipVersion = val.num;
        else if (key.str == "binary_format_major_version") m.majorVersion = val.num;
    }
    m.ok = !c.bad;
    return m;
}

// 读搜索树里第 node 个节点的第 idx(0=左,1=右) 条记录。record_size 只有 24/28/32 三种。
quint64 readRecord(const uchar *tree, quint64 node, int idx, quint64 recordSize)
{
    const qint64 bytesPerNode = qint64(recordSize) / 4; // = recordSize*2/8
    const uchar *n = tree + qint64(node) * bytesPerNode;
    switch (recordSize) {
    case 24:
        return (quint64(n[idx * 3]) << 16) | (quint64(n[idx * 3 + 1]) << 8) | n[idx * 3 + 2];
    case 28:
        // 中间那个字节被两条记录**对半分**：高 4 位属左、低 4 位属右。
        if (idx == 0)
            return (quint64(n[3] & 0xF0) << 20) | (quint64(n[0]) << 16) | (quint64(n[1]) << 8) | n[2];
        return (quint64(n[3] & 0x0F) << 24) | (quint64(n[4]) << 16) | (quint64(n[5]) << 8) | n[6];
    case 32:
        return (quint64(n[idx * 4]) << 24) | (quint64(n[idx * 4 + 1]) << 16)
                | (quint64(n[idx * 4 + 2]) << 8) | n[idx * 4 + 3];
    default:
        return 0;
    }
}

} // namespace

bool MmdbFile::validate(const QByteArray &data, QString *why)
{
    const auto fail = [why](const QString &msg) {
        if (why)
            *why = msg;
        return false;
    };

    if (data.size() < kMinBytes) {
        return fail(QStringLiteral("文件只有 %1 字节，远小于一份 GeoIP 库应有的体积"
                                   "（多半是错误页或截断的响应）")
                        .arg(data.size()));
    }

    // 在末尾 128 KiB 里找最后一次出现的 metadata 标记。
    const qint64 from = qMax<qint64>(0, qint64(data.size()) - kMetadataSearchWindow);
    const qsizetype markerPos = data.lastIndexOf(QByteArray(kMarker, kMarkerLen));
    if (markerPos < 0 || qint64(markerPos) < from)
        return fail(QStringLiteral("找不到 MaxMind DB 的 metadata 标记，这不是一份 mmdb"));

    const uchar *base = reinterpret_cast<const uchar *>(data.constData());
    const qint64 metaStart = markerPos + kMarkerLen;
    const Meta m = parseMeta(base + metaStart, data.size() - metaStart);
    if (!m.ok)
        return fail(QStringLiteral("metadata 段解不开（文件损坏）"));
    if (m.majorVersion != 2)
        return fail(QStringLiteral("不认识的 mmdb 格式主版本 %1").arg(m.majorVersion));
    if (m.recordSize != 24 && m.recordSize != 28 && m.recordSize != 32)
        return fail(QStringLiteral("非法的 record_size %1").arg(m.recordSize));
    if (m.nodeCount == 0)
        return fail(QStringLiteral("node_count 为 0，搜索树是空的"));
    if (m.ipVersion != 4 && m.ipVersion != 6)
        return fail(QStringLiteral("非法的 ip_version %1").arg(m.ipVersion));

    // 树尺寸必须与文件长度自洽。
    const qint64 treeSize = qint64(m.nodeCount) * (qint64(m.recordSize) / 4);
    if (treeSize <= 0 || treeSize + 16 >= markerPos) {
        return fail(QStringLiteral("metadata 声称搜索树有 %1 字节，但文件到 metadata 只有 %2 字节"
                                   "——文件被截断或 metadata 与内容对不上")
                        .arg(treeSize)
                        .arg(markerPos));
    }

    // ★ 数据段分隔符：树的正后方必须是 16 个零字节。真机上那份坏文件正是栽在这里 ——
    //   它在这个偏移上放的已经是数据段内容（"MM >\xe8 EGMyanmar "），说明 metadata 声称的
    //   node_count 比实际的树大，任何读者从此往下走都是在读垃圾。核心不报错，只是查什么都查不到。
    for (qint64 i = treeSize; i < treeSize + 16; ++i) {
        if (base[i] != 0) {
            return fail(QStringLiteral("搜索树末尾的 16 字节数据段分隔符不是全零"
                                       "（偏移 %1 处为 0x%2）——node_count 与实际树尺寸不符，"
                                       "这份库查任何 IP 都会返回空")
                            .arg(treeSize)
                            .arg(uint(base[i]), 2, 16, QLatin1Char('0')));
        }
    }

    // 探针：拿几个知名 IP 走一遍树。合法的终止记录只有两种可能 —— 等于 node_count（未收录），
    // 或落在 [node_count+16, node_count+16+数据段长度) 这个数据指针区间里。超出区间 =
    // 树本身是垃圾。这一步防的是「分隔符恰好对了、但树内容坏了」。
    const qint64 dataSize = markerPos - (treeSize + 16);
    const quint64 ptrLo = m.nodeCount + 16;
    const quint64 ptrHi = m.nodeCount + 16 + quint64(dataSize);
    quint64 node = 0;
    bool haveIpv4Start = true;
    if (m.ipVersion == 6) {
        // v6 库里的 IPv4 数据挂在 ::/96 子树下（各家读者都是先走 96 个 0 位再走 32 位）。
        for (int i = 0; i < 96 && haveIpv4Start; ++i) {
            if (node >= m.nodeCount) {
                haveIpv4Start = false; // 纯 v6 库：没有 v4 子树，跳过探针（不算失败）
                break;
            }
            node = readRecord(base, node, 0, m.recordSize);
        }
        if (node >= m.nodeCount)
            haveIpv4Start = false;
    }
    if (haveIpv4Start) {
        const quint64 ipv4Start = node;
        static const quint32 kProbes[] = {
            0x01010101u, // 1.1.1.1
            0x08080808u, // 8.8.8.8
            0xDF050505u, // 223.5.5.5
            0x72727272u, // 114.114.114.114
        };
        int resolved = 0;
        for (quint32 ip : kProbes) {
            quint64 n = ipv4Start;
            for (int i = 0; i < 32; ++i) {
                if (n >= m.nodeCount)
                    break;
                n = readRecord(base, n, int((ip >> (31 - i)) & 1), m.recordSize);
            }
            if (n == m.nodeCount)
                continue; // 未收录：合法
            if (n < m.nodeCount || n < ptrLo || n >= ptrHi) {
                return fail(QStringLiteral("搜索树内容异常：探测 IP 走到了非法记录值 %1"
                                           "（合法区间 [%2, %3)）")
                                .arg(n).arg(ptrLo).arg(ptrHi));
            }
            ++resolved;
        }
        if (resolved == 0) {
            return fail(QStringLiteral("四个知名公网 IP 一个都查不出记录，这份库对任何 IP 都不会"
                                       "生效（GEOIP 规则将全部失配）"));
        }
    }

    return true;
}

bool MmdbFile::stage(const QByteArray &data, const QString &target, QString *why)
{
    if (!validate(data, why))
        return false;

    const QString staged = target + QLatin1String(".new");
    QSaveFile out(staged);
    // QSaveFile：写临时文件 → commit() 时才原子改名。中途崩溃/断电留下的是临时文件，
    // 不会出现「半份 .new」被下次启动当成好文件换上去。
    if (!out.open(QIODevice::WriteOnly)) {
        if (why)
            *why = QStringLiteral("写不了 %1: %2").arg(staged, out.errorString());
        return false;
    }
    if (out.write(data) != data.size()) { // ★ 返回值必须看：短写是静默产生截断文件的主要来源
        if (why)
            *why = QStringLiteral("写入不完整: %1").arg(out.errorString());
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        if (why)
            *why = QStringLiteral("落盘失败: %1").arg(out.errorString());
        return false;
    }
    return true;
}

bool MmdbFile::applyStaged(const QString &target)
{
    const QString staged = target + QLatin1String(".new");
    if (!QFileInfo::exists(staged))
        return false;

    QFile in(staged);
    if (!in.open(QIODevice::ReadOnly)) {
        return false; // 读不了就先留着，下次启动再试（别删——可能只是被杀毒软件临时锁住）
    }
    const QByteArray data = in.readAll();
    in.close();

    QString why;
    if (!MmdbFile::validate(data, &why)) {
        // 坏的暂存文件直接删掉：留着只会每次启动都白读一遍，而且掩盖「该重新下载了」这件事。
        QFile::remove(staged);
        qWarning().noquote() << "MmdbFile: 暂存的 GeoIP 库校验不通过，已丢弃 —" << why;
        return false;
    }

    // 走到这里 target 一定没被 mmap（本函数只在核心启动前调用），可以安全替换。
    QFile::remove(target); // Windows 上 rename 不覆盖已存在的文件
    if (!QFile::rename(staged, target)) {
        qWarning().noquote() << "MmdbFile: 换上暂存的 GeoIP 库失败，路径:" << target;
        return false;
    }
    return true;
}

#pragma once

// GeoIP 数据库（MaxMind DB / Country.mmdb）的**校验 + 落盘**。
//
// 为什么要有这个文件（这不是防御性编程的洁癖）：
//   原先两处下载（AboutController::checkGeoip 启动时静默下、SettingsController::updateGeoip 手动
//   按钮）都是同一个写法 —— 判一下 reply->error()==NoError && !data.isEmpty()，然后
//   QIODevice::Truncate 直接盖到线上的 Country.mmdb 上，write() 的返回值都不看。于是：
//     · 半截 body / 代理返回的错误页 / CDN 截断，全都会被当成新数据库写下去；
//     · 覆盖是原地截断，好文件当场没了，没有后悔药；
//     · 更狠的是**核心不报错**——maxminddb 打得开、不 fatal，只是每次查询返回空。
//   真机上就这么坏过一次（2026-07-29 09:37）：GEOIP,CN 是 MATCH 之前最后一条、负责把国内流量
//   留在直连的规则，它静默失效之后，所有嗅探不出域名的裸 IP 目的地（推送/IM/游戏/部分 CDN）
//   全部落进 MATCH → 出海到美国节点。表现就是「国内国外都很慢」，而日志里一个字都没有。
//   核心那边 hitCount=0 / missCount=1019 是唯一的痕迹。
//
// 所以这里做两件事，缺一不可：
//   1) validate() —— 写之前先确认这坨字节确实是一份**结构自洽**的 MaxMind DB。
//   2) stage()    —— 校验通过也**不碰**线上文件，只写到 <target>.new；由 applyStaged() 在核心
//      启动前换上去。理由有两条：
//        · 核心把 Country.mmdb **mmap** 着，Windows 上原地替换会直接失败
//          （"a file with a user-mapped section open"）——实测过；
//        · 就算换成功了也没用 —— mmdb 在核心里是 sync.Once 加载的，不重启核心不会重读。
//      即：原地覆盖从来就没有「立刻生效」过，它唯一的效果就是有概率毁掉好文件。
//   3) validateFile() —— 上面两条只保证「以后不会再写坏」，管不到**已经**坏在用户目录里的那份。
//      而且旧的下载路径写坏文件时已经把 QSettings 的 geoip/lastPublished 记成了最新版本，
//      checkGeoip() 之后每次都在 `last == stamp && haveLocal` 处直接 return —— 坏库会一直用到
//      上游发下一个 release 为止。所以起核心前还要给线上那份做一次体检，坏了就退回 qrc 种子
//      并清掉版本戳（见 CoreController::start）。
#include <QByteArray>
#include <QString>

namespace MmdbFile {

// 结构性校验。只查 MaxMind DB 规范里的硬不变量，不解数据段内容：
//   · 体积下限（挡 HTML 错误页/空响应这类明显不是数据库的东西）
//   · 末尾的 metadata 段存在且能解出 node_count / record_size / ip_version / 格式主版本
//   · 由 node_count×record_size 算出的搜索树尺寸与文件长度自洽
//   · 搜索树末尾那 16 字节的数据段分隔符必须全零 ★ 真机那个坏文件正是栽在这一条
//   · 拿几个知名 IP 走一遍树，终止记录必须落在合法的数据指针区间内
// 通过返回 true；失败返回 false 并把人话原因写进 *why（可为 nullptr）。
bool validate(const QByteArray &data, QString *why = nullptr);

// 同 validate()，但校验的是磁盘上已有的一份库。文件不存在/读不出来一律算不通过。
// 给「起核心前体检线上 Country.mmdb」用（见 CoreController::start 的自愈分支）。
bool validateFile(const QString &path, QString *why = nullptr);

// 库的构建时刻（metadata 里的 build_epoch，Unix 秒）。读不出来返回 0。
//
// GeoIP 没有版本号，「你手上这份是哪天的」就是全部信息 —— 而这个问题**只能**问 build_epoch：
// 文件 mtime 说的是我们哪天把它写到盘上的，重装、换机、甚至一次失败重下都会把它刷成今天，
// 库本身却还是原来那份。设置页「GeoIP 数据」那一行显示的就是它。
qint64 buildEpoch(const QString &path);

// 校验 + 原子写到 <target>.new（QSaveFile：先写临时文件，commit 时才改名）。
// **不动 target 本身**——它可能正被运行中的核心 mmap 着。失败置 *why。
bool stage(const QByteArray &data, const QString &target, QString *why = nullptr);

// 核心启动前调用：若 <target>.new 存在，把它换成 target。返回是否真的换了。
// 换之前**再校验一遍**：暂存文件可能是上个版本留下的，或磁盘出过问题；这一次读盘（几 MB、
// 每次启核心一次）远比让核心带着一份坏 GeoIP 跑一整天便宜。校验不过就删掉暂存文件。
bool applyStaged(const QString &target);

} // namespace MmdbFile

#include "proto/OutboundRegistry.h"
#ifdef COAST_HAVE_QUIC
#include "proto/Hysteria2Outbound.h"
#endif
#include "ProxyConfig.h"
#include "../IOutbound.h"
#include <QCoreApplication>
#include <QTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <cstdio>
static bool testOne(const char* label, const ProxyNode& n){
  IOutboundTcp* ob = OutboundRegistry::instance().createTcp(n, nullptr);
  if(!ob){ fprintf(stderr,"%-7s FAIL: 未注册该 type (%s)\n",label,qUtf8Printable(n.type)); return false; }
  QEventLoop loop; QByteArray resp; bool est=false; QString err;
  QObject::connect(ob,&IOutboundTcp::established,[&]{ est=true; ob->write(QByteArray("GET / HTTP/1.1\r\nHost: www.baidu.com\r\nConnection: close\r\n\r\n")); });
  QObject::connect(ob,&IOutboundTcp::dataReceived,[&](const QByteArray&d){ resp+=d; if(resp.contains("HTTP/1.")) loop.quit(); });
  QObject::connect(ob,&IOutboundTcp::failed,[&](const QString&r){ err=r; loop.quit(); });
  QObject::connect(ob,&IOutboundTcp::closed,[&]{ loop.quit(); });
  // ★ 超时守卫必须用**栈上的 QTimer**，不能用 QTimer::singleShot：
  //   singleShot 挂在全局，用例提前成功后它并不会被取消，15 秒后会在**下一个用例**的事件循环里
  //   触发，去 quit 一个早已析构的 QEventLoop → 段错误。栈上的 QTimer 随本帧销毁而取消。
  //   (这个坑是在加 UDP 用例后才炸出来的：UDP 等得久，正好等到上一个用例的游离定时器。)
  QTimer guard; guard.setSingleShot(true);
  QObject::connect(&guard,&QTimer::timeout,[&]{ loop.quit(); });
  guard.start(15000);
  ob->connectTo("www.baidu.com",80,"");
  loop.exec();
  bool ok = resp.contains("HTTP/1.");
  fprintf(stderr,"%-7s %s  est=%d bytes=%d status='%s'%s\n",label, ok?"PASS":"FAIL", est,(int)resp.size(),
          resp.left(15).trimmed().constData(), err.isEmpty()?"":(QByteArray(" err=")+err.toUtf8()).constData());
  ob->deleteLater();
  return ok;
}
// UDP 用例：走出站的 UDP 中继(Hy2 用 QUIC 数据报)向一个 **DNS 服务器**发一条真查询，
// 收到「同 ID 且带应答」的回包才算通。选 DNS 而不是自建 echo，是因为它同时验了
// 「请求出得去」「回包能按源地址还原回来」两件事，且服务端侧不需要额外部署。
static bool testUdp(const char* label, const ProxyNode& n, const char* dnsIp){
  IOutboundUdp* ob = OutboundRegistry::instance().createUdp(n, nullptr);
  if(!ob){ fprintf(stderr,"%-7s FAIL: 未注册该 type 的 UDP (%s)\n",label,qUtf8Printable(n.type)); return false; }
  // 一条最小 DNS 查询：ID=0xC0A5, RD=1, QDCOUNT=1, QNAME=www.baidu.com, QTYPE=A, QCLASS=IN
  QByteArray q = QByteArray::fromHex("c0a5010000010000000000000377777705626169647503636f6d0000010001");
  QEventLoop loop; QByteArray resp; QString err; bool got=false;
  // ★ 发三次(0/300/900ms)：UDP 本就不可靠，且服务端的 UDP 会话管理器是认证成功后**另起协程**才跑起来的，
  //   紧贴认证响应发出的第一包有可能落在它启动之前而被丢掉。真实场景里 DNS/QUIC 客户端自带重试，
  //   这里照做，免得把「首包时序」误判成「UDP 不通」。
  QObject::connect(ob,&IOutboundUdp::ready,[&]{
    ob->sendTo(QHostAddress(QString(dnsIp)),53,q);
    QTimer::singleShot(300, ob, [&,ob]{ if(!got) ob->sendTo(QHostAddress(QString(dnsIp)),53,q); });
    QTimer::singleShot(900, ob, [&,ob]{ if(!got) ob->sendTo(QHostAddress(QString(dnsIp)),53,q); });
  });
  QObject::connect(ob,&IOutboundUdp::datagramReceived,[&](const QHostAddress&,quint16,const QByteArray&d){
    resp=d; got=true; loop.quit(); });
  QObject::connect(ob,&IOutboundUdp::failed,[&](const QString&r){ err=r; loop.quit(); });
  QObject::connect(ob,&IOutboundUdp::closed,[&]{ loop.quit(); });
  // ★ 超时守卫必须用**栈上的 QTimer**，不能用 QTimer::singleShot：
  //   singleShot 挂在全局，用例提前成功后它并不会被取消，15 秒后会在**下一个用例**的事件循环里
  //   触发，去 quit 一个早已析构的 QEventLoop → 段错误。栈上的 QTimer 随本帧销毁而取消。
  //   (这个坑是在加 UDP 用例后才炸出来的：UDP 等得久，正好等到上一个用例的游离定时器。)
  QTimer guard; guard.setSingleShot(true);
  QObject::connect(&guard,&QTimer::timeout,[&]{ loop.quit(); });
  guard.start(15000);
  ob->associate("");
  loop.exec();
  // 判据：回包 ID 与请求一致、QR=1(应答)、ANCOUNT>0
  const bool idOk = resp.size()>=12 && quint8(resp[0])==0xc0 && quint8(resp[1])==0xa5;
  const bool isResp = resp.size()>=12 && (quint8(resp[2])&0x80);
  const int ancount = resp.size()>=8 ? (quint8(resp[6])<<8|quint8(resp[7])) : 0;
  const bool ok = idOk && isResp && ancount>0;
  fprintf(stderr,"%-7s %s  got=%d bytes=%d id=%d qr=%d ans=%d%s\n",label, ok?"PASS":"FAIL",
          got,(int)resp.size(),idOk,isResp,ancount,
          err.isEmpty()?"":(QByteArray(" err=")+err.toUtf8()).constData());
  ob->deleteLater();
  return ok;
}

int main(int argc,char**argv){
  QCoreApplication app(argc,argv);
  ProxyNode ss; ss.name="ss"; ss.type="ss"; ss.server="127.0.0.1"; ss.port=18388; ss.cipher="aes-128-gcm"; ss.password="testpass123";
  ProxyNode vm; vm.name="vm"; vm.type="vmess"; vm.server="127.0.0.1"; vm.port=18389; vm.uuid="b831381d-6324-4d53-ad4f-8cda48b30811"; vm.cipher="auto";
  ProxyNode tj; tj.name="tj"; tj.type="trojan"; tj.server="127.0.0.1"; tj.port=18390; tj.password="trojanpass123";
  tj.sni="test.local"; tj.skipCertVerify=true; tj.tls=true;
  ProxyNode vl; vl.name="vl"; vl.type="vless"; vl.server="127.0.0.1"; vl.port=18391;
  vl.uuid="b831381d-6324-4d53-ad4f-8cda48b30811"; vl.sni="test.local"; vl.skipCertVerify=true; vl.tls=true;
  ProxyNode rl; rl.name="rl"; rl.type="reality"; rl.server="127.0.0.1"; rl.port=18392;
  rl.uuid="b831381d-6324-4d53-ad4f-8cda48b30811"; rl.sni="dl.google.com";
  rl.realityPublicKey="1l2LpjEwsgCjefaPQTMtpr9ni1g9oHkumdi1HdMoJWM"; rl.realityShortId="686c0ef0"; rl.fingerprint="chrome"; rl.tls=true;
  int fails=0;
  if(!testOne("SS",ss)) fails++;
  if(!testOne("VMESS",vm)) fails++;
  if(!testOne("TROJAN",tj)) fails++;
  if(!testOne("VLESS",vl)) fails++;
  if(!testOne("REALITY",rl)) fails++;
#ifdef COAST_HAVE_QUIC
  // 先跑纯函数 KAT（不碰网络）：Hy2 认证成败全靠从响应里解出 :status，那段 QPACK/Huffman 解码
  // 曾经因为常量表写错而在真机上炸过，所以每次跑 harness 都先钉一遍。
  { QString why; const bool ok = hysteria2QpackSelfTest(&why);
    fprintf(stderr,"%-7s %s%s\n","QPACK", ok?"PASS":"FAIL", ok?"":qUtf8Printable(QStringLiteral("  ")+why));
    if(!ok) fails++; }
  // QUIC 系参照服务端：
  //   · Hysteria2 → **官方 hysteria 服务端**(hy2srv.yaml, :18395)。**不能用 mihomo 当 hy2 服务端**，
  //     那条路被 msquic 的一个 off-by-one 判死（见 README「msquic 的 2^60 坑」）。
  //   · TUIC     → mihomo 的 tuic listener(nodesrv3.yaml, :18394)。
  // ★ Hysteria2 的认证串就是**纯密码**（不是 "用户名:密码"；写错时服务端返回伪装站的 404 而非 401）。
  ProxyNode h2; h2.name="h2"; h2.type="hysteria2"; h2.server="127.0.0.1"; h2.port=18395;
  h2.password="hy2pass123"; h2.sni="test.local"; h2.skipCertVerify=true;
  if(!testOne("HY2",h2)) fails++;
  // Hy2 的另外半边：UDP 中继(QUIC 数据报 + 分片)。DNS 服务器可用 argv[1] 覆盖。
  if(!testUdp("HY2UDP",h2, argc>1?argv[1]:"223.5.5.5")) fails++;
  ProxyNode tu; tu.name="tu"; tu.type="tuic"; tu.server="127.0.0.1"; tu.port=18394;
  tu.uuid="b831381d-6324-4d53-ad4f-8cda48b30811"; tu.password="tuicpass123";
  tu.sni="test.local"; tu.skipCertVerify=true;
# ifdef COAST_HAVE_QUIC_KEYING
  if(!testOne("TUIC",tu)) fails++;
# else
  fprintf(stderr,"%-7s SKIP  (msquic 无 keying exporter，需 v2.6+；token 无法导出)\n","TUIC");
# endif
#else
  fprintf(stderr,"HY2/TUIC SKIP  (构建时未找到 msquic)\n");
#endif
  fprintf(stderr,"== fails=%d ==\n",fails);
  return fails;
}

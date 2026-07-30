#include "proto/OutboundRegistry.h"
#ifdef COAST_HAVE_QUIC
#include "proto/Hysteria2Outbound.h"
#endif
#include "ProxyConfig.h"
#include "../IOutbound.h"
#include <QCoreApplication>
#include <QTimer>
#include <QEventLoop>
#include <cstdio>
static bool testOne(const char* label, const ProxyNode& n){
  IOutboundTcp* ob = OutboundRegistry::instance().createTcp(n, nullptr);
  if(!ob){ fprintf(stderr,"%-7s FAIL: 未注册该 type (%s)\n",label,qUtf8Printable(n.type)); return false; }
  QEventLoop loop; QByteArray resp; bool est=false; QString err;
  QObject::connect(ob,&IOutboundTcp::established,[&]{ est=true; ob->write(QByteArray("GET / HTTP/1.1\r\nHost: www.baidu.com\r\nConnection: close\r\n\r\n")); });
  QObject::connect(ob,&IOutboundTcp::dataReceived,[&](const QByteArray&d){ resp+=d; if(resp.contains("HTTP/1.")) loop.quit(); });
  QObject::connect(ob,&IOutboundTcp::failed,[&](const QString&r){ err=r; loop.quit(); });
  QObject::connect(ob,&IOutboundTcp::closed,[&]{ loop.quit(); });
  QTimer::singleShot(15000,[&]{ loop.quit(); });
  ob->connectTo("www.baidu.com",80,"");
  loop.exec();
  bool ok = resp.contains("HTTP/1.");
  fprintf(stderr,"%-7s %s  est=%d bytes=%d status='%s'%s\n",label, ok?"PASS":"FAIL", est,(int)resp.size(),
          resp.left(15).trimmed().constData(), err.isEmpty()?"":(QByteArray(" err=")+err.toUtf8()).constData());
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

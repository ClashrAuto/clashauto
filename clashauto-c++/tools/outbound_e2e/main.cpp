#include "proto/OutboundRegistry.h"
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
  fprintf(stderr,"== fails=%d ==\n",fails);
  if(!testOne("REALITY",rl)) fails++;
  return fails;
}

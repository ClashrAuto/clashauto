#!/usr/bin/env bash
# 一次性把 macOS 签名 + 公证所需的 5 个 GitHub secret 设进仓库。
#
# 由**你本人**运行:密码与 Issuer ID 是当场输入的,不写进任何文件、不留在命令历史里
# (read -s 不回显,变量只活在本进程内)。
#
#   bash scripts/setup_signing_secrets.sh
#
# 前置:
#   brew install gh && gh auth login      # 需要对该仓库有 admin 权限
#
# 不想装 gh 也行,网页上手工设同样的 5 个(Settings → Secrets and variables → Actions):
#   MACOS_CERT_P12       ← base64 -i ~/Downloads/证书.p12 | pbcopy
#   MACOS_CERT_PW        ← p12 密码
#   MACOS_NOTARY_KEY     ← base64 -i ~/Downloads/AuthKey_4K4L8A5ZU9.p8 | pbcopy
#   MACOS_NOTARY_KEY_ID  ← 4K4L8A5ZU9
#   MACOS_NOTARY_ISSUER  ← App Store Connect → Users and Access → Integrations 顶部的 UUID
set -euo pipefail

REPO="${REPO:-ClashrAuto/clashauto}"
P12="${P12:-$HOME/Downloads/证书.p12}"
P8="${P8:-$HOME/Downloads/AuthKey_4K4L8A5ZU9.p8}"
KEY_ID="${KEY_ID:-4K4L8A5ZU9}"   # 取自 p8 文件名 AuthKey_<KEYID>.p8

for f in "$P12" "$P8"; do
    [[ -f "$f" ]] || { echo "找不到:$f" >&2; exit 1; }
done

command -v gh >/dev/null || {
    echo "未安装 gh。先跑:  brew install gh && gh auth login" >&2
    echo "(或按本文件头部的说明在网页上手工设这 5 个 secret)" >&2
    exit 1
}
gh auth status >/dev/null 2>&1 || { echo "gh 未登录,先跑:  gh auth login" >&2; exit 1; }

# p12 密码:先本地验一次再上传 —— 密码错了的话 CI 要跑完整个构建才会失败在 security import,
# 那时排查成本比在这里多敲一次高得多。
read -r -s -p "p12 密码(不回显): " CERT_PW; echo
if ! openssl pkcs12 -in "$P12" -nokeys -passin pass:"$CERT_PW" -legacy >/dev/null 2>&1 \
   && ! openssl pkcs12 -in "$P12" -nokeys -passin pass:"$CERT_PW" >/dev/null 2>&1; then
    echo "密码不对(用它读不出证书),没有上传任何东西。" >&2
    exit 1
fi
echo "✅ 密码可用"

# Issuer ID:App Store Connect → Users and Access → Integrations 页面顶部的那个 UUID
read -r -p "App Store Connect Issuer ID (UUID): " ISSUER
[[ "$ISSUER" =~ ^[0-9a-fA-F-]{36}$ ]] || { echo "不像 UUID,已中止。" >&2; exit 1; }

echo "==> 写入 $REPO 的 secret"
base64 -i "$P12" | gh secret set MACOS_CERT_P12       --repo "$REPO"
printf '%s' "$CERT_PW" | gh secret set MACOS_CERT_PW  --repo "$REPO"
base64 -i "$P8"  | gh secret set MACOS_NOTARY_KEY     --repo "$REPO"
printf '%s' "$KEY_ID" | gh secret set MACOS_NOTARY_KEY_ID --repo "$REPO"
printf '%s' "$ISSUER" | gh secret set MACOS_NOTARY_ISSUER --repo "$REPO"

unset CERT_PW ISSUER
echo
echo "==> 已设置:"
gh secret list --repo "$REPO" | grep -E "MACOS_" || true
echo
echo "下一次 push 就会产出**已公证**的 Coast-<ver>-macos-universal.dmg。"
echo "想立刻验证而不改代码:git commit --allow-empty -m 'ci: 验证签名公证' && git push"

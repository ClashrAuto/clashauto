// 候选 B —— gVisor netstack（tailscale / sing-box / hysteria 都在用的那套）。
//
// 与候选 A(lwip2socks) 严格同拓扑同接口：TAP 收以太帧 → 栈终结 TCP →
// 用内核 socket 连 127.0.0.1:<原始目的端口> → 双向搬字节。
// 唯一的差别就是中间那个栈，这样两边的数才可比。
//
// catch-all 靠 promiscuous + spoofing：NIC 收下发往任意 IP 的包，
// 路由表一条 0.0.0.0/0 → NIC1 兜住回程。对应 lwIP 那边的 accept-all 补丁。
//
// 用法: gvnet2socks <tap-if> <stack-ip> <netmask>
package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"runtime"
	"strconv"
	"syscall"
	"unsafe"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/fdbased"
	"gvisor.dev/gvisor/pkg/tcpip/network/arp"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/waiter"
)

const nicID = 1

var mac = tcpip.LinkAddress([]byte{0x02, 0x00, 0x5b, 0x00, 0x00, 0x02})

// TAP 打开：与 lwip2socks.c 的 tap_open() 等价（IFF_TAP|IFF_NO_PI，挂到已存在的持久 TAP）
func tapOpen(name string) (int, error) {
	fd, err := syscall.Open("/dev/net/tun", syscall.O_RDWR, 0)
	if err != nil {
		return -1, err
	}
	var req struct {
		name  [16]byte
		flags uint16
		_     [22]byte
	}
	copy(req.name[:], name)
	req.flags = syscall.IFF_TAP | syscall.IFF_NO_PI
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd),
		uintptr(syscall.TUNSETIFF), uintptr(unsafe.Pointer(&req))); e != 0 {
		syscall.Close(fd)
		return -1, e
	}
	return fd, nil
}

// 双向搬字节。用 io.Copy —— Go 侧会自动走 splice/sendfile 是不可能的（一端是 netstack
// 的用户态 endpoint），所以这里就是普通的 read/write 循环，和 C 那边的语义一致。
func pipe(a net.Conn, b net.Conn) {
	done := make(chan struct{}, 2)
	go func() { io.Copy(a, b); a.(interface{ CloseWrite() error }).CloseWrite(); done <- struct{}{} }()
	go func() { io.Copy(b, a); b.(interface{ CloseWrite() error }).CloseWrite(); done <- struct{}{} }()
	<-done
	<-done
	a.Close()
	b.Close()
}

func maskLen(m string) int {
	ip := net.ParseIP(m).To4()
	if ip == nil {
		return 24
	}
	ones, _ := net.IPMask(ip).Size()
	return ones
}

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: gvnet2socks <tap> <ip> <mask>")
		os.Exit(2)
	}
	tapName, ipStr, maskStr := os.Args[1], os.Args[2], os.Args[3]

	fd, err := tapOpen(tapName)
	if err != nil {
		fmt.Fprintln(os.Stderr, "tap:", err)
		os.Exit(1)
	}

	s := stack.New(stack.Options{
		NetworkProtocols: []stack.NetworkProtocolFactory{
			ipv4.NewProtocol, ipv6.NewProtocol, arp.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol},
	})

	ep, err := fdbased.New(&fdbased.Options{
		FDs:                []int{fd},
		MTU:                1500,
		EthernetHeader:     true,
		Address:            mac,
		PacketDispatchMode: fdbased.Readv, // TAP 是字符设备不是 socket，RecvMMsg 用不了
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "fdbased:", err)
		os.Exit(1)
	}
	if e := s.CreateNIC(nicID, ep); e != nil {
		fmt.Fprintln(os.Stderr, "CreateNIC:", e)
		os.Exit(1)
	}

	addr := tcpip.AddrFromSlice(net.ParseIP(ipStr).To4())
	if e := s.AddProtocolAddress(nicID, tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: tcpip.AddressWithPrefix{Address: addr, PrefixLen: maskLen(maskStr)},
	}, stack.AddressProperties{}); e != nil {
		fmt.Fprintln(os.Stderr, "AddProtocolAddress:", e)
		os.Exit(1)
	}
	// ★ catch-all 的两个开关（对应 lwIP 的 accept-all 补丁）：
	//   Promiscuous  = 收下目的 MAC 不是我的帧
	//   Spoofing     = 允许以「不是本 NIC 地址」的源地址回包（回程 SYN-ACK 要用原始目的 IP）
	s.SetPromiscuousMode(nicID, true)
	s.SetSpoofing(nicID, true)
	s.SetRouteTable([]tcpip.Route{
		{Destination: header.IPv4EmptySubnet, NIC: nicID},
		{Destination: header.IPv6EmptySubnet, NIC: nicID},
	})

	fwd := tcp.NewForwarder(s, 0, 4096, func(r *tcp.ForwarderRequest) {
		id := r.ID()
		var wq waiter.Queue
		lep, e := r.CreateEndpoint(&wq)
		if e != nil {
			r.Complete(true)
			return
		}
		r.Complete(false)
		down := gonet.NewTCPConn(&wq, lep)
		go func() {
			up, err := net.Dial("tcp", "127.0.0.1:"+strconv.Itoa(int(id.LocalPort)))
			if err != nil {
				down.Close()
				return
			}
			if t, ok := up.(*net.TCPConn); ok {
				t.SetNoDelay(true)
			}
			pipe(down, up)
		}()
	})
	s.SetTransportProtocolHandler(tcp.ProtocolNumber, fwd.HandlePacket)

	fmt.Fprintf(os.Stderr, "gvnet2socks up on %s ip=%s gomaxprocs=%d\n",
		tapName, ipStr, runtime.GOMAXPROCS(0))
	select {}
}

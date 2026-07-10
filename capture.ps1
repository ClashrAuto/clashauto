param(
  [int]$Page = 0,          # 0=status 1=subscriptions 2=settings 3=logs 4=about
  [string]$Out = "C:\Users\ultra\Desktop\clashauto\shot.png",
  [switch]$NoClick         # just capture current page
)
Add-Type -AssemblyName System.Drawing
$sig = @"
using System;
using System.Runtime.InteropServices;
public class Cap {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x,int y,int cx,int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
Add-Type $sig
$TOP=[IntPtr](-1); $NOTOP=[IntPtr](-2); $SWP=0x0003
$h = (Get-Process clashauto-cpp -ErrorAction SilentlyContinue | Select-Object -First 1).MainWindowHandle
if(-not $h){ "NO WINDOW"; exit 1 }

# robust foreground: attach to foreground thread's input queue
[void][Cap]::ShowWindow($h, 9)
$fg = [Cap]::GetForegroundWindow()
$tid1 = [Cap]::GetWindowThreadProcessId($fg, [ref]([uint32]0))
$tid2 = [Cap]::GetCurrentThreadId()
[void][Cap]::AttachThreadInput($tid2, $tid1, $true)
[void][Cap]::SetWindowPos($h,$TOP,0,0,0,0,$SWP)
[void][Cap]::BringWindowToTop($h)
[void][Cap]::SetForegroundWindow($h)
[void][Cap]::AttachThreadInput($tid2, $tid1, $false)
Start-Sleep -Milliseconds 400

if(-not $NoClick){
  $r = New-Object Cap+RECT; [void][Cap]::GetWindowRect($h,[ref]$r)
  $x = $r.L + 55
  $yTarget = $r.T + [int](181 + $Page * 45.25)
  $yPrime  = $r.T + [int](181 + (($Page + 2) % 5) * 45.25)  # a different menu item to absorb activation
  # priming click (first input event is swallowed for window activation)
  [void][Cap]::SetCursorPos($x,$yPrime); Start-Sleep -Milliseconds 130
  [void][Cap]::mouse_event(0x02,0,0,0,[IntPtr]::Zero); [void][Cap]::mouse_event(0x04,0,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 450
  # real click on the target page
  [void][Cap]::SetCursorPos($x,$yTarget); Start-Sleep -Milliseconds 130
  [void][Cap]::mouse_event(0x02,0,0,0,[IntPtr]::Zero); [void][Cap]::mouse_event(0x04,0,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 700
}
# move cursor off the menu so no hover highlight bleeds in
$r2 = New-Object Cap+RECT; [void][Cap]::GetWindowRect($h,[ref]$r2)
[void][Cap]::SetCursorPos($r2.R-20, $r2.T+8); Start-Sleep -Milliseconds 200
$w=$r2.R-$r2.L; $ht=$r2.B-$r2.T
$bmp = New-Object System.Drawing.Bitmap($w,$ht)
$g=[System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r2.L,$r2.T,0,0,$bmp.Size)
$bmp.Save($Out); $g.Dispose(); $bmp.Dispose()
[void][Cap]::SetWindowPos($h,$NOTOP,0,0,0,0,$SWP)
"SAVED $Out page=$Page ($w x $ht)"

' Dima Wiki 启动器：
' 1) 用 WMI 探测是否已有 http.server 8931 在跑，没有则启动隐藏窗口的本地静态服务
'    （仅监听 127.0.0.1:8931，服务内容 = wiki 构建产物，不出本机）
' 2) 打开浏览器访问 http://localhost:8931/
Option Explicit
Dim sh, wmi, procs, p, busy
Set sh = CreateObject("WScript.Shell")
busy = False
On Error Resume Next
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set procs = wmi.ExecQuery("SELECT CommandLine FROM Win32_Process WHERE Name LIKE 'python%' AND CommandLine LIKE '%http.server 8931%'")
For Each p In procs
  If InStr(p.CommandLine & "", "http.server 8931") > 0 Then busy = True
Next
Err.Clear
On Error GoTo 0
If Not busy Then
  sh.Run """C:\Users\master\AppData\Local\Microsoft\WindowsApps\python.exe"" -m http.server 8931 --bind 127.0.0.1 --directory ""E:\freertos\H743_FreeRTOS\wiki\.vitepress\dist""", 0, False
  WScript.Sleep 1500
End If
sh.Run "http://localhost:8931/", 1, False

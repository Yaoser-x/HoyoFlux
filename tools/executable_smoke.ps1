[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
if ([IO.Path]::GetExtension($exePath) -ne '.exe') {
    throw "smoke target is not an EXE: $exePath"
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public sealed class HoyoFluxSmokeResult
{
    public int ExitCode { get; set; }
    public bool TokenIsElevated { get; set; }
    public string UserSid { get; set; }
    public string ProfileDirectory { get; set; }
}

public static class HoyoFluxSmokeNative
{
    private const uint LogonWithProfile = 0x00000001;
    private const uint CreateUnicodeEnvironment = 0x00000400;
    private const uint ProcessQueryLimitedInformation = 0x00001000;
    private const uint TokenQuery = 0x00000008;
    private const uint WaitObject0 = 0x00000000;
    private const uint WaitTimeout = 0x00000102;

    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfo
    {
        public uint cb;
        public IntPtr lpReserved;
        public IntPtr lpDesktop;
        public IntPtr lpTitle;
        public uint dwX;
        public uint dwY;
        public uint dwXSize;
        public uint dwYSize;
        public uint dwXCountChars;
        public uint dwYCountChars;
        public uint dwFillAttribute;
        public uint dwFlags;
        public ushort wShowWindow;
        public ushort cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public uint dwProcessId;
        public uint dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TokenElevation
    {
        public uint TokenIsElevated;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SidAndAttributes
    {
        public IntPtr Sid;
        public uint Attributes;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessWithLogonW(
        string userName, string domain, string password, uint logonFlags,
        string applicationName, StringBuilder commandLine, uint creationFlags,
        IntPtr environment, string currentDirectory,
        ref StartupInfo startupInfo, out ProcessInformation processInformation);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(
        IntPtr token, int informationClass, out TokenElevation information,
        uint informationLength, out uint returnLength);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(
        IntPtr token, int informationClass, IntPtr information,
        uint informationLength, out uint returnLength);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ConvertSidToStringSidW(IntPtr sid, out IntPtr stringSid);

    [DllImport("userenv.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetUserProfileDirectoryW(
        IntPtr token, StringBuilder profileDirectory, ref uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    private static string SidFromToken(IntPtr token)
    {
        uint required = 0;
        GetTokenInformation(token, 1, IntPtr.Zero, 0, out required);
        if (required == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "TokenUser size query failed");

        IntPtr buffer = Marshal.AllocHGlobal((int)required);
        try
        {
            if (!GetTokenInformation(token, 1, buffer, required, out required))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "TokenUser query failed");
            SidAndAttributes user = Marshal.PtrToStructure<SidAndAttributes>(buffer);
            IntPtr text = IntPtr.Zero;
            if (!ConvertSidToStringSidW(user.Sid, out text))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertSidToStringSidW failed");
            try { return Marshal.PtrToStringUni(text); }
            finally { LocalFree(text); }
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    private static string ProfileFromToken(IntPtr token)
    {
        uint required = 0;
        GetUserProfileDirectoryW(token, null, ref required);
        if (required == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "profile path size query failed");
        StringBuilder path = new StringBuilder((int)required);
        if (!GetUserProfileDirectoryW(token, path, ref required))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetUserProfileDirectoryW failed");
        return path.ToString();
    }

    private static HoyoFluxSmokeResult InspectAndWait(
        IntPtr process, int timeoutMilliseconds)
    {
        IntPtr token = IntPtr.Zero;
        try
        {
            if (!OpenProcessToken(process, TokenQuery, out token))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken failed");
            TokenElevation elevation;
            uint returned;
            if (!GetTokenInformation(token, 20, out elevation,
                                     (uint)Marshal.SizeOf(typeof(TokenElevation)), out returned))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "TokenElevation query failed");

            string sid = SidFromToken(token);
            string profile = ProfileFromToken(token);
            uint wait = WaitForSingleObject(process, (uint)timeoutMilliseconds);
            if (wait == WaitTimeout)
            {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
                throw new TimeoutException("smoke process did not exit within the timeout");
            }
            if (wait != WaitObject0)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WaitForSingleObject failed");
            uint exitCode;
            if (!GetExitCodeProcess(process, out exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "GetExitCodeProcess failed");

            return new HoyoFluxSmokeResult {
                ExitCode = unchecked((int)exitCode),
                TokenIsElevated = elevation.TokenIsElevated != 0,
                UserSid = sid,
                ProfileDirectory = profile
            };
        }
        finally { if (token != IntPtr.Zero) CloseHandle(token); }
    }

    public static HoyoFluxSmokeResult RunAsUser(
        string userName, string domain, string password, string executable,
        string workingDirectory, int timeoutMilliseconds, IntPtr environment)
    {
        StartupInfo startup = new StartupInfo();
        startup.cb = (uint)Marshal.SizeOf(typeof(StartupInfo));
        ProcessInformation process;
        StringBuilder commandLine = new StringBuilder("\"" + executable + "\"");
        uint flags = environment == IntPtr.Zero ? 0 : CreateUnicodeEnvironment;
        if (!CreateProcessWithLogonW(userName, domain, password, LogonWithProfile,
                                     executable, commandLine, flags, environment,
                                     workingDirectory, ref startup, out process))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessWithLogonW failed");
        try { return InspectAndWait(process.hProcess, timeoutMilliseconds); }
        finally
        {
            if (process.hThread != IntPtr.Zero) CloseHandle(process.hThread);
            if (process.hProcess != IntPtr.Zero) CloseHandle(process.hProcess);
        }
    }

    public static string GetProcessUserSid(int processId)
    {
        IntPtr process = OpenProcess(ProcessQueryLimitedInformation, false, (uint)processId);
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            IntPtr token = IntPtr.Zero;
            try
            {
                if (!OpenProcessToken(process, TokenQuery, out token))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                return SidFromToken(token);
            }
            finally { if (token != IntPtr.Zero) CloseHandle(token); }
        }
        finally { CloseHandle(process); }
    }

    public static int StopProcessesForSid(string expectedSid)
    {
        int stopped = 0;
        int currentProcessId = Process.GetCurrentProcess().Id;
        foreach (Process process in Process.GetProcesses())
        {
            try
            {
                if (process.Id != currentProcessId &&
                    String.Equals(GetProcessUserSid(process.Id), expectedSid,
                                  StringComparison.OrdinalIgnoreCase))
                {
                    process.Kill();
                    stopped++;
                }
            }
            catch { }
            finally { process.Dispose(); }
        }
        return stopped;
    }
}
'@

$originalPath = $env:PATH
$testUser = $null
$testUserSid = $null
$profileDirectory = $null
$smokeRoot = $null
$userCreated = $false
$profileNamePrefix = 'hf-smoke-'
$userNamePrefix = 'hfs-'
$isElevatedRunner = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isElevatedRunner) {
    throw 'This release smoke test must run on an isolated administrator Windows runner so it can create a temporary standard user.'
}

try {
    $testUser = $userNamePrefix + [guid]::NewGuid().ToString('N').Substring(0, 12)
    $password = 'Hf9!' + [guid]::NewGuid().ToString('N') + 'xZ2@'
    $net = Join-Path $env:SystemRoot 'System32/net.exe'
    $createOutput = & $net user $testUser $password /add 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "failed to create temporary standard user: $($createOutput -join ' ')"
    }
    $userCreated = $true
    $testUserSid = ([Security.Principal.NTAccount]::new(
        $env:COMPUTERNAME, $testUser
    )).Translate([Security.Principal.SecurityIdentifier]).Value
    $smokeParent = Join-Path $env:PUBLIC 'Documents'
    New-Item -ItemType Directory -Force -Path $smokeParent | Out-Null

    $smokeRoot = Join-Path $smokeParent ($profileNamePrefix + [guid]::NewGuid().ToString('N') + ' 中文 path')
    New-Item -ItemType Directory -Path $smokeRoot | Out-Null
    $acl = Get-Acl -LiteralPath $smokeRoot
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new($testUserSid),
        [Security.AccessControl.FileSystemRights]::Modify,
        [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
            [Security.AccessControl.InheritanceFlags]::ObjectInherit,
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Allow
    )
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $smokeRoot -AclObject $acl

    $smokeExe = Join-Path $smokeRoot 'hoyoflux.exe'
    Copy-Item -LiteralPath $exePath -Destination $smokeExe
    $env:PATH = Join-Path $env:SystemRoot 'System32'

    $passwordSecure = ConvertTo-SecureString -String $password -AsPlainText -Force
    $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($passwordSecure)
    try { $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer) }

    $whoami = Join-Path $env:SystemRoot 'System32/whoami.exe'
    $bootstrap = [HoyoFluxSmokeNative]::RunAsUser(
        $testUser, $env:COMPUTERNAME, $plainPassword, $whoami,
        $env:SystemRoot, 15000, [IntPtr]::Zero
    )
    if ($bootstrap.ExitCode -ne 0 -or $bootstrap.TokenIsElevated -or
        $bootstrap.UserSid -ne $testUserSid) {
        throw 'temporary account bootstrap did not produce the expected standard-user token'
    }
    $profileDirectory = $bootstrap.ProfileDirectory
    $legacyRoot = Join-Path $profileDirectory 'AppData/Local/HoyoFlux'
    if (Test-Path -LiteralPath $legacyRoot) {
        throw "temporary test account unexpectedly has legacy AppData: $legacyRoot"
    }

    $machineEnvironment = [Environment]::GetEnvironmentVariables(
        [EnvironmentVariableTarget]::Machine
    )
    $environment = @{}
    foreach ($key in $machineEnvironment.Keys) { $environment[[string]$key] = [string]$machineEnvironment[$key] }
    $drive = [IO.Path]::GetPathRoot($profileDirectory).TrimEnd('\')
    $homePath = $profileDirectory.Substring($drive.Length)
    $localData = Join-Path $profileDirectory 'AppData/Local'
    $roamingData = Join-Path $profileDirectory 'AppData/Roaming'
    $tempPath = Join-Path $localData 'Temp'
    New-Item -ItemType Directory -Force -Path $tempPath | Out-Null
    $environment['PATH'] = Join-Path $env:SystemRoot 'System32'
    $environment['SystemRoot'] = $env:SystemRoot
    $environment['windir'] = $env:SystemRoot
    $environment['COMSPEC'] = Join-Path $env:SystemRoot 'System32/cmd.exe'
    $environment['USERNAME'] = $testUser
    $environment['USERDOMAIN'] = $env:COMPUTERNAME
    $environment['USERPROFILE'] = $profileDirectory
    $environment['HOMEDRIVE'] = $drive
    $environment['HOMEPATH'] = $homePath
    $environment['APPDATA'] = $roamingData
    $environment['LOCALAPPDATA'] = $localData
    $environment['TEMP'] = $tempPath
    $environment['TMP'] = $tempPath
    $envLines = @($environment.Keys | Sort-Object | ForEach-Object { "$_=$($environment[$_])" })
    $environmentBlock = [string]::Join([string][char]0, [string[]]$envLines) + [string][char]0 + [string][char]0
    $environmentPointer = [Runtime.InteropServices.Marshal]::StringToHGlobalUni($environmentBlock)
    try {
        $firstRun = [HoyoFluxSmokeNative]::RunAsUser(
            $testUser, $env:COMPUTERNAME, $plainPassword, $smokeExe,
            $smokeRoot, 30000, $environmentPointer
        )
        $profileDirectory = $firstRun.ProfileDirectory
        $configPath = Join-Path $smokeRoot 'config.toml'
        if (-not (Test-Path -LiteralPath $configPath)) {
            throw "first EXE launch did not create config.toml (exit=$($firstRun.ExitCode))"
        }
        if ($firstRun.ExitCode -ne 0) {
            throw "first EXE launch returned exit code $($firstRun.ExitCode)"
        }

        $config = [IO.File]::ReadAllText($configPath, [Text.Encoding]::UTF8)
        if (-not $config.Contains('action = "launch"')) {
            throw 'first-run config does not contain the expected launch action'
        }
        $config = $config.Replace('action = "launch"', 'action = "diagnose"')
        [IO.File]::WriteAllText($configPath, $config, [Text.UTF8Encoding]::new($false))
        $diagnoseRun = [HoyoFluxSmokeNative]::RunAsUser(
            $testUser, $env:COMPUTERNAME, $plainPassword, $smokeExe,
            $smokeRoot, 30000, $environmentPointer
        )
    } finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($environmentPointer)
    }

    foreach ($run in @($firstRun, $diagnoseRun)) {
        if ($null -eq $run -or $run.TokenIsElevated -or $run.UserSid -ne $testUserSid) {
            throw 'EXE did not run with the expected non-elevated user token'
        }
        if ($run.ExitCode -ne 0) { throw "EXE smoke process exited with code $($run.ExitCode)" }
    }

    $reportPath = Join-Path $smokeRoot 'data/diagnostics.txt'
    if (-not (Test-Path -LiteralPath $reportPath)) { throw 'diagnose EXE launch did not create diagnostics.txt' }
    $report = [IO.File]::ReadAllText($reportPath, [Text.Encoding]::UTF8)
    if (-not $report.Contains("HoyoFlux 诊断报告 $ExpectedVersion")) {
        throw "diagnostic report version does not match $ExpectedVersion"
    }
    $legacyRoot = Join-Path $profileDirectory 'AppData/Local/HoyoFlux'
    if (Test-Path -LiteralPath $legacyRoot) { throw 'EXE launch created AppData state' }
    Write-Output "Real EXE first-run and diagnose smoke passed as standard user $testUserSid."
} finally {
    $env:PATH = $originalPath
    if ($testUserSid) {
        [void][HoyoFluxSmokeNative]::StopProcessesForSid($testUserSid)
    }
    if ($userCreated) {
        $net = Join-Path $env:SystemRoot 'System32/net.exe'
        $deleteOutput = & $net user $testUser /delete 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not remove smoke user ${testUser}: $($deleteOutput -join ' ')" }
    }
    if ($userCreated -and $profileDirectory) {
        $profileFull = [IO.Path]::GetFullPath($profileDirectory).TrimEnd('\')
        $usersRoot = [IO.Path]::GetFullPath((Join-Path $env:SystemDrive 'Users')).TrimEnd('\') + '\'
        if ($profileFull.StartsWith($usersRoot, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path $profileFull -Leaf).Equals($testUser, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $profileFull -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    if ($smokeRoot) {
        $rootFull = [IO.Path]::GetFullPath($smokeRoot).TrimEnd('\')
        $publicDocs = [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'Documents')).TrimEnd('\') + '\'
        if ($rootFull.StartsWith($publicDocs, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path $rootFull -Leaf).StartsWith($profileNamePrefix, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $rootFull -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

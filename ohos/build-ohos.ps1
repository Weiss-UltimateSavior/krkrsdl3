<#
.SYNOPSIS
    krkrsdl3 OpenHarmony 构建入口

.DESCRIPTION
    用法: .\build-ohos.ps1 <command> [-Arch arm64-v8a] [-BuildType debug]

    命令:
      full      全量构建 (deps �?native �?hap)
      deps      交叉编译依赖 (含预编译复用)
      native    配置 CMake + 编译 libkrkrsdl3.so
      hap       打包 HAP
      clean     清理 out/ohos

.PARAMETER Arch
    目标架构: arm64-v8a (默认) | armeabi-v7a | x86_64
#>

param(
    [Parameter(Position=0)]
    [ValidateSet("full","deps","native","libs","hap","clean")]
    [string]$Command = "full",

    [Parameter(Position=1)]
    [string]$Arch = "arm64-v8a",

    [Parameter(Position=2)]
    [string]$BuildType = "debug"
)

$ErrorActionPreference = "Continue"
$Root = "$PSScriptRoot\.."
$SDK = "C:\D\openharmony\20\native"
$BuildDir = "$Root\out\ohos\$Arch"
$DepsDst = "$Root\out\ohos\deps\install\$Arch"
$DepsSrc = "$Root\out\ohos\deps\src"
$Prebuild = "$Root\ohos\prebuild"
$LogDir = "$Root\out\ohos\logs"
$CC = "$SDK\llvm\bin\clang.exe"
$AR = "$SDK\llvm\bin\llvm-ar.exe"
$Sysroot = "$SDK\sysroot"
$Toolchain = "$SDK\build\cmake\ohos.toolchain.cmake"
$Triple = if ($Arch -eq "arm64-v8a") { "aarch64-linux-ohos" }
           elseif ($Arch -eq "armeabi-v7a") { "arm-linux-ohos" }
           else { "x86_64-linux-ohos" }

# ================================================================
#  BUILD COMMANDS
# ================================================================

function Invoke-Deps {
    Write-Host "=== Building dependencies ==="
    if (Test-Path "$DepsDst\.stamp") {
        Write-Host "  Already built. (remove $DepsDst\.stamp to force)"
        return
    }
    New-Item -ItemType Directory -Force -Path "$DepsSrc","$DepsDst\lib","$DepsDst\include" | Out-Null

    function _git($name, $url, $branch) {
        $d = "$DepsSrc\$name"
        if (-not (Test-Path $d)) { $null = git clone -q --depth 1 --branch $branch $url $d 2>&1 }
    }
    function _cmake($name, $extra) {
        Push-Location "$DepsSrc\$name"
        if (Test-Path build) { Remove-Item -Force -Recurse build }
        $a = @("-G","Ninja","-DCMAKE_POLICY_VERSION_MINIMUM=3.5","-DCMAKE_TOOLCHAIN_FILE=$Toolchain","-DOHOS_ARCH=$Arch","-DCMAKE_BUILD_TYPE=Release","-DBUILD_SHARED_LIBS=OFF","-DCMAKE_INSTALL_PREFIX=$DepsDst","-B","build","-S",".")
        if ($extra) { $a += $extra -split ' ' | Where-Object { $_ } }
        $null = & cmake $a 2>&1; if ($LASTEXITCODE) { throw "cmake" }
        $null = cmake --build build --config Release --parallel 2>&1; if ($LASTEXITCODE) { throw "build" }
        $null = cmake --install build --prefix $DepsDst 2>&1; if ($LASTEXITCODE) { throw "install" }
        Pop-Location
    }
    function _cp($files) {
        Copy-Item -Force $files -Destination "$DepsDst\lib\" 2>$null
    }
    function _cpInc($srcDir) {
        if (Test-Path $srcDir) { Copy-Item -Recurse -Force "$srcDir\*" "$DepsDst\include\" 2>$null }
    }

    # --- zlib ---
    Write-Host "[zlib]"
    _git zlib https://github.com/madler/zlib.git v1.3.1
    Push-Location "$DepsSrc\zlib"
    $null = & $CC --target=$Triple --sysroot=$Sysroot -fPIC -D__MUSL__ -O2 -c adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c 2>&1
    & $AR rcs "$DepsDst\lib\libz.a" *.o
    Copy-Item zlib.h,zconf.h "$DepsDst\include\" -Force
    Pop-Location

    # --- libpng ---
    Write-Host "[libpng]"
    _git libpng https://github.com/pnggroup/libpng.git v1.6.43
    Push-Location "$DepsSrc\libpng"
    Copy-Item scripts\pnglibconf.h.prebuilt pnglibconf.h -Force
    $null = & $CC --target=$Triple --sysroot=$Sysroot -fPIC -D__MUSL__ -O2 "-I$DepsDst\include" -c png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c pngwtran.c pngwutil.c 2>&1
    & $AR rcs "$DepsDst\lib\libpng16.a" *.o
    Copy-Item png.h,pngconf.h,pnglibconf.h "$DepsDst\include\" -Force
    Pop-Location

    # --- libjpeg-turbo ---
    Write-Host "[libjpeg-turbo]"
    _git libjpeg-turbo https://github.com/libjpeg-turbo/libjpeg-turbo.git 3.0.4
    _cmake libjpeg-turbo "-DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON"

    # --- libogg ---
    Write-Host "[libogg]"
    _git ogg https://github.com/xiph/ogg.git v1.3.5
    _cmake ogg ""

    # --- opus ---
    Write-Host "[opus]"
    _git opus https://github.com/xiph/opus.git v1.5.2
    _cmake opus ""

    # --- libvorbis ---
    Write-Host "[libvorbis]"
    _git vorbis https://github.com/xiph/vorbis.git v1.3.7
    _cmake vorbis "-DOGG_INCLUDE_DIR=$DepsDst\include -DOGG_LIBRARY=$DepsDst\lib\libogg.a"

    # --- opusfile ---
    Write-Host "[opusfile]"
    _git opusfile https://github.com/xiph/opusfile.git v0.12
    Push-Location "$DepsSrc\opusfile"
    $null = & $CC --target=$Triple --sysroot=$Sysroot -fPIC -D__MUSL__ -O2 -Iinclude "-I$DepsDst\include" "-I$DepsDst\include\opus" -c src/opusfile.c src/stream.c src/http.c src/internal.c src/info.c src/wincerts.c 2>&1
    & $AR rcs "$DepsDst\lib\libopusfile.a" *.o
    Copy-Item include\opusfile.h "$DepsDst\include\" -Force
    Pop-Location

    # --- freetype ---
    Write-Host "[freetype]"
    _git freetype https://github.com/freetype/freetype.git VER-2-13-2
    _cmake freetype "-DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DPNG_ROOT=$DepsDst -DZLIB_ROOT=$DepsDst"

    # --- libwebp ---
    Write-Host "[libwebp]"
    _git libwebp https://github.com/webmproject/libwebp.git v1.4.0
    _cmake libwebp ""

    # --- oniguruma ---
    Write-Host "[oniguruma]"
    _git oniguruma https://github.com/kkos/oniguruma.git v6.9.9
    _cmake oniguruma ""

    # --- glm (header-only) ---
    Write-Host "[glm]"
    _git glm https://github.com/g-truc/glm.git 1.0.1
    Copy-Item -Recurse "$DepsSrc\glm\glm" "$DepsDst\include\glm" -Force

    # --- ffmpeg (prebuilt) ---
    Write-Host "[ffmpeg]"
    $ff = "$Prebuild\ffmpeg\$Arch"
    if (Test-Path "$ff\lib\libavcodec.a") {
        _cp "$ff\lib\*"
        Copy-Item -Recurse -Force "$ff\include\*" "$DepsDst\include\" 2>$null
        Write-Host "  prebuilt"
    } else { Write-Host "  skipped (no prebuilt)" }

    # --- plutovg ---
    Write-Host "[plutovg]"
    _git plutovg https://github.com/sammycage/plutovg.git v0.0.6
    _cmake plutovg ""

    # --- post-install ---
    $null = New-Item -ItemType Directory -Force -Path "$DepsDst\include\opus"
    Copy-Item "$DepsDst\include\opusfile.h" "$DepsDst\include\opus\" -Force -ErrorAction SilentlyContinue
    Copy-Item "$DepsDst\include\plutovg\plutovg.h" "$DepsDst\include\" -Force -ErrorAction SilentlyContinue

    "done" | Set-Content "$DepsDst\.stamp"
    Write-Host "All deps built."
}

function Invoke-Native {
    Write-Host "=== Native build ==="
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $ts = Get-Date -Format "yyyyMMdd_HHmmss"

    # Configure
    Write-Host "[1/2] CMake configure..."
    $cfgLog = "$LogDir\configure_${ts}.log"
    $null = & cmake -G Ninja -B $BuildDir `
        -DOHOS=TRUE -DOHOS_ARCH="$Arch" -DOHOS_STL="c++_shared" `
        -DCMAKE_BUILD_TYPE="$BuildType" `
        -DCMAKE_TOOLCHAIN_FILE="$Toolchain" `
        -DUSE_FFMPEG=ON -DUSE_SDL3=OFF -S $Root 2>&1 | Tee-Object -FilePath $cfgLog
    if ($LASTEXITCODE) { Write-Host "CONFIGURE FAILED �?$cfgLog"; throw }

    # Build
    Write-Host "[2/2] Compiling..."
    $buildLog = "$LogDir\build_${ts}.log"
    $null = & cmake --build $BuildDir --config $BuildType --parallel 2>&1 | Tee-Object -FilePath $buildLog
    if ($LASTEXITCODE) { Write-Host "BUILD FAILED �?$buildLog"; throw }

    $so = Get-ChildItem "$BuildDir\libkrkrsdl3.so" -ErrorAction SilentlyContinue
    if ($so) { Write-Host "OK: $($so.Name) $([math]::Round($so.Length/1MB,1))MB" }
}

function Invoke-Hap {
    Write-Host "=== Packaging HAP ==="
    $hapDir = "$BuildDir\hap"
    Remove-Item -Force -Recurse $hapDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path "$hapDir\libs\$Arch","$hapDir\resources\base\profile","$hapDir\resources\base\element","$hapDir\ets" | Out-Null

    # .so
    Copy-Item "$BuildDir\libkrkrsdl3.so" "$hapDir\libs\$Arch\" -ErrorAction SilentlyContinue

    # module.json
    $modJson = Get-Content "$Root\ohos\entry\src\main\module.json5" -Raw
    $modJson | Set-Content "$hapDir\module.json" -Force

    # resources
    Copy-Item "$Root\ohos\entry\src\main\resources\base\profile\main_pages.json" "$hapDir\resources\base\profile\" -Force
    @'
{ "color": [ { "name": "start_window_background", "value": "#000000" } ] }
'@ | Set-Content "$hapDir\resources\base\element\color.json" -Force
    @'
{ "string": [ { "name": "app_name", "value": "krkrsdl3" } ] }
'@ | Set-Content "$hapDir\resources\base\element\string.json" -Force
    @'
{ "summary": { "app": { "bundleName": "org.tvp.krkrsdl3", "version": { "code": 1, "name": "0.0.6" } } }, "packages": [ { "name": "entry", "type": "entry", "deviceType": ["default","tablet"] } ] }
'@ | Set-Content "$hapDir\pack.info" -Force

    # ZIP �?HAP
    $hapFile = "$BuildDir\krkrsdl3_${Arch}_${BuildType}.hap"
    if (Test-Path $hapFile) { Remove-Item $hapFile }
    $zipFile = "$BuildDir\krkrsdl3.zip"; Compress-Archive -Path "$hapDir\*" -DestinationPath $zipFile -Force; Move-Item $zipFile $hapFile -Force
    $info = Get-Item $hapFile
    Write-Host "HAP: $hapFile ($([math]::Round($info.Length/1KB,1))KB)"
}

function Invoke-Libs {
    Write-Host "=== Copying libs for IDE ==="
    $libDest = "$Root\ohos\entry\libs\$Arch"
    New-Item -ItemType Directory -Force -Path $libDest | Out-Null
    $so = "$BuildDir\libkrkrsdl3.so"
    if (Test-Path $so) {
        Copy-Item $so "$libDest\" -Force
        Write-Host "  $so -> $libDest\"
    } else {
        Write-Host "  ERROR: $so not found. Run 'native' first."
    }
}

# ================================================================
#  ENTRY
# ================================================================

Write-Host "=== krkrsdl3 OHOS ==="
Write-Host "    CMD: $Command | ARCH: $Arch | TYPE: $BuildType"
Write-Host "    SDK: $SDK | OUT: $BuildDir"

if (-not (Test-Path $SDK)) { Write-Host "ERROR: SDK not found at $SDK"; exit 1 }

try {
    switch ($Command) {
        "full"  { Invoke-Deps; Invoke-Native; Invoke-Libs; Invoke-Hap }
        "deps"  { Invoke-Deps }
        "native"{ Invoke-Native }
        "libs"  { Invoke-Libs }
        "hap"   { Invoke-Hap }
        "clean" { Remove-Item -Force -Recurse "$Root\out\ohos" -ErrorAction SilentlyContinue; Write-Host "Cleaned." }
    }
} catch {
    Write-Host "ERROR: $_"
    exit 1
}


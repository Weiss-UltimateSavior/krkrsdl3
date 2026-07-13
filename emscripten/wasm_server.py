#!/usr/bin/env python3
"""
WASM 开发服务器 — 自动添加 COOP/COEP 响应头，支持 pthreads SharedArrayBuffer。

用法:
    python script/wasm_server.py [端口号] [输出目录]

参数:
    端口号    可选，默认 5500
    输出目录  可选，指定服务目录（相对或绝对路径）
              也可通过环境变量 WASM_OUT_DIR 设置
              未指定时使用当前目录

示例:
    python script/wasm_server.py
    python script/wasm_server.py 5500
    python script/wasm_server.py 5500 out/emscripten/Debug
    set WASM_OUT_DIR=out/emscripten/Debug && python script/wasm_server.py

服务器会:
    1. 为所有 .html/.js/.wasm 等文件添加 COOP/COEP 头
    2. 切换到指定目录（默认当前目录）
    3. 显示访问地址
"""

import http.server
import os
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5500

# 确定输出目录：命令行参数 > 环境变量 > 当前目录
if len(sys.argv) > 2:
    OUT_DIR = sys.argv[2]
elif os.environ.get("WASM_OUT_DIR"):
    OUT_DIR = os.environ["WASM_OUT_DIR"]
else:
    OUT_DIR = os.getcwd()

OUT_DIR = os.path.abspath(OUT_DIR)

if not os.path.isdir(OUT_DIR):
    print(f"错误: 找不到目录 {OUT_DIR}")
    sys.exit(1)

os.chdir(OUT_DIR)
print(f"服务目录: {OUT_DIR}")


class WASMHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        # COOP/COEP — 必须，否则 pthreads 的 SharedArrayBuffer 不可用
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # 缓存控制
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        super().end_headers()

    def log_message(self, format, *args):
        # 简化日志输出
        print(f"[{self.log_date_time_string()}] {args[0]} {args[1]} {args[2]}")


if __name__ == '__main__':
    server = http.server.HTTPServer(('0.0.0.0', PORT), WASMHandler)
    print(f"WASM 开发服务器启动: http://localhost:{PORT}/krkrsdl3.html?game=data.xp3")
    print("COOP/COEP 头已启用 ✓")
    print("按 Ctrl+C 停止")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")
        server.server_close()

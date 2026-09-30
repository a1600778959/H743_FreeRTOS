# 按运行 Make 的主机选择原生 Python，WSL 属于 Linux，不切换到 Windows 进程。
# MAKE_HOST 来自 Make 自身，避免 WSL 继承 OS=Windows_NT 时误选 python.exe。
ifneq ($(findstring linux,$(MAKE_HOST)),)
PYTHON ?= python3
DIMA_DEFAULT_BUILD_DIR := build-linux
else
DIMA_USERPROFILE_POSIX := $(subst \,/,$(USERPROFILE))
DIMA_PLATFORMIO_PYTHON := $(DIMA_USERPROFILE_POSIX)/.platformio/penv/Scripts/python.exe
PYTHON ?= $(if $(wildcard $(DIMA_PLATFORMIO_PYTHON)),$(DIMA_PLATFORMIO_PYTHON),python.exe)
DIMA_DEFAULT_BUILD_DIR := build
endif

# 缓存目录始终由本机 Python 计算，Linux Make 的规则目标不能出现 Windows 盘符冒号。
ifeq ($(origin HOST_TOOLS_CACHE_ROOT),undefined)
# 目录属于本次 Make 求值的常量。递归变量会在每个生成/签名依赖展开时
# 反复启动 Python；立即求值只查询一次，显式环境/命令行覆盖保持有效。
HOST_TOOLS_CACHE_ROOT := $(shell $(PYTHON) -c "import pathlib; print((pathlib.Path.home() / '.cache' / 'dima-rover' / 'host-tools').as_posix())")
endif
# project.mk 的生成规则在解析时就展开前置条件，必须提前定义 stamp，
# 否则首次 Linux 构建会在安装 Cerberus 等正式依赖之前启动生成器。
# 外层调度 Make 已求值并导出时直接复用，避免每个嵌套 Make 重复启动
# Python 做同一份内容哈希。
ifeq ($(origin HOST_TOOLS_ID),undefined)
HOST_TOOLS_ID := $(shell $(PYTHON) tools/build_progress.py host-key)
endif
ifeq ($(strip $(HOST_TOOLS_ID)),)
$(error unable to identify host tool requirements and Python ABI)
endif
export HOST_TOOLS_ID
# 与旧版可整体替换的 host-python 并列，避免另一个旧会话重装时移走新环境。
HOST_PYTHON_DIR = $(HOST_TOOLS_CACHE_ROOT)/host-python-envs/$(HOST_TOOLS_ID)
HOST_TOOLS_STAMP = $(HOST_PYTHON_DIR)/.installed
export HOST_TOOLS_CACHE_ROOT

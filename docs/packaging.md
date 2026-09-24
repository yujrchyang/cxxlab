# cxxlab 打包策略

## 1. 总体原则

cxxlab 采用分层打包策略：

- **第三方依赖**（RocksDB、ISA-L、spdlog）：统一打包，安装到私有路径
- **cxxlab 核心库**：打包为独立 RPM，依赖第三方包
- **用户程序**：链接 cxxlab 库，不直接接触第三方依赖
- **测试框架**（Google Test）：仅构建时使用，不进入安装包

## 2. 第三方依赖打包策略

### 2.1 依赖清单

| 依赖 | 版本 | 用途 | 打包方式 |
| ------ | ------ | ------ | --------- |
| **RocksDB** | 7.10.2 | KV 存储引擎 | 动态库 ✅ |
| **ISA-L** | - | 存储加速（CRC、RAID） | 动态库 ✅ |
| **spdlog** | - | 日志框架（bundled fmt） | 动态库 ✅ |
| **Google Test** | - | 测试框架 | 不打包 ❌ |

### 2.2 为什么统一打包

| 问题 | 解决方案 |
| ------ | --------- |
| 系统可能已安装其他版本 | 使用独立包名 `cxxlab-thirdparty`，安装到私有路径 |
| 避免符号冲突 | 隔离到 `/usr/lib/cxxlab-thirdparty/` |
| 版本控制 | cxxlab 明确声明依赖的第三方库版本 |
| 独立更新 | 第三方库可单独升级，无需重新编译 cxxlab |
| 避免代码重复 | 多个 cxxlab 模块共享同一份第三方库 |

### 2.3 RPM 规格文件

```spec
# cxxlab-thirdparty.spec
Name:           cxxlab-thirdparty
Version:        1.0.0
Release:        1%{?dist}
Summary:        Third-party libraries for cxxlab
License:        Apache-2.0 AND GPL-2.0-only AND MIT
URL:            https://github.com/your-org/cxxlab
Source0:        cxxlab-thirdparty-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  zlib-devel
BuildRequires:  bzip2-devel
BuildRequires:  lz4-devel
BuildRequires:  zstd-devel
BuildRequires:  nasm

%description
Third-party libraries for cxxlab, including RocksDB, ISA-L, and spdlog.
These libraries are built as shared objects and installed to a private path
to avoid conflicts with system libraries.

%prep
%setup -q

%build
# RocksDB
mkdir -p rocksdb/build
cd rocksdb/build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=lib/cxxlab-thirdparty \
    -DWITH_GFLAGS=OFF \
    -DWITH_TESTS=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF \
    -DROCKSDB_BUILD_SHARED=ON \
    -DROCKSDB_BUILD_STATIC=OFF
make %{?_smp_mflags}
cd ../..

# ISA-L
cd isa-l
./autogen.sh
./configure --prefix=%{_prefix} --libdir=%{_prefix}/lib/cxxlab-thirdparty
make %{?_smp_mflags}
cd ..

# spdlog
mkdir -p spdlog/build
cd spdlog/build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=lib/cxxlab-thirdparty \
    -DSPDLOG_BUILD_SHARED=ON \
    -DSPDLOG_BUILD_STATIC=OFF \
    -DSPDLOG_FMT_EXTERNAL=OFF
make %{?_smp_mflags}
cd ../..

%install
# RocksDB
cd rocksdb/build
make install DESTDIR=%{buildroot}
cd ../..

# ISA-L
cd isa-l
make install DESTDIR=%{buildroot}
cd ..

# spdlog
cd spdlog/build
make install DESTDIR=%{buildroot}
cd ../..

# 创建 ldconfig 配置
mkdir -p %{buildroot}%{_sysconfdir}/ld.so.conf.d
echo "/usr/lib/cxxlab-thirdparty" > %{buildroot}%{_sysconfdir}/ld.so.conf.d/cxxlab-thirdparty.conf

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%files
%{_libdir}/cxxlab-thirdparty/librocksdb.so*
%{_libdir}/cxxlab-thirdparty/libisal.so*
%{_libdir}/cxxlab-thirdparty/libspdlog.so*
%config(noreplace) %{_sysconfdir}/ld.so.conf.d/cxxlab-thirdparty.conf
%doc README.md
%license LICENSE

%changelog
* Thu Sep 25 2025 cxxlab Team - 1.0.0-1
- Initial package for cxxlab
- Includes RocksDB 7.10.2, ISA-L, and spdlog
```

### 2.4 构建命令

```bash
# 构建第三方依赖 RPM
rpmbuild -bb cxxlab-thirdparty.spec

# 生成文件
# cxxlab-thirdparty-1.0.0-1.x86_64.rpm
# cxxlab-thirdparty-devel-1.0.0-1.x86_64.rpm  (如果需要开发包)
```

## 3. cxxlab 打包策略

### 3.1 RPM 规格文件

```spec
# cxxlab.spec
Name:           cxxlab
Version:        1.0.0
Release:        1%{?dist}
Summary:        High-performance storage engine based on Ceph BlueStore
License:        Apache-2.0
URL:            https://github.com/your-org/cxxlab
Source0:        cxxlab-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  cxxlab-thirdparty-devel = 1.0.0

Requires:       cxxlab-thirdparty = 1.0.0

%description
cxxlab is a storage engine implementation based on Ceph's BlueStore architecture,
providing high-performance key-value storage with direct block device access.

%prep
%setup -q

%build
mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib/cxxlab \
    -DCMAKE_INSTALL_RPATH="/usr/lib/cxxlab-thirdparty" \
    -DBUILD_SHARED_LIBS=ON

make %{?_smp_mflags}

%install
cd build
make install DESTDIR=%{buildroot}

# 创建 ldconfig 配置
mkdir -p %{buildroot}%{_sysconfdir}/ld.so.conf.d
echo "/usr/lib/cxxlab" > %{buildroot}%{_sysconfdir}/ld.so.conf.d/cxxlab.conf

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%files
%{_libdir}/cxxlab/libcommon.so
%{_libdir}/cxxlab/libkv.so
%{_libdir}/cxxlab/libblk.so
%{_libdir}/cxxlab/libbluestore.so
%{_libdir}/cxxlab/libbtier.so
%{_bindir}/cxxlab-demo
%config(noreplace) %{_sysconfdir}/ld.so.conf.d/cxxlab.conf
%doc README.md
%license LICENSE

%package devel
Summary:        Development files for cxxlab
Requires:       %{name} = %{version}-%{release}

%description devel
Development files for cxxlab, including headers and static libraries.

%files devel
%{_includedir}/cxxlab/*.h
%{_libdir}/cxxlab/*.so

%changelog
* Thu Sep 25 2025 cxxlab Team - 1.0.0-1
- Initial package
```

### 3.2 构建命令

```bash
# 构建 cxxlab RPM
rpmbuild -bb cxxlab.spec

# 生成文件
# cxxlab-1.0.0-1.x86_64.rpm
# cxxlab-devel-1.0.0-1.x86_64.rpm
```

## 4. 目录结构

安装后的完整目录结构：

```plaintext
/usr/
├── lib/
│   ├── cxxlab/                          # cxxlab 库（私有路径）
│   │   ├── libcommon.so                 # 公共工具库（使用 spdlog）
│   │   ├── libkv.so                     # KV 抽象层（使用 RocksDB）
│   │   ├── libblk.so                    # 块设备抽象层
│   │   ├── libbluestore.so              # BlueStore 引擎（Phase 3）
│   │   └── libbtier.so                  # B-Tier 引擎
│   │
│   └── cxxlab-thirdparty/               # 第三方库（私有路径）
│       ├── librocksdb.so                # RocksDB 7.10.2
│       ├── librocksdb.so.7
│       ├── librocksdb.so.7.10.2
│       ├── libisal.so                   # Intel ISA-L
│       └── libspdlog.so                 # spdlog（bundled fmt）
│
├── include/
│   └── cxxlab/                          # cxxlab 头文件（可选）
│       ├── common/
│       ├── kv/
│       ├── blk/
│       ├── bluestore/
│       └── btier/
│
├── bin/
│   └── cxxlab-demo                      # 示例程序
│
└── etc/
    └── ld.so.conf.d/
        ├── cxxlab.conf                  # 包含 /usr/lib/cxxlab
        └── cxxlab-thirdparty.conf       # 包含 /usr/lib/cxxlab-thirdparty
```

## 5. RPATH 配置

### 5.1 CMakeLists.txt 配置

```cmake
# 顶层 CMakeLists.txt
set(CMAKE_INSTALL_RPATH "/usr/lib/cxxlab-thirdparty")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

# 对使用第三方库的目标设置
set_target_properties(kv bluestore btier PROPERTIES
    INSTALL_RPATH "/usr/lib/cxxlab-thirdparty"
)
```

### 5.2 验证 RPATH

```bash
# 检查二进制文件的 RPATH
readelf -d /usr/lib/cxxlab/libkv.so | grep RPATH

# 检查运行时库加载
ldd /usr/lib/cxxlab/libkv.so

# 检查库搜索路径
LD_DEBUG=libs /usr/bin/cxxlab-demo 2>&1 | grep librocksdb
```

## 6. 用户安装流程

### 6.1 安装步骤

```bash
# 1. 添加 cxxlab 仓库（如果使用 yum/dnf）
sudo dnf config-manager --add-repo https://repo.cxxlab.org/cxxlab.repo

# 2. 安装 cxxlab（自动拉取依赖）
sudo dnf install cxxlab

# 3. 验证安装
cxxlab-demo --version
```

### 6.2 依赖解析

```plaintext
cxxlab-1.0.0-1.x86_64.rpm
  └── Requires: cxxlab-thirdparty = 1.0.0
        └── cxxlab-thirdparty-1.0.0-1.x86_64.rpm
              ├── RocksDB 7.10.2
              ├── ISA-L
              └── spdlog
```

### 6.3 卸载

```bash
# 卸载 cxxlab（保留第三方依赖）
sudo dnf remove cxxlab

# 完全卸载（包括第三方依赖）
sudo dnf remove cxxlab cxxlab-thirdparty
```

## 7. 开发包使用

### 7.1 安装开发包

```bash
sudo dnf install cxxlab-devel cxxlab-thirdparty-devel
```

### 7.2 编译用户程序

```cmake
# CMakeLists.txt
find_package(cxxlab REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE cxxlab::kv cxxlab::bluestore)
```

或者使用 pkg-config：

```bash
g++ -o myapp main.cpp $(pkg-config --cflags --libs cxxlab)
```

## 8. 版本管理

### 8.1 版本对应关系

| cxxlab 版本 | cxxlab-thirdparty 版本 | 包含的第三方库 | 说明 |
| ------------- | ------------------------ | ---------------- | ------ |
| 1.0.0 | 1.0.0 | RocksDB 7.10.2, ISA-L, spdlog | 初始版本 |
| 1.1.0 | 1.0.0 | 同上 | 兼容版本 |
| 1.2.0 | 1.1.0 | RocksDB 7.10.2, ISA-L, spdlog 1.x | 升级 spdlog |
| 2.0.0 | 2.0.0 | RocksDB 8.0.0, ISA-L, spdlog 2.x | 大版本升级 |

### 8.2 升级策略

```bash
# 升级 cxxlab（如果第三方依赖版本不变）
sudo dnf upgrade cxxlab

# 如果新版本需要新的第三方依赖
sudo dnf upgrade cxxlab cxxlab-thirdparty

# 单独升级第三方依赖（如果 cxxlab 兼容）
sudo dnf upgrade cxxlab-thirdparty
```

## 9. 调试和故障排除

### 9.1 库加载问题

```bash
# 检查库是否安装
rpm -qa | grep cxxlab

# 检查 ldconfig 缓存
ldconfig -p | grep cxxlab

# 刷新 ldconfig 缓存
sudo ldconfig

# 手动设置 LD_LIBRARY_PATH（临时）
export LD_LIBRARY_PATH=/usr/lib/cxxlab:/usr/lib/cxxlab-thirdparty:$LD_LIBRARY_PATH
```

### 9.2 符号冲突

```bash
# 检查符号定义
nm -D /usr/lib/cxxlab/libkv.so | grep rocksdb

# 检查依赖关系
ldd /usr/lib/cxxlab/libkv.so
```

## 10. 总结

cxxlab 的打包策略核心要点：

1. **第三方库统一打包**：RocksDB、ISA-L、spdlog 统一打包到 `/usr/lib/cxxlab-thirdparty/`
2. **RPATH 定向**：cxxlab 库通过 RPATH 指向私有路径，避免系统库污染
3. **版本绑定**：cxxlab 明确声明依赖的第三方库版本
4. **ldconfig 集成**：通过 `/etc/ld.so.conf.d/` 注册库路径
5. **开发包分离**：提供 `-devel` 包供二次开发使用
6. **测试框架隔离**：Google Test 仅在构建时使用，不进入安装包

这种策略确保了：

- ✅ 版本隔离，不干扰系统
- ✅ 依赖清晰，便于维护
- ✅ 符合 Linux 发行包规范
- ✅ 用户安装简单，自动解析依赖
- ✅ 第三方库可独立升级
- ✅ 避免多个 cxxlab 模块重复包含相同代码

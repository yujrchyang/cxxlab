#include <gtest/gtest.h>

#include <type_traits>

#include "common/cpu.h"

using namespace TOPNSPC;

TEST(CpuTest, ProbeRunsWithoutCrash) {
    EXPECT_NO_FATAL_FAILURE(cpu::probe());
}

TEST(CpuTest, ProbeIsIdempotent) {
    EXPECT_NO_FATAL_FAILURE(cpu::probe());
    EXPECT_NO_FATAL_FAILURE(cpu::probe());
    EXPECT_NO_FATAL_FAILURE(cpu::probe());
}

#if defined(__x86_64__)
TEST(CpuX86Test, Sse2IsBaseline) {
    cpu::probe();
    EXPECT_TRUE(cpu::features::intel_sse2);
}

TEST(CpuX86Test, AtLeastOneFeatureEnabled) {
    cpu::probe();
    bool any = cpu::features::intel_pclmul || cpu::features::intel_sse42 || cpu::features::intel_sse41 || cpu::features::intel_ssse3 || cpu::features::intel_sse3 || cpu::features::intel_sse2 || cpu::features::intel_aesni;
    EXPECT_TRUE(any);
}

TEST(CpuX86Test, AesniIsStableAfterProbe) {
    cpu::probe();
    bool v1 = cpu::features::intel_aesni;
    cpu::probe();
    bool v2 = cpu::features::intel_aesni;
    EXPECT_EQ(v1, v2);
}
#elif defined(__aarch64__)
TEST(CpuAarch64Test, NeonIsBaseline) {
    cpu::probe();
    EXPECT_TRUE(cpu::features::neon);
}

TEST(CpuAarch64Test, Crc32IsStableAfterProbe) {
    cpu::probe();
    bool v1 = cpu::features::aarch64_crc32;
    cpu::probe();
    bool v2 = cpu::features::aarch64_crc32;
    EXPECT_EQ(v1, v2);
}
#elif defined(__arm__)
TEST(CpuArmTest, NeonIsStableAfterProbe) {
    cpu::probe();
    bool v1 = cpu::features::neon;
    cpu::probe();
    bool v2 = cpu::features::neon;
    EXPECT_EQ(v1, v2);
}
#endif

TEST(CpuTest, FeaturesAreAccessibleAfterStaticInit) {
    // 验证静态初始化后所有特性字段可访问且为有效 bool 值
    EXPECT_NO_FATAL_FAILURE({
        bool any = cpu::features::intel_pclmul || cpu::features::intel_sse42 ||
                   cpu::features::intel_sse41 || cpu::features::intel_ssse3 ||
                   cpu::features::intel_sse3 || cpu::features::intel_sse2 ||
                   cpu::features::intel_aesni || cpu::features::neon ||
                   cpu::features::aarch64_crc32 || cpu::features::aarch64_pmull;
        (void)any;
    });
}

TEST(CpuTest, AllFeaturesAreBool) {
    static_assert(std::is_same_v<decltype(cpu::features::intel_pclmul), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::intel_sse42), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::intel_sse41), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::intel_ssse3), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::intel_sse3), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::intel_sse2), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::intel_aesni), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::neon), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::aarch64_crc32), bool>);
    static_assert(std::is_same_v<decltype(cpu::features::aarch64_pmull), bool>);
}
